using System.Buffers.Binary;
using System.Net.Sockets;

namespace Gateway.Protocols;

public readonly record struct FinsAddress(byte Area, ushort Word, byte Bit, ushort Count)
{
    public static FinsAddress Parse(string address, TagType type)
    {
        var split = address.Split(':');
        if (split.Length != 2) throw new ArgumentException("FINS address must be D:100, CIO:10, W:0, H:0 or A:0; BOOL requires e.g. CIO:10.3.");
        var parts = split[1].Split('.');
        if (!ushort.TryParse(parts[0], out var word) || parts.Length > 2) throw new ArgumentException("Invalid FINS word.");
        byte bit = 0;
        var isBit = type == TagType.Bool;
        if (isBit && (parts.Length != 2 || !byte.TryParse(parts[1], out bit) || bit > 15) || !isBit && parts.Length != 1)
            throw new ArgumentException("BOOL needs a bit 0–15; numeric tags need a word address.");
        byte area = split[0].ToUpperInvariant() switch
        {
            "D" => (byte)(isBit ? 0x02 : 0x82), "CIO" => (byte)(isBit ? 0x30 : 0xb0),
            "W" => (byte)(isBit ? 0x31 : 0xb1), "H" => (byte)(isBit ? 0x32 : 0xb2),
            "A" => (byte)(isBit ? 0x33 : 0xb3), _ => throw new ArgumentException("Unsupported FINS memory area.")
        };
        ushort count = type switch { TagType.Int32 or TagType.UInt32 or TagType.Float32 => 2, TagType.Float64 => 4, _ => 1 };
        if (word + count > 65536) throw new ArgumentException("FINS read exceeds address range.");
        return new(area, word, bit, count);
    }
}
public static class FinsCodec
{
    public static byte[] ReadRequest(ControllerOptions c, TagOptions tag, byte sid)
    {
        var a = FinsAddress.Parse(tag.Address, tag.Type);
        byte[] b = [0x80, 0, 2, c.DestinationNetwork, c.DestinationNode, c.DestinationUnit,
            c.SourceNetwork, c.SourceNode, c.SourceUnit, sid, 1, 1, a.Area, 0, 0, a.Bit, 0, 0];
        BinaryPrimitives.WriteUInt16BigEndian(b.AsSpan(13), a.Word);
        BinaryPrimitives.WriteUInt16BigEndian(b.AsSpan(16), a.Count);
        return b;
    }
    public static double Decode(ReadOnlySpan<byte> response, ControllerOptions c, TagOptions tag, byte sid)
    {
        var address = FinsAddress.Parse(tag.Address, tag.Type);
        var size = tag.Type == TagType.Bool ? 1 : address.Count * 2;
        if (response.Length != 14 + size || (response[0] & 0xc0) != 0xc0 || response[9] != sid || response[10] != 1 || response[11] != 1 ||
            response[3] != c.SourceNetwork || response[4] != c.SourceNode || response[5] != c.SourceUnit ||
            response[6] != c.DestinationNetwork || response[7] != c.DestinationNode || response[8] != c.DestinationUnit)
            throw new IOException("Invalid or mismatched FINS reply.");
        var status = BinaryPrimitives.ReadUInt16BigEndian(response[12..]);
        if (status != 0) throw new IOException($"FINS end code 0x{status:X4}.");
        var data = response[14..];
        if (tag.Type == TagType.Bool) return data[0] switch { 0 => 0, 1 => 1, _ => throw new IOException("Invalid FINS bit value.") };
        Span<byte> ordered = stackalloc byte[size];
        for (var i = 0; i < address.Count; i++)
        {
            var source = c.LowWordFirst ? address.Count - 1 - i : i;
            data.Slice(source * 2, 2).CopyTo(ordered.Slice(i * 2, 2));
        }
        return tag.Type switch
        {
            TagType.Int16 => BinaryPrimitives.ReadInt16BigEndian(ordered), TagType.UInt16 => BinaryPrimitives.ReadUInt16BigEndian(ordered),
            TagType.Int32 => BinaryPrimitives.ReadInt32BigEndian(ordered), TagType.UInt32 => BinaryPrimitives.ReadUInt32BigEndian(ordered),
            TagType.Float32 => BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32BigEndian(ordered)),
            TagType.Float64 => BitConverter.Int64BitsToDouble(BinaryPrimitives.ReadInt64BigEndian(ordered)), _ => throw new IOException("Unsupported type.")
        };
    }
}
public sealed class FinsClient(ControllerOptions options) : IControllerClient
{
    private UdpClient? socket;
    private byte sid;
    public async Task<double> ReadAsync(TagOptions tag, CancellationToken token)
    {
        socket ??= Connect();
        var id = ++sid;
        var request = FinsCodec.ReadRequest(options, tag, id);
        try
        {
            await socket.SendAsync(request, token);
            while (true)
            {
                var reply = await socket.ReceiveAsync(token);
                // UDP may deliver a late response from the preceding transaction.
                if (reply.Buffer.Length >= 10 && reply.Buffer[9] != id) continue;
                return FinsCodec.Decode(reply.Buffer, options, tag, id);
            }
        }
        catch { socket.Dispose(); socket = null; throw; }
    }
    public async Task<IReadOnlyList<TagRead>> ReadBatchAsync(TagOptions[] tags, CancellationToken token)
    {
        var first = tags[0];
        var start = ReadPlan.Position(first);
        var end = tags.Max(t => ReadPlan.Position(t) + FinsAddress.Parse(t.Address, t.Type).Count);
        var bitArea = first.Type == TagType.Bool;
        var length = (end - start) * (bitArea ? 1 : 2);
        var id = ++sid;
        var request = FinsCodec.ReadRequest(options, first, id);
        BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(16), checked((ushort)(end - start)));
        socket ??= Connect();
        try
        {
            await socket.SendAsync(request, token);
            byte[] response;
            while (true)
            {
                response = (await socket.ReceiveAsync(token)).Buffer;
                if (response.Length >= 10 && response[9] != id) continue;
                break;
            }
            if (response.Length < 14) throw new IOException("Truncated FINS response.");
            var status = BinaryPrimitives.ReadUInt16BigEndian(response.AsSpan(12));
            if (status != 0) throw new IOException($"FINS end code 0x{status:X4}.");
            if (response.Length != 14 + length) throw new IOException("Invalid FINS batch length.");
            var results = new List<TagRead>(tags.Length);
            foreach (var tag in tags)
            {
                var a = FinsAddress.Parse(tag.Address, tag.Type);
                var size = a.Count * (bitArea ? 1 : 2);
                var slice = new byte[14 + size]; response.AsSpan(0, 14).CopyTo(slice);
                response.AsSpan(14 + (ReadPlan.Position(tag) - start) * (bitArea ? 1 : 2), size).CopyTo(slice.AsSpan(14));
                try { results.Add(new(tag, FinsCodec.Decode(slice, options, tag, id))); }
                catch (IOException ex) { results.Add(new(tag, null, ex.Message)); }
            }
            return results;
        }
        catch { socket.Dispose(); socket = null; throw; }
    }
    private UdpClient Connect() { var s = new UdpClient(); s.Connect(options.Host, options.Port); return s; }
    public ValueTask DisposeAsync() { socket?.Dispose(); return ValueTask.CompletedTask; }
}
