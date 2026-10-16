using System.Buffers.Binary;

namespace Gateway.Secs;

public sealed record HsmsFrame(ushort Session, byte Stream, byte Function, bool ReplyExpected, byte SType, uint SystemBytes, byte[] Body)
{
    public byte[] Encode()
    {
        if (Body.Length > 1048576 - 10) throw new IOException("HSMS frame too large.");
        var b = new byte[14 + Body.Length];
        BinaryPrimitives.WriteUInt32BigEndian(b, (uint)(10 + Body.Length));
        BinaryPrimitives.WriteUInt16BigEndian(b.AsSpan(4), Session);
        b[6] = (byte)(Stream | (ReplyExpected ? 0x80 : 0)); b[7] = Function; b[9] = SType;
        BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(10), SystemBytes); Body.CopyTo(b, 14); return b;
    }
    public static async Task<HsmsFrame> ReadAsync(Stream stream, int t8Seconds, CancellationToken token)
    {
        var header = new byte[14];
        // Idle sessions have no T8 deadline until the first byte arrives.
        await stream.ReadExactlyAsync(header.AsMemory(0, 1), token);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token); timeout.CancelAfter(TimeSpan.FromSeconds(t8Seconds));
        await stream.ReadExactlyAsync(header.AsMemory(1, 3), timeout.Token);
        var size = BinaryPrimitives.ReadUInt32BigEndian(header);
        if (size is < 10 or > 1048576) throw new IOException("Invalid HSMS frame length.");
        await stream.ReadExactlyAsync(header.AsMemory(4, 10), timeout.Token);
        if (header[8] != 0) throw new IOException("Unsupported HSMS PType.");
        var body = new byte[size - 10]; await stream.ReadExactlyAsync(body, timeout.Token);
        return new(BinaryPrimitives.ReadUInt16BigEndian(header.AsSpan(4)), (byte)(header[6] & 0x7f), header[7], (header[6] & 0x80) != 0,
            header[9], BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(10)), body);
    }
    public static HsmsFrame Control(byte type, uint system, byte status = 0) => new(0xffff, 0, status, false, type, system, []);
}
