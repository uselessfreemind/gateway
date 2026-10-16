namespace Gateway.Protocols;

public sealed record TagRead(TagOptions Tag, double? Value, string? Error = null);
public static class ReadPlan
{
    public static IReadOnlyList<TagOptions[]> Create(ControllerOptions c)
    {
        var result = new List<TagOptions[]>();
        var group = new List<TagOptions>();
        if (c.Protocol == ControllerProtocol.EtherNetIp)
        {
            var requestBytes = 8; var responseBytes = 6;
            foreach (var tag in c.Tags)
            {
                var requestSize = CipCodec.ReadRequest(tag.Address).Length + 2;
                if (requestSize + 8 > 480) throw new ArgumentException($"CIP address {tag.Address} exceeds the explicit-message budget.");
                if (group.Count > 0 && (requestBytes + requestSize > 480 || responseBytes + 16 > 480))
                { result.Add(group.ToArray()); group.Clear(); requestBytes = 8; responseBytes = 6; }
                group.Add(tag); requestBytes += requestSize; responseBytes += 16;
            }
        }
        else
        {
            byte? area = null; var first = 0; var end = 0;
            foreach (var tag in c.Tags.OrderBy(t => FinsAddress.Parse(t.Address, t.Type).Area).ThenBy(t => Position(t)))
            {
                var a = FinsAddress.Parse(tag.Address, tag.Type);
                var start = Position(tag); var nextEnd = start + a.Count;
                // For bit areas Count is bits; for word areas it is words.
                if (group.Count > 0 && (area != a.Area || nextEnd - first > 400 || start > end + 16))
                { result.Add(group.ToArray()); group.Clear(); }
                if (group.Count == 0) { first = start; area = a.Area; end = nextEnd; }
                group.Add(tag); end = Math.Max(end, nextEnd);
            }
        }
        if (group.Count > 0) result.Add(group.ToArray());
        return result;
    }
    public static int Position(TagOptions tag)
    {
        var a = FinsAddress.Parse(tag.Address, tag.Type);
        return tag.Type == TagType.Bool ? a.Word * 16 + a.Bit : a.Word;
    }
}
