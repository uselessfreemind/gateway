using System.Buffers.Binary;
using System.Text;

namespace Gateway.Secs;

public sealed record SecsItem(byte Format, object Value)
{
    public const byte ListFormat = 0x00, BinaryFormat = 0x20, AsciiFormat = 0x40, U4Format = 0xb0, F8Format = 0x80;
    public static SecsItem L(params SecsItem[] items) => new(ListFormat, items);
    public static SecsItem B(byte value) => new(BinaryFormat, new byte[] { value });
    public static SecsItem A(string value) => new(AsciiFormat, value);
    public static SecsItem U(params uint[] values) => new(U4Format, values);
    public static SecsItem F(double value) => new(F8Format, new double[] { value });
    public SecsItem[] Items => Format == ListFormat ? (SecsItem[])Value : throw new FormatException("Expected SECS list.");
    public uint UInt => Format == U4Format && ((uint[])Value).Length == 1 ? ((uint[])Value)[0] : throw new FormatException("Expected scalar U4.");
    public bool Flag => Format == 0x24 && ((byte[])Value).Length == 1 ? ((byte[])Value)[0] != 0 : throw new FormatException("Expected BOOLEAN.");
    public byte[] Encode()
    {
        byte[] payload;
        int length;
        switch (Format)
        {
            case ListFormat:
                var items = Items; length = items.Length;
                payload = items.SelectMany(x => x.Encode()).ToArray(); break;
            case AsciiFormat: payload = Encoding.ASCII.GetBytes((string)Value); length = payload.Length; break;
            case U4Format:
                var ints = (uint[])Value; payload = new byte[ints.Length * 4];
                for (var i = 0; i < ints.Length; i++) BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(i * 4), ints[i]);
                length = payload.Length; break;
            case F8Format:
                var floats = (double[])Value; payload = new byte[floats.Length * 8];
                for (var i = 0; i < floats.Length; i++) BinaryPrimitives.WriteInt64BigEndian(payload.AsSpan(i * 8), BitConverter.DoubleToInt64Bits(floats[i]));
                length = payload.Length; break;
            default: payload = (byte[])Value; length = payload.Length; break;
        }
        if (length > 0xffffff) throw new FormatException("SECS item too large.");
        int n = length <= 255 ? 1 : length <= 65535 ? 2 : 3;
        var output = new byte[1 + n + payload.Length]; output[0] = (byte)(Format | n);
        for (var i = 0; i < n; i++) output[1 + i] = (byte)(length >> (8 * (n - 1 - i)));
        payload.CopyTo(output, 1 + n); return output;
    }
    public static SecsItem Decode(ReadOnlySpan<byte> bytes)
    {
        var pos = 0;
        var item = Parse(bytes, ref pos, 0);
        if (pos != bytes.Length) throw new FormatException("Trailing SECS data.");
        return item;
    }
    private static SecsItem Parse(ReadOnlySpan<byte> bytes, ref int pos, int depth)
    {
        if (depth > 16 || pos >= bytes.Length) throw new FormatException("Invalid SECS nesting or truncated item.");
        var header = bytes[pos++]; var n = header & 3; var format = (byte)(header & 0xfc);
        if (n == 0 || pos + n > bytes.Length) throw new FormatException("Invalid SECS length.");
        var length = 0; for (var i = 0; i < n; i++) length = (length << 8) | bytes[pos++];
        if (format == ListFormat)
        {
            if (length > 10000) throw new FormatException("SECS list too large.");
            var children = new SecsItem[length];
            for (var i = 0; i < length; i++) children[i] = Parse(bytes, ref pos, depth + 1);
            return L(children);
        }
        if (length > bytes.Length - pos) throw new FormatException("Truncated SECS value.");
        var data = bytes.Slice(pos, length); pos += length;
        switch (format)
        {
            case AsciiFormat: return A(Encoding.ASCII.GetString(data));
            case U4Format:
                if (length % 4 != 0) throw new FormatException("Invalid U4 length.");
                var ints = new uint[length / 4]; for (var i = 0; i < ints.Length; i++) ints[i] = BinaryPrimitives.ReadUInt32BigEndian(data[(i * 4)..]);
                return U(ints);
            case F8Format:
                if (length % 8 != 0) throw new FormatException("Invalid F8 length.");
                var floats = new double[length / 8]; for (var i = 0; i < floats.Length; i++) floats[i] = BitConverter.Int64BitsToDouble(BinaryPrimitives.ReadInt64BigEndian(data[(i * 8)..]));
                return new(format, floats);
            case BinaryFormat: case 0x24: return new(format, data.ToArray());
            default: throw new FormatException($"Unsupported SECS format 0x{format:X2}; IDs in this interface use U4.");
        }
    }
}
