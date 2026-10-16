using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Gateway;
using Gateway.Acquisition;
using Gateway.Archive;
using Gateway.Secs;

var builder = WebApplication.CreateBuilder(args);
// Load a large tag catalog directly: IConfiguration's per-section key scans become
// prohibitively expensive for hundreds of thousands of tags.
var options = builder.Configuration.GetSection("Gateway").Get<GatewayOptions>() ?? new();
var configFile = Environment.GetEnvironmentVariable("GatewayConfig");
if (!string.IsNullOrWhiteSpace(configFile))
{
    using var document = JsonDocument.Parse(File.ReadAllText(Path.GetFullPath(configFile)));
    var jsonOptions = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
    jsonOptions.Converters.Add(new JsonStringEnumConverter());
    options = document.RootElement.GetProperty("Gateway").Deserialize<GatewayOptions>(jsonOptions)
        ?? throw new ArgumentException("Site file requires a Gateway object.");
}
// Scalar/runtime overrides stay small. Controller/tag catalogs belong in the JSON file.
var overrides = new ConfigurationBuilder().AddEnvironmentVariables().AddCommandLine(args).Build();
if (overrides.GetSection("Gateway:Controllers").GetChildren().Any())
    throw new ArgumentException("Set controllers and tags in GatewayConfig, not environment-variable arrays.");
overrides.GetSection("Gateway").Bind(options);
options.Validate();
builder.Services.ConfigureHttpJsonOptions(o => o.SerializerOptions.Converters.Add(new JsonStringEnumConverter()));
builder.Services.AddSingleton(options);
builder.Services.AddSingleton<DataState>();
builder.Services.AddSingleton<SecsState>();
builder.Services.AddSingleton<SampleQueue>();
builder.Services.AddSingleton<IArchiveStore>(_ => options.ArchiveProvider == "Postgres" ? new PostgresArchiveStore(options) : new ArchiveStore(options));
builder.Services.AddSignalR(o => { o.MaximumReceiveMessageSize = 4096; o.MaximumParallelInvocationsPerClient = 1; });
builder.Services.AddHostedService<ArchiveWriter>();
builder.Services.AddHostedService<RetentionService>();
builder.Services.AddHostedService<AcquisitionService>();
builder.Services.AddHostedService<HsmsService>();
builder.Services.Configure<HostOptions>(o => o.ShutdownTimeout = TimeSpan.FromSeconds(30));
var app = builder.Build();
app.Services.GetRequiredService<IArchiveStore>().Initialize();
var apiKey = builder.Configuration["GatewayApiKey"];
var controllerMap = options.Controllers.ToDictionary(c => c.Id);
app.Use(async (context, next) =>
{
    if (!string.IsNullOrEmpty(apiKey))
    {
        var supplied = context.Request.Headers["X-Api-Key"].ToString();
        if (!CryptographicOperations.FixedTimeEquals(SHA256.HashData(Encoding.UTF8.GetBytes(supplied)), SHA256.HashData(Encoding.UTF8.GetBytes(apiKey))))
        { context.Response.StatusCode = 401; return; }
    }
    await next(context);
});
app.MapGet("/health", (DataState state, SampleQueue queue, SecsState secs) => Results.Ok(new
{
    status = state.ArchiveError == null ? "running" : "archive-failed",
    configuredControllers = options.Controllers.Count,
    controllersWithGoodData = state.Controllers.Values.Count(c => c.LastSuccessMs != null),
    queueDepth = queue.Channel.Reader.Count, archivedSamples = Interlocked.Read(ref state.ArchivedSamples),
    state.LastArchiveMs, state.ArchiveError,
    secs = new { enabled = options.Secs.Enabled, secs.Connected, secs.Selected, secs.Communicating, secs.Online, secs.LastError }
}));
app.MapGet("/api/status", (DataState state) => state.Controllers.Values.OrderBy(c => c.Id));
app.MapGet("/api/controllers", (DataState state) => options.Controllers.Select(c => new
{
    c.Id, c.Protocol, c.Host, c.Port, c.PollIntervalMs, c.TimeoutMs,
    status = state.Controllers.GetValueOrDefault(c.Id),
    tags = c.Tags.Select(t => new { t.Id, t.Address, t.Type, t.Svid, t.Unit, t.Scale, t.Offset })
}));
app.MapGet("/api/latest", (string? controller, DataState state) =>
    state.Latest.Values.Where(s => controller == null || s.ControllerId == controller).Select(EffectiveSample).OrderBy(s => s.ControllerId).ThenBy(s => s.TagId));
app.MapGet("/health/ready", (DataState state) =>
{
    var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
    var ready = options.Controllers.Count > 0 && state.ArchiveError == null && options.Controllers.All(c =>
        state.Controllers.TryGetValue(c.Id, out var status) && status.LastSuccessMs.HasValue &&
        now - status.LastSuccessMs.Value <= 2L * c.PollIntervalMs + c.TimeoutMs &&
        c.Tags.All(t => state.Latest.TryGetValue(t.Svid, out var sample) && EffectiveSample(sample).Quality == "Good"));
    return Results.Json(new { ready }, statusCode: ready ? 200 : 503);
});
app.MapGet("/api/history/{controller}/{tag}", (string controller, string tag, long from, long to, int? limit, long? afterId, IArchiveStore store) =>
{
    if (!Known(controller, tag)) return Results.NotFound();
    if (!ValidRange(from, to) || limit is < 1 or > 10000 || afterId is < 0) return Results.BadRequest(new { error = "Use UTC epoch milliseconds, from <= to, limit 1–10000 and nonnegative afterId." });
    var samples = store.History(controller, tag, from, to, limit ?? 1000, afterId ?? 0);
    return Results.Ok(new { samples, nextAfterId = samples.Count == 0 ? (long?)null : samples[^1].Id });
});
app.MapGet("/api/trend/{controller}/{tag}", (string controller, string tag, long from, long to, long bucketMs, IArchiveStore store) =>
{
    if (!Known(controller, tag)) return Results.NotFound();
    if (!ValidRange(from, to) || bucketMs < 100 || (to / bucketMs - from / bucketMs) > 1999)
        return Results.BadRequest(new { error = "Use a valid UTC range and bucketMs >= 100 producing at most 2000 buckets." });
    return Results.Ok(store.Trend(controller, tag, from, to, bucketMs));
});
app.MapHub<DataHub>("/live");
app.Run();

bool Known(string controller, string tag) => options.Controllers.Any(c => c.Id == controller && c.Tags.Any(t => t.Id == tag));
bool ValidRange(long from, long to) => from >= 0 && to >= from && to <= 253402300799999L;

Sample EffectiveSample(Sample sample)
{
    var c = controllerMap[sample.ControllerId];
    return sample.Quality == "Good" && DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - sample.TimestampMs > 2L * c.PollIntervalMs + c.TimeoutMs
        ? sample with { Quality = "Stale", Value = null, Error = "No recent sample" } : sample;
}

public partial class Program { }
