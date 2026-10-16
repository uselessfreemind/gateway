using System.Collections.Concurrent;
using System.Threading.Channels;
using Gateway.Protocols;

namespace Gateway.Acquisition;

public sealed record Sample(string ControllerId, string TagId, uint Svid, long TimestampMs, double? Value, string Quality, string? Error);
public sealed record ControllerStatus(string Id, long? LastAttemptMs, long? LastSuccessMs, double LastScanMs, long CompletedScans, long FailedReads, long Overruns);
public sealed class DataState
{
    public ConcurrentDictionary<uint, Sample> Latest { get; } = new();
    public ConcurrentDictionary<string, ControllerStatus> Controllers { get; } = new();
    public long ArchivedSamples;
    public long? LastArchiveMs;
    public string? ArchiveError;
}
public sealed class SampleQueue(GatewayOptions options)
{
    public Channel<Sample> Channel { get; } = System.Threading.Channels.Channel.CreateBounded<Sample>(new BoundedChannelOptions(options.QueueCapacity)
    { FullMode = BoundedChannelFullMode.Wait, SingleReader = true, SingleWriter = false });
}
public sealed class AcquisitionService(GatewayOptions options, SampleQueue queue, DataState state, ILogger<AcquisitionService> logger) : BackgroundService
{
    private readonly SemaphoreSlim concurrency = new(options.MaxConcurrentPolls);
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        await Task.WhenAll(options.Controllers.Select(c => Poll(c, stoppingToken)));
    }
    private async Task Poll(ControllerOptions c, CancellationToken token)
    {
        await using IControllerClient client = c.Protocol == ControllerProtocol.FinsUdp ? new FinsClient(c) : new EtherNetIpClient(c);
        var plan = ReadPlan.Create(c);
        long completed = 0, failed = 0, overruns = 0;
        long? lastSuccess = null;
        state.Controllers[c.Id] = new(c.Id, null, null, 0, 0, 0, 0);
        try
        {
            // Spread connection establishment rather than opening 500 sessions at once.
            await Task.Delay(Random.Shared.Next(Math.Min(c.PollIntervalMs, 1000)), token);
            while (!token.IsCancellationRequested)
            {
                var watch = System.Diagnostics.Stopwatch.StartNew();
                var attempt = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
                await concurrency.WaitAsync(token);
                var samples = new List<Sample>(c.Tags.Count);
                try
                {
                    for (var index = 0; index < plan.Count; index++)
                    {
                        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                        timeout.CancelAfter(c.TimeoutMs);
                        IReadOnlyList<TagRead> reads;
                        var interrupted = false;
                        try { reads = await client.ReadBatchAsync(plan[index], timeout.Token); }
                        catch (Exception ex) when (!token.IsCancellationRequested && ex is IOException or System.Net.Sockets.SocketException or OperationCanceledException)
                        {
                            var error = ex is OperationCanceledException ? "Read timeout" : ex.Message;
                            reads = plan[index].Select(t => new TagRead(t, null, error)).ToArray();
                            interrupted = true;
                        }
                        foreach (var read in reads)
                        {
                            var scaled = read.Value * read.Tag.Scale + read.Tag.Offset;
                            var error = read.Error;
                            if (scaled.HasValue && !double.IsFinite(scaled.Value)) { scaled = null; error = "Non-finite PLC value"; }
                            if (error != null) failed++;
                            else lastSuccess = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
                            samples.Add(new(c.Id, read.Tag.Id, read.Tag.Svid, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), scaled, error == null ? "Good" : "Bad", error));
                        }
                        if (interrupted)
                        {
                            foreach (var rest in plan.Skip(index + 1).SelectMany(x => x))
                            {
                                failed++;
                                samples.Add(new(c.Id, rest.Id, rest.Svid, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), null, "Bad", "Scan interrupted after failed read"));
                            }
                            break;
                        }
                    }
                }
                finally { concurrency.Release(); }
                foreach (var sample in samples)
                {
                    await queue.Channel.Writer.WriteAsync(sample, token);
                    state.Latest[sample.Svid] = sample;
                }
                watch.Stop(); completed++;
                if (watch.ElapsedMilliseconds > c.PollIntervalMs) overruns++;
                state.Controllers[c.Id] = new(c.Id, attempt, lastSuccess, watch.Elapsed.TotalMilliseconds, completed, failed, overruns);
                // Missed deadlines never accumulate into overlapping scans.
                await Task.Delay(TimeSpan.FromMilliseconds(Math.Max(1, c.PollIntervalMs - watch.Elapsed.TotalMilliseconds)), token);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception ex) { logger.LogCritical(ex, "Controller worker {Controller} failed", c.Id); throw; }
    }
}
