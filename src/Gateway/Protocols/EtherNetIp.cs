using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace Gateway.Protocols;

public interface IControllerClient : IAsyncDisposable
{
    Task<double> ReadAsync(TagOptions tag, CancellationToken token);
    Task<IReadOnlyList<TagRead>> ReadBatchAsync(TagOptions[] tags, CancellationToken token);
}
public static partial class CipCodec
{
    [GeneratedRegex(@"^([A-Za-z_][A-Za-z0-9_]*)(?:\[([0-9]+)\])?$")]
    private static partial Regex SegmentPattern();
    public static byte[] SymbolPath(string address)
    {
        var bytes = new List<byte>();
        foreach (var segment in address.Split('.'))
        {
            var m = SegmentPattern().Match(segment);
            if (!m.Success) throw new ArgumentException("CIP address must be a symbolic tag, member, or one-dimensional array element.");
            var name = Encoding.ASCII.GetBytes(m.Groups[1].Value);
            if (name.Length > 255) throw new ArgumentException("CIP symbol too long.");
            bytes.Add(0x91); bytes.Add((byte)name.Length); bytes.AddRange(name);
            if (name.Length % 2 != 0) bytes.Add(0);
            if (m.Groups[2].Success)
            {
                if (!uint.TryParse(m.Groups[2].Value, out var index)) throw new ArgumentException("CIP array index too large.");
                if (index <= 255) { bytes.Add(0x28); bytes.Add((byte)index); }
                else if (index <= 65535) { bytes.AddRange([0x29, 0, (byte)index, (byte)(index >> 8)]); }
                else { bytes.AddRange([0x2a, 0, (byte)index, (byte)(index >> 8), (byte)(index >> 16), (byte)(index >> 24)]); }
            }
        }
        if (bytes.Count > 510) throw new ArgumentException("CIP path exceeds 255 words.");
        return bytes.ToArray();
    }
    public static byte[] ReadRequest(string address)
    {
        var path = SymbolPath(address);
        return [(byte)0x4c, (byte)(path.Length / 2), .. path, 1, 0];
    }
    public static double Decode(ReadOnlySpan<byte> reply, TagType type)
    {
        if (reply.Length < 4 || reply[0] != 0xcc) throw new IOException("Invalid CIP read response.");
        if (reply[2] != 0) throw new IOException($"CIP general status 0x{reply[2]:X2}.");
        var offset = 4 + reply[3] * 2;
        if (reply.Length < offset + 2) throw new IOException("Truncated CIP type.");
        var wireType = BinaryPrimitives.ReadUInt16LittleEndian(reply[offset..]);
        var expected = type switch { TagType.Bool => 0xc1, TagType.Int16 => 0xc3, TagType.UInt16 => 0xc7, TagType.Int32 => 0xc4, TagType.UInt32 => 0xc8, TagType.Float32 => 0xca, TagType.Float64 => 0xcb, _ => -1 };
        if (wireType != expected) throw new IOException($"CIP type 0x{wireType:X} does not match {type}.");
        var data = reply[(offset + 2)..];
        int size = type switch { TagType.Bool => 1, TagType.Int16 or TagType.UInt16 => 2, TagType.Float64 => 8, _ => 4 };
        if (data.Length != size) throw new IOException("Invalid CIP value length.");
        return type switch
        {
            TagType.Bool => data[0] == 0 ? 0 : 1, TagType.Int16 => BinaryPrimitives.ReadInt16LittleEndian(data),
            TagType.UInt16 => BinaryPrimitives.ReadUInt16LittleEndian(data), TagType.Int32 => BinaryPrimitives.ReadInt32LittleEndian(data),
            TagType.UInt32 => BinaryPrimitives.ReadUInt32LittleEndian(data),
            TagType.Float32 => BitConverter.Int32BitsToSingle(BinaryPrimitives.ReadInt32LittleEndian(data)),
            TagType.Float64 => BitConverter.Int64BitsToDouble(BinaryPrimitives.ReadInt64LittleEndian(data)), _ => throw new IOException("Unsupported CIP type.")
        };
    }
}
// Unconnected explicit messaging: RegisterSession + SendRRData + CIP Read Tag (0x4C).
// No PLC write, mode change, program download, or implicit I/O connection is exposed.
public sealed class EtherNetIpClient(ControllerOptions options) : IControllerClient
{
    private TcpClient? client;
    private uint session;
    private ulong context;
    private async Task<byte[]> Exchange(ushort command, byte[] payload, CancellationToken token)
    {
        var stream = client!.GetStream();
        var tx = new byte[24 + payload.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(tx, command);
        BinaryPrimitives.WriteUInt16LittleEndian(tx.AsSpan(2), checked((ushort)payload.Length));
        BinaryPrimitives.WriteUInt32LittleEndian(tx.AsSpan(4), session);
        var correlation = ++context;
        BinaryPrimitives.WriteUInt64LittleEndian(tx.AsSpan(12), correlation);
        payload.CopyTo(tx, 24);
        await stream.WriteAsync(tx, token);
        var header = new byte[24];
        await stream.ReadExactlyAsync(header, token);
        var length = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2));
        if (BinaryPrimitives.ReadUInt16LittleEndian(header) != command || BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8)) != 0 ||
            BinaryPrimitives.ReadUInt64LittleEndian(header.AsSpan(12)) != correlation || length > 8192)
            throw new IOException("Invalid EtherNet/IP encapsulation response.");
        var returnedSession = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4));
        if (command == 0x65) { if (returnedSession == 0) throw new IOException("Invalid session."); session = returnedSession; }
        else if (returnedSession != session) throw new IOException("Session mismatch.");
        var body = new byte[length];
        await stream.ReadExactlyAsync(body, token);
        return body;
    }
    private async Task<byte[]> SendCip(byte[] cip, CancellationToken token)
    {
        try
        {
            if (client == null)
            {
                client = new TcpClient { NoDelay = true };
                await client.ConnectAsync(options.Host, options.Port, token);
                var registered = await Exchange(0x65, [1, 0, 0, 0], token);
                if (!registered.SequenceEqual(new byte[] { 1, 0, 0, 0 })) throw new IOException("Invalid RegisterSession response.");
            }
            var cpf = new byte[16 + cip.Length];
            BinaryPrimitives.WriteUInt16LittleEndian(cpf.AsSpan(6), 2); // two CPF items
            BinaryPrimitives.WriteUInt16LittleEndian(cpf.AsSpan(12), 0xb2); // unconnected data
            BinaryPrimitives.WriteUInt16LittleEndian(cpf.AsSpan(14), checked((ushort)cip.Length));
            cip.CopyTo(cpf, 16);
            var response = await Exchange(0x6f, cpf, token);
            if (response.Length < 8 || BinaryPrimitives.ReadUInt32LittleEndian(response) != 0) throw new IOException("Invalid CPF interface.");
            var count = BinaryPrimitives.ReadUInt16LittleEndian(response.AsSpan(6));
            var pos = 8;
            byte[]? data = null;
            for (var i = 0; i < count; i++)
            {
                if (pos + 4 > response.Length) throw new IOException("Truncated CPF item.");
                var id = BinaryPrimitives.ReadUInt16LittleEndian(response.AsSpan(pos));
                var len = BinaryPrimitives.ReadUInt16LittleEndian(response.AsSpan(pos + 2)); pos += 4;
                if (pos + len > response.Length) throw new IOException("Truncated CPF payload.");
                if (id == 0xb2) { if (data != null) throw new IOException("Duplicate CPF data."); data = response.AsSpan(pos, len).ToArray(); }
                pos += len;
            }
            if (pos != response.Length || data == null) throw new IOException("Missing CPF data.");
            return data;
        }
        catch { client?.Dispose(); client = null; session = 0; throw; }
    }
    public async Task<double> ReadAsync(TagOptions tag, CancellationToken token) => CipCodec.Decode(await SendCip(CipCodec.ReadRequest(tag.Address), token), tag.Type);
    public async Task<IReadOnlyList<TagRead>> ReadBatchAsync(TagOptions[] tags, CancellationToken token)
    {
        if (tags.Length == 1) return [new(tags[0], await ReadAsync(tags[0], token))];
        var requests = tags.Select(t => CipCodec.ReadRequest(t.Address)).ToArray();
        var count = tags.Length;
        var size = 2 + 2 * count + requests.Sum(r => r.Length);
        var body = new byte[6 + size];
        new byte[] { 0x0a, 2, 0x20, 2, 0x24, 1 }.CopyTo(body, 0);
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(6), (ushort)count);
        var offset = 2 + 2 * count;
        for (var i = 0; i < count; i++)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(8 + 2 * i), (ushort)offset);
            requests[i].CopyTo(body, 6 + offset); offset += requests[i].Length;
        }
        var response = await SendCip(body, token);
        if (response.Length < 6 || response[0] != 0x8a || response[2] is not (0 or 0x1e)) throw new IOException("Invalid CIP Multiple Service response; verify PLC support.");
        var start = 4 + response[3] * 2;
        if (response.Length < start + 2 + count * 2 || BinaryPrimitives.ReadUInt16LittleEndian(response.AsSpan(start)) != count)
            throw new IOException("Invalid CIP Multiple Service count.");
        var results = new List<TagRead>(count);
        for (var i = 0; i < count; i++)
        {
            int from = BinaryPrimitives.ReadUInt16LittleEndian(response.AsSpan(start + 2 + i * 2));
            int to = i + 1 < count ? BinaryPrimitives.ReadUInt16LittleEndian(response.AsSpan(start + 4 + i * 2)) : response.Length - start;
            if (from < 2 + count * 2 || to < from || start + to > response.Length) throw new IOException("Invalid CIP Multiple Service offsets.");
            try { results.Add(new(tags[i], CipCodec.Decode(response.AsSpan(start + from, to - from), tags[i].Type))); }
            catch (IOException ex) { results.Add(new(tags[i], null, ex.Message)); }
        }
        return results;
    }
    public ValueTask DisposeAsync() { client?.Dispose(); return ValueTask.CompletedTask; }
}
