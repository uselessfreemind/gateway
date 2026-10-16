using System.Net;
using Gateway.Protocols;

namespace Gateway;

public sealed class GatewayOptions
{
    public string ArchiveProvider { get; set; } = "Sqlite";
    public string? PostgresConnectionString { get; set; }
    public string DatabasePath { get; set; } = "data/gateway.db";
    public int MaxConcurrentPolls { get; set; } = 64;
    public int QueueCapacity { get; set; } = 20000;
    public List<ControllerOptions> Controllers { get; set; } = [];
    public SecsOptions Secs { get; set; } = new();

    public void Validate()
    {
        if (ArchiveProvider is not ("Sqlite" or "Postgres")) throw new ArgumentException("ArchiveProvider must be Sqlite or Postgres.");
        if (ArchiveProvider == "Postgres" && string.IsNullOrWhiteSpace(PostgresConnectionString)) throw new ArgumentException("Postgres requires Gateway__PostgresConnectionString.");
        if (string.IsNullOrWhiteSpace(DatabasePath)) throw new ArgumentException("DatabasePath is required.");
        if (Controllers.Count > 500) throw new ArgumentException("At most 500 controllers are supported.");
        if (MaxConcurrentPolls is < 1 or > 500 || QueueCapacity is < 100 or > 1000000)
            throw new ArgumentException("Invalid concurrency or queue capacity.");
        var controllers = new HashSet<string>(StringComparer.Ordinal);
        var svids = new HashSet<uint>();
        foreach (var c in Controllers)
        {
            if (!ValidId(c.Id) || !controllers.Add(c.Id)) throw new ArgumentException("Controller IDs must be unique and contain only letters, numbers, '-' or '_'.");
            if (!IPAddress.TryParse(c.Host, out var ip) || ip.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
                throw new ArgumentException($"{c.Id}: use an IPv4 address for Host.");
            if (c.Port is < 1 or > 65535 || c.PollIntervalMs is < 20 or > 86400000 || c.TimeoutMs is < 50 or > 60000)
                throw new ArgumentException($"{c.Id}: invalid port, interval or timeout.");
            if (!Enum.IsDefined(c.Protocol)) throw new ArgumentException($"{c.Id}: invalid protocol.");
            if (c.Tags.Count is < 1 or > 10000) throw new ArgumentException($"{c.Id}: configure 1–10000 tags.");
            var tags = new HashSet<string>(StringComparer.Ordinal);
            foreach (var t in c.Tags)
            {
                if (!ValidId(t.Id) || !tags.Add(t.Id) || !svids.Add(t.Svid) || t.Svid == 0)
                    throw new ArgumentException($"{c.Id}: tag IDs must be unique per controller; SVIDs must be globally unique and positive.");
                if (!Enum.IsDefined(t.Type) || !double.IsFinite(t.Scale) || !double.IsFinite(t.Offset)) throw new ArgumentException("Invalid tag type or scaling.");
                if (c.Protocol == ControllerProtocol.FinsUdp) FinsAddress.Parse(t.Address, t.Type);
                else CipCodec.SymbolPath(t.Address);
            }
        }
        foreach (var c in Controllers) ReadPlan.Create(c);
        Secs.Validate();
    }
    private static bool ValidId(string id) => id.Length is > 0 and <= 80 && id.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_');
}
public enum ControllerProtocol { FinsUdp, EtherNetIp }
public enum TagType { Bool, Int16, UInt16, Int32, UInt32, Float32, Float64 }
public sealed class ControllerOptions
{
    public string Id { get; set; } = "";
    public ControllerProtocol Protocol { get; set; }
    public string Host { get; set; } = "127.0.0.1";
    public int Port { get; set; } = 9600;
    public int PollIntervalMs { get; set; } = 1000;
    public int TimeoutMs { get; set; } = 1000;
    public byte DestinationNetwork { get; set; }
    public byte DestinationNode { get; set; } = 1;
    public byte DestinationUnit { get; set; }
    public byte SourceNetwork { get; set; }
    public byte SourceNode { get; set; } = 100;
    public byte SourceUnit { get; set; }
    // FINS word order varies by PLC family and configured data layout.
    public bool LowWordFirst { get; set; } = true;
    public List<TagOptions> Tags { get; set; } = [];
}
public sealed class TagOptions
{
    public string Id { get; set; } = "";
    public string Address { get; set; } = "";
    public TagType Type { get; set; } = TagType.UInt16;
    public uint Svid { get; set; }
    public string Unit { get; set; } = "";
    public double Scale { get; set; } = 1;
    public double Offset { get; set; }
}
public sealed class SecsOptions
{
    public bool Enabled { get; set; }
    public string Mode { get; set; } = "Passive";
    public string Host { get; set; } = "127.0.0.1";
    public int Port { get; set; } = 5000;
    public ushort DeviceId { get; set; }
    public int T3Seconds { get; set; } = 45;
    public int T6Seconds { get; set; } = 5;
    public int T7Seconds { get; set; } = 10;
    public int T8Seconds { get; set; } = 5;
    public int ReconnectSeconds { get; set; } = 5;
    public int EventIntervalMs { get; set; } = 1000;
    public void Validate()
    {
        if (Mode is not ("Passive" or "Active") || !IPAddress.TryParse(Host, out _) || Port is < 1 or > 65535 || DeviceId > 32767 ||
            T3Seconds is < 1 or > 120 || T6Seconds is < 1 or > 120 || T7Seconds is < 1 or > 120 || T8Seconds is < 1 or > 120 ||
            ReconnectSeconds is < 1 or > 300 || EventIntervalMs is < 100 or > 60000)
            throw new ArgumentException("Invalid SECS/HSMS configuration.");
    }
}
