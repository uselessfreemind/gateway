using System.Net;
using System.Net.Sockets;
using Gateway.Acquisition;
using Gateway.Secs;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace Gateway.Tests;

public class SecsTests
{
    [Fact]
    public void SecsItemsRoundTripNestedAndRejectTruncation()
    {
        var item = SecsItem.L(SecsItem.A("Omron"), SecsItem.U(1, 350000), SecsItem.F(12.5), SecsItem.L(SecsItem.B(0)));
        var bytes = item.Encode(); Assert.Equal(bytes, SecsItem.Decode(bytes).Encode());
        Assert.Throws<FormatException>(() => SecsItem.Decode(bytes[..^1]));
        Assert.Throws<FormatException>(() => SecsItem.Decode([0]));
        var nesting = new byte[40]; for (var i = 0; i < 40; i += 2) { nesting[i] = 1; nesting[i + 1] = 1; }
        Assert.Throws<FormatException>(() => SecsItem.Decode(nesting));
    }
    private static (GatewayOptions Options, DataState State) Fixture()
    {
        var options = new GatewayOptions { Controllers = [new() { Id = "nx", Tags = [new() { Id = "temperature", Address = "Temperature", Type = TagType.Float32, Svid = 100, Unit = "C" }] }] };
        var state = new DataState(); state.Latest[100] = new("nx", "temperature", 100, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), 23.5, "Good", null);
        return (options, state);
    }
    private static SecsItem Establish(GemModel model) => model.Handle(1, 13, SecsItem.L()).Body!;
    [Fact]
    public void GemEquipmentEstablishOnlineReportsAndEvents()
    {
        var (options, state) = Fixture(); var model = new GemModel(options, state);
        Assert.Throws<FormatException>(() => model.Handle(1, 3, SecsItem.L()));
        Assert.Equal(0, ((byte[])Establish(model).Items[0].Value)[0]);
        var values = model.Handle(1, 3, SecsItem.L(SecsItem.U(100), SecsItem.U(999))).Body!;
        Assert.Equal(23.5, ((double[])values.Items[0].Value)[0]); Assert.Empty(values.Items[1].Items);
        Assert.Null(model.CollectionEvent()); model.Handle(1, 17, null);
        var define = model.Handle(2, 33, SecsItem.L(SecsItem.U(0), SecsItem.L(SecsItem.L(SecsItem.U(1), SecsItem.L(SecsItem.U(100))))));
        Assert.Equal(0, ((byte[])define.Body!.Value)[0]);
        model.Handle(2, 35, SecsItem.L(SecsItem.U(0), SecsItem.L(SecsItem.L(SecsItem.U(1001), SecsItem.L(SecsItem.U(1))))));
        model.Handle(2, 37, SecsItem.L(new(0x24, new byte[] { 1 }), SecsItem.L(SecsItem.U(1001))));
        Assert.Equal(1001u, model.CollectionEvent()!.Items[1].UInt);
        model.Handle(1, 15, null); Assert.Null(model.CollectionEvent());
        Assert.Equal(1, ((byte[])model.Handle(2, 41, SecsItem.L()).Body!.Items[0].Value)[0]);
    }
    [Fact]
    public void GemNeverReturnsStaleOrBadValuesAsGood()
    {
        var (options, state) = Fixture(); var model = new GemModel(options, state); Establish(model);
        state.Latest[100] = state.Latest[100] with { TimestampMs = 0 };
        Assert.Empty(model.Handle(1, 3, SecsItem.L(SecsItem.U(100))).Body!.Items[0].Items);
        state.Latest[100] = state.Latest[100] with { TimestampMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), Quality = "Bad" };
        Assert.Empty(model.Handle(1, 3, SecsItem.L(SecsItem.U(100))).Body!.Items[0].Items);
    }
    [Fact]
    public async Task HsmsFramesHandleFragmentationAndRejectOversizedInput()
    {
        var frame = new HsmsFrame(123, 1, 3, true, 0, 42, SecsItem.L(SecsItem.U(100)).Encode());
        using var stream = new MemoryStream(frame.Encode());
        var decoded = await HsmsFrame.ReadAsync(stream, 1, CancellationToken.None);
        Assert.Equal(123, decoded.Session); Assert.Equal(42u, decoded.SystemBytes); Assert.True(decoded.ReplyExpected);
        Assert.Equal(frame.Body, decoded.Body);
        using var invalid = new MemoryStream(new byte[] { 0x7f, 0xff, 0xff, 0xff });
        await Assert.ThrowsAsync<IOException>(() => HsmsFrame.ReadAsync(invalid, 1, CancellationToken.None));
    }
    [Fact]
    public async Task HsmsPassiveSelectEstablishAndReadStatusOverTcp()
    {
        var (options, data) = Fixture();
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        using var lifetime = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var host = new TcpClient(); await host.ConnectAsync(IPAddress.Loopback, ((IPEndPoint)listener.LocalEndpoint).Port, lifetime.Token);
        using var equipment = await listener.AcceptTcpClientAsync(lifetime.Token); listener.Stop();
        var state = new SecsState();
        var run = new HsmsSession(equipment.GetStream(), options, data, state, NullLogger.Instance).RunAsync(lifetime.Token);
        var stream = host.GetStream();
        await stream.WriteAsync(HsmsFrame.Control(1, 10).Encode(), lifetime.Token);
        var selection = await HsmsFrame.ReadAsync(stream, 2, lifetime.Token);
        Assert.Equal(2, selection.SType); Assert.Equal(10u, selection.SystemBytes); Assert.Equal(0, selection.Function);
        await stream.WriteAsync(new HsmsFrame(0, 1, 13, true, 0, 11, SecsItem.L().Encode()).Encode(), lifetime.Token);
        var established = await HsmsFrame.ReadAsync(stream, 2, lifetime.Token); Assert.Equal(14, established.Function);
        await stream.WriteAsync(new HsmsFrame(0, 1, 3, true, 0, 12, SecsItem.L(SecsItem.U(100)).Encode()).Encode(), lifetime.Token);
        var status = await HsmsFrame.ReadAsync(stream, 2, lifetime.Token);
        Assert.Equal(4, status.Function); Assert.Equal(12u, status.SystemBytes);
        Assert.Equal(23.5, ((double[])SecsItem.Decode(status.Body).Items[0].Value)[0]);
        Assert.True(state.Selected); Assert.True(state.Communicating);
        async Task Request(byte streamNumber, byte function, uint system, SecsItem? body)
        {
            await stream.WriteAsync(new HsmsFrame(0, streamNumber, function, true, 0, system, body?.Encode() ?? []).Encode(), lifetime.Token);
            var response = await HsmsFrame.ReadAsync(stream, 2, lifetime.Token);
            Assert.Equal(function + 1, response.Function); Assert.Equal(system, response.SystemBytes);
        }
        await Request(1, 17, 13, null);
        await Request(2, 33, 14, SecsItem.L(SecsItem.U(0), SecsItem.L(SecsItem.L(SecsItem.U(1), SecsItem.L(SecsItem.U(100))))));
        await Request(2, 35, 15, SecsItem.L(SecsItem.U(0), SecsItem.L(SecsItem.L(SecsItem.U(1001), SecsItem.L(SecsItem.U(1))))));
        await Request(2, 37, 16, SecsItem.L(new(0x24, new byte[] { 1 }), SecsItem.L(SecsItem.U(1001))));
        var report = await HsmsFrame.ReadAsync(stream, 2, lifetime.Token);
        Assert.Equal(6, report.Stream); Assert.Equal(11, report.Function); Assert.True(report.ReplyExpected);
        var reportBody = SecsItem.Decode(report.Body);
        Assert.Equal(1001u, reportBody.Items[1].UInt);
        Assert.Equal(23.5, ((double[])reportBody.Items[2].Items[0].Items[1].Items[0].Value)[0]);
        await stream.WriteAsync(new HsmsFrame(0, 6, 12, false, 0, report.SystemBytes, SecsItem.B(0).Encode()).Encode(), lifetime.Token);
        lifetime.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => run);
    }
    [Fact]
    public async Task ActiveSelectionDoesNotLeaveT7ArmedAfterSelectResponse()
    {
        var (options, data) = Fixture(); options.Secs.Mode = "Active"; options.Secs.T7Seconds = 1;
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        using var lifetime = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var host = new TcpClient(); await host.ConnectAsync(IPAddress.Loopback, ((IPEndPoint)listener.LocalEndpoint).Port, lifetime.Token);
        using var equipment = await listener.AcceptTcpClientAsync(lifetime.Token); listener.Stop();
        var run = new HsmsSession(equipment.GetStream(), options, data, new(), NullLogger.Instance).RunAsync(lifetime.Token);
        var stream = host.GetStream();
        var selection = await HsmsFrame.ReadAsync(stream, 1, lifetime.Token); Assert.Equal(1, selection.SType);
        await stream.WriteAsync(HsmsFrame.Control(2, selection.SystemBytes).Encode(), lifetime.Token);
        await Task.Delay(1200, lifetime.Token);
        await stream.WriteAsync(HsmsFrame.Control(5, 99).Encode(), lifetime.Token);
        var linktest = await HsmsFrame.ReadAsync(stream, 1, lifetime.Token); Assert.Equal(6, linktest.SType); Assert.Equal(99u, linktest.SystemBytes);
        lifetime.Cancel(); await Assert.ThrowsAnyAsync<OperationCanceledException>(() => run);
    }

}
