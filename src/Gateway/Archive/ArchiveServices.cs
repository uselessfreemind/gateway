using Gateway.Acquisition;
using Microsoft.AspNetCore.SignalR;

namespace Gateway.Archive;

public sealed class DataHub(GatewayOptions options) : Hub
{
    public Task Subscribe(string controller)
    {
        if (!options.Controllers.Any(c => c.Id == controller)) throw new HubException("Unknown controller.");
        return Groups.AddToGroupAsync(Context.ConnectionId, controller);
    }
    public Task Unsubscribe(string controller) => Groups.RemoveFromGroupAsync(Context.ConnectionId, controller);
}
public sealed class ArchiveWriter(SampleQueue queue, IArchiveStore store, DataState state, IHubContext<DataHub> hub, ILogger<ArchiveWriter> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        // StopAsync completes the channel after acquisition stops; drain accepted data before exit.
        await foreach (var first in queue.Channel.Reader.ReadAllAsync())
        {
            var batch = new List<Sample>(5000) { first };
            while (batch.Count < 5000 && queue.Channel.Reader.TryRead(out var sample)) batch.Add(sample);
            try
            {
                store.Append(batch);
                Interlocked.Add(ref state.ArchivedSamples, batch.Count);
                state.LastArchiveMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(); state.ArchiveError = null;
            }
            catch (Exception ex)
            {
                state.ArchiveError = ex.Message;
                logger.LogCritical(ex, "Archive commit failed; stopping rather than silently losing samples");
                throw;
            }
            // Live viewers receive committed samples. Slow viewers cannot stall archive writes indefinitely.
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            try { await Task.WhenAll(batch.GroupBy(s => s.ControllerId).Select(g => hub.Clients.Group(g.Key).SendAsync("samples", g.ToArray(), timeout.Token))); }
            catch (Exception ex) when (ex is OperationCanceledException or IOException) { logger.LogWarning("Live update delivery timed out; clients can resync using history"); }
        }
    }
    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        queue.Channel.Writer.TryComplete();
        if (ExecuteTask != null) await ExecuteTask.WaitAsync(cancellationToken);
        await base.StopAsync(cancellationToken);
    }
}
public sealed class RetentionService(IArchiveStore store, ILogger<RetentionService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromHours(1));
        do
        {
            logger.LogInformation("Retention removed {Count} expired samples", store.Purge(DateTimeOffset.UtcNow));
        } while (await timer.WaitForNextTickAsync(stoppingToken));
    }
}
