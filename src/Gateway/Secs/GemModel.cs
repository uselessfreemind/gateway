using Gateway.Acquisition;

namespace Gateway.Secs;

// Equipment-side subset. Host report definitions and links are scoped to one HSMS connection.
public sealed class GemModel(GatewayOptions options, DataState state)
{
    private readonly Dictionary<uint, (ControllerOptions Controller, TagOptions Tag)> tags = options.Controllers
        .SelectMany(c => c.Tags.Select(t => (c, t))).ToDictionary(x => x.t.Svid, x => (x.c, x.t));
    private readonly Dictionary<uint, uint[]> reports = new();
    private uint[] linkedReports = [];
    private bool enabled;
    public bool Communicating { get; private set; }
    public bool Online { get; private set; }
    public (byte Function, SecsItem? Body) Handle(byte stream, byte function, SecsItem? body)
    {
        if (stream == 1)
        {
            switch (function)
            {
                case 1: return (2, SecsItem.L(SecsItem.A("OmronGateway"), SecsItem.A("0.1.0")));
                case 13:
                    if (body == null || body.Items.Length != 0) throw new FormatException("S1F13 host body must be L[0].");
                    Communicating = true;
                    return (14, SecsItem.L(SecsItem.B(0), SecsItem.L(SecsItem.A("OmronGateway"), SecsItem.A("0.1.0"))));
                case 3:
                    RequireCommunication();
                    var ids = Ids(body!); if (ids.Length == 0) ids = tags.Keys.Order().ToArray();
                    return (4, SecsItem.L(ids.Select(Value).ToArray()));
                case 11:
                    RequireCommunication();
                    var names = Ids(body!); if (names.Length == 0) names = tags.Keys.Order().ToArray();
                    return (12, SecsItem.L(names.Select(id => tags.TryGetValue(id, out var tag)
                        ? SecsItem.L(SecsItem.U(id), SecsItem.A($"{tag.Controller.Id}.{tag.Tag.Id}"), SecsItem.A(tag.Tag.Unit))
                        : SecsItem.L(SecsItem.U(id), SecsItem.A(""), SecsItem.A(""))).ToArray()));
                case 15: RequireCommunication(); Online = false; return (16, SecsItem.B(0));
                case 17: RequireCommunication(); Online = true; return (18, SecsItem.B(0));
            }
        }
        if (stream == 2)
        {
            RequireCommunication();
            switch (function)
            {
                case 33: // Define reports: L[DATAID, L[L[RPTID, L[SVID...]]...]]
                    var define = Exact(body!, 2); _ = define[0].UInt;
                    var definitions = define[1].Items;
                    var proposed = new Dictionary<uint, uint[]>(reports);
                    if (definitions.Length == 0) { reports.Clear(); linkedReports = []; return (34, SecsItem.B(0)); }
                    foreach (var definition in definitions)
                    {
                        var d = Exact(definition, 2); var id = d[0].UInt; var vids = Ids(d[1]);
                        if (vids.Any(v => !tags.ContainsKey(v))) return (34, SecsItem.B(4));
                        if (vids.Length == 0) proposed.Remove(id);
                        else if (!proposed.TryAdd(id, vids)) return (34, SecsItem.B(3));
                    }
                    if (proposed.Count > 100 || proposed.Values.Sum(x => x.Length) > 10000) return (34, SecsItem.B(1));
                    reports.Clear(); foreach (var p in proposed) reports.Add(p.Key, p.Value);
                    linkedReports = linkedReports.Where(reports.ContainsKey).ToArray();
                    return (34, SecsItem.B(0));
                case 35: // Link event 1001 to reports
                    var link = Exact(body!, 2); _ = link[0].UInt;
                    var entries = link[1].Items;
                    if (entries.Length == 0) { linkedReports = []; return (36, SecsItem.B(0)); }
                    if (entries.Length != 1) return (36, SecsItem.B(4));
                    var entry = Exact(entries[0], 2);
                    if (entry[0].UInt != 1001) return (36, SecsItem.B(4));
                    var linked = Ids(entry[1]);
                    if (linked.Any(id => !reports.ContainsKey(id))) return (36, SecsItem.B(5));
                    if (linked.Distinct().Count() != linked.Length) return (36, SecsItem.B(3));
                    linkedReports = linked; return (36, SecsItem.B(0));
                case 37:
                    var enable = Exact(body!, 2); var flag = enable[0].Flag; var events = Ids(enable[1]);
                    if (events.Any(e => e != 1001)) return (38, SecsItem.B(1));
                    enabled = flag; return (38, SecsItem.B(0));
                case 41: // No remote PLC control, including through GEM.
                    return (42, SecsItem.L(SecsItem.B(1), SecsItem.L()));
            }
        }
        return (0, null); // SxF0 abort for unsupported transactions.
    }
    public SecsItem? CollectionEvent()
    {
        if (!Communicating || !Online || !enabled || linkedReports.Length == 0) return null;
        return SecsItem.L(SecsItem.U(0), SecsItem.U(1001), SecsItem.L(linkedReports.Select(id =>
            SecsItem.L(SecsItem.U(id), SecsItem.L(reports[id].Select(Value).ToArray()))).ToArray()));
    }
    private SecsItem Value(uint id)
    {
        if (!tags.TryGetValue(id, out var t) || !state.Latest.TryGetValue(id, out var sample) || sample.Quality != "Good" || sample.Value == null ||
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - sample.TimestampMs > 2L * t.Controller.PollIntervalMs + t.Controller.TimeoutMs)
            return SecsItem.L();
        return SecsItem.F(sample.Value.Value);
    }
    private void RequireCommunication() { if (!Communicating) throw new FormatException("Establish communications with S1F13 first."); }
    private static uint[] Ids(SecsItem item) => item.Items.Select(x => x.UInt).ToArray();
    private static SecsItem[] Exact(SecsItem item, int length) => item.Items.Length == length ? item.Items : throw new FormatException("Invalid SECS list shape.");
}
