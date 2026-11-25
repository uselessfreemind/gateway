using System.Net;
using System.Net.Sockets;
using Gateway;
using Gateway.Acquisition;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;
namespace Gateway.Tests;
public class SamplingControlTests
{
    [Fact]
    public async Task SilentControllerAbortsAfterThreeRetriesAndManualRetryAndGlobalRestartResumeReads()
    {
        using var socket = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        using var stop = new CancellationTokenSource();
        var requests = 0;
        var respond = false;
        var simulator = Task.Run(async () =>
        {
            try
            {
                while (!stop.IsCancellationRequested)
                {
                    var packet = await socket.ReceiveAsync(stop.Token);
                    Interlocked.Increment(ref requests);
                    if (!Volatile.Read(ref respond)) continue;
                    var reply = packet.Buffer[..12].Concat(new byte[] { 0, 0, 0, 42 }).ToArray();
                    reply[0] = 0xc0;
                    Array.Copy(packet.Buffer, 6, reply, 3, 3);
                    Array.Copy(packet.Buffer, 3, reply, 6, 3);
                    await socket.SendAsync(reply, packet.RemoteEndPoint, stop.Token);
                }
            }
            catch (OperationCanceledException) { }
        });
        var options = new GatewayOptions { QueueCapacity = 1000, Controllers = [new ControllerOptions
        {
            Id = "plc", Host = "127.0.0.1", Port = ((IPEndPoint)socket.Client.LocalEndPoint!).Port,
            PollIntervalMs = 50, TimeoutMs = 50,
            Tags = [new TagOptions { Id = "pressure", Address = "D:0", Svid = 1, Type = TagType.UInt16 }]
        }] };
        var state = new DataState(); var control = new SamplingControl();
        using var service = new AcquisitionService(options, new SampleQueue(options), state, control, NullLogger<AcquisitionService>.Instance);
        async Task Until(Func<bool> predicate)
        {
            var deadline = DateTime.UtcNow.AddSeconds(10);
            while (!predicate()) { Assert.True(DateTime.UtcNow < deadline, "Timed out waiting for sampling state"); await Task.Delay(20); }
        }
        await service.StartAsync(default);
        try
        {
            await Until(() => control.Connections.TryGetValue("plc", out var s) && s.State == "Aborted");
            Assert.Equal(4, Volatile.Read(ref requests));
            Assert.Equal(3, control.Connections["plc"].RetryAttempts);
            await Task.Delay(250); Assert.Equal(4, Volatile.Read(ref requests));
            Volatile.Write(ref respond, true); control.Retry("plc");
            await Until(() => control.Connections["plc"].State == "Connected");
            Assert.Equal(0, control.Connections["plc"].RetryAttempts);
            control.RestartAll(["plc"]);
            await Until(() => control.Connections["plc"].State == "Connected");
            Assert.Equal(42d, state.Latest[1].Value);
        }
        finally { await service.StopAsync(default); stop.Cancel(); await simulator; }
    }
}
