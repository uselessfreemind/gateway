using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using Gateway.Protocols;
using Xunit;

namespace Gateway.Tests;

public class ProtocolTests
{
    [Theory]
    [InlineData("D:100", TagType.Float32, 0x82, 2)]
    [InlineData("CIO:10.3", TagType.Bool, 0x30, 1)]
    [InlineData("W:0", TagType.UInt32, 0xb1, 2)]
    public void FinsAddressAndReadOnlyOpcode(string address, TagType type, int area, int count)
    {
        var tag = new TagOptions { Address = address, Type = type };
        var request = FinsCodec.ReadRequest(new(), tag, 7);
        Assert.Equal(new byte[] { 1, 1 }, request[10..12]);
        Assert.Equal(area, request[12]); Assert.Equal(count, BinaryPrimitives.ReadUInt16BigEndian(request.AsSpan(16)));
    }
    [Theory]
    [InlineData("D:65535", TagType.Float32)]
    [InlineData("D:1.16", TagType.Bool)]
    [InlineData("D:1.0", TagType.UInt16)]
    [InlineData("BAD:1", TagType.UInt16)]
    public void FinsRejectsInvalidAddresses(string address, TagType type) => Assert.Throws<ArgumentException>(() => FinsAddress.Parse(address, type));

    private static byte[] FinsReply(ControllerOptions c, TagOptions tag, byte sid, byte[] data)
    {
        var request = FinsCodec.ReadRequest(c, tag, sid);
        byte[] reply = new byte[14 + data.Length]; request.AsSpan(0, 12).CopyTo(reply); reply[0] = 0xc0;
        request.AsSpan(6, 3).CopyTo(reply.AsSpan(3)); request.AsSpan(3, 3).CopyTo(reply.AsSpan(6)); data.CopyTo(reply, 14); return reply;
    }
    [Fact]
    public void FinsWordOrderAndCorrelation()
    {
        var c = new ControllerOptions(); var tag = new TagOptions { Address = "D:0", Type = TagType.Float32 };
        var reply = FinsReply(c, tag, 7, [0, 0, 0x3f, 0x80]);
        Assert.Equal(1d, FinsCodec.Decode(reply, c, tag, 7));
        Assert.Throws<IOException>(() => FinsCodec.Decode(reply, c, tag, 8));
        reply[12] = 0x11; Assert.Throws<IOException>(() => FinsCodec.Decode(reply, c, tag, 7));
    }
    [Fact]
    public async Task FinsClientGroupsContiguousReads()
    {
        using var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        var c = new ControllerOptions { Port = ((IPEndPoint)server.Client.LocalEndPoint!).Port };
        var tags = Enumerable.Range(0, 700).Select(i => new TagOptions { Id = $"t{i}", Address = $"D:{i}", Type = TagType.UInt16 }).ToArray();
        c.Tags = tags.ToList(); var plan = ReadPlan.Create(c); Assert.Equal(2, plan.Count);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var peer = Task.Run(async () =>
        {
            foreach (var batch in plan)
            {
                var packet = await server.ReceiveAsync(timeout.Token);
                Assert.Equal(new byte[] { 1, 1 }, packet.Buffer[10..12]);
                int start = BinaryPrimitives.ReadUInt16BigEndian(packet.Buffer.AsSpan(13));
                int count = BinaryPrimitives.ReadUInt16BigEndian(packet.Buffer.AsSpan(16));
                var data = new byte[count * 2];
                for (var i = 0; i < count; i++) BinaryPrimitives.WriteUInt16BigEndian(data.AsSpan(i * 2), (ushort)(start + i));
                var reply = FinsReply(c, batch[0], packet.Buffer[9], data);
                await server.SendAsync(reply, packet.RemoteEndPoint, timeout.Token);
            }
        });
        await using var client = new FinsClient(c);
        var values = new List<TagRead>();
        foreach (var batch in plan) values.AddRange(await client.ReadBatchAsync(batch, timeout.Token));
        await peer;
        Assert.Equal(700, values.Count); Assert.Equal(699d, values[^1].Value); Assert.All(values, v => Assert.Null(v.Error));
    }
    [Fact]
    public void CipPathAndTypedValues()
    {
        var request = CipCodec.ReadRequest("Machine.Temperature[300]");
        Assert.Equal(0x4c, request[0]); Assert.Contains((byte)0x29, request);
        Assert.Equal(new byte[] { 1, 0 }, request[^2..]);
        Assert.Equal(1d, CipCodec.Decode(new byte[] { 0xcc, 0, 0, 0, 0xca, 0, 0, 0, 0x80, 0x3f }, TagType.Float32));
        Assert.Throws<IOException>(() => CipCodec.Decode(new byte[] { 0xcc, 0, 5, 0 }, TagType.Float32));
        Assert.Throws<IOException>(() => CipCodec.Decode(new byte[] { 0xcc, 0, 0, 0, 0xc3, 0, 1, 0 }, TagType.Float32));
        Assert.Throws<ArgumentException>(() => CipCodec.SymbolPath("Tag;Write(1)"));
    }
    [Fact]
    public async Task EtherNetIpSessionAndMultipleServiceReads()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0); listener.Start();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        var peer = Task.Run(async () =>
        {
            using var socket = await listener.AcceptTcpClientAsync(timeout.Token); var stream = socket.GetStream();
            for (var turn = 0; turn < 2; turn++)
            {
                var header = new byte[24]; await stream.ReadExactlyAsync(header, timeout.Token);
                var body = new byte[BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2))]; await stream.ReadExactlyAsync(body, timeout.Token);
                byte[] response;
                if (turn == 0) { Assert.Equal(0x65, BinaryPrimitives.ReadUInt16LittleEndian(header)); response = [1, 0, 0, 0]; }
                else
                {
                    Assert.Equal(0x6f, BinaryPrimitives.ReadUInt16LittleEndian(header));
                    Assert.Equal(0x0a, body[16]);
                    Assert.Equal(2, BinaryPrimitives.ReadUInt16LittleEndian(body.AsSpan(22)));
                    // Two independently typed embedded Read Tag replies.
                    byte[] cip = [0x8a, 0, 0, 0, 2, 0, 6, 0, 14, 0, 0xcc, 0, 0, 0, 0xc7, 0, 42, 0, 0xcc, 0, 0, 0, 0xc7, 0, 43, 0];
                    response = new byte[16 + cip.Length]; BinaryPrimitives.WriteUInt16LittleEndian(response.AsSpan(6), 2);
                    BinaryPrimitives.WriteUInt16LittleEndian(response.AsSpan(12), 0xb2); BinaryPrimitives.WriteUInt16LittleEndian(response.AsSpan(14), (ushort)cip.Length); cip.CopyTo(response, 16);
                }
                BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(2), (ushort)response.Length);
                BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4), 123);
                await stream.WriteAsync(header, timeout.Token); await stream.WriteAsync(response, timeout.Token);
            }
        });
        try
        {
            await using var client = new EtherNetIpClient(new() { Port = ((IPEndPoint)listener.LocalEndpoint).Port });
            var result = await client.ReadBatchAsync([new() { Address = "TagA" }, new() { Address = "TagB" }], timeout.Token);
            Assert.Equal(new double?[] { 42, 43 }, result.Select(r => r.Value)); await peer;
        }
        finally { listener.Stop(); }
    }
    [Fact]
    public async Task FinsTimeoutIsCancelable()
    {
        using var server = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        await using var client = new FinsClient(new() { Port = ((IPEndPoint)server.Client.LocalEndPoint!).Port });
        using var timeout = new CancellationTokenSource(100);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => client.ReadAsync(new() { Address = "D:0" }, timeout.Token));
    }
}
