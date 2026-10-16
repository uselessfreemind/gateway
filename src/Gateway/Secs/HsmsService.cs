using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using Gateway.Acquisition;

namespace Gateway.Secs;

public sealed class SecsState
{
    public volatile bool Connected;
    public volatile bool Selected;
    public volatile bool Communicating;
    public volatile bool Online;
    public string? LastError;
}
public sealed class HsmsService(GatewayOptions options, DataState data, SecsState state, ILogger<HsmsService> logger) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var o = options.Secs;
        if (!o.Enabled) return;
        TcpListener? listener = null;
        if (o.Mode == "Passive") { listener = new(IPAddress.Parse(o.Host), o.Port); listener.Start(1); }
        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                try
                {
                    using var client = listener != null ? await listener.AcceptTcpClientAsync(stoppingToken) : new TcpClient();
                    if (listener == null)
                    {
                        using var connect = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
                        connect.CancelAfter(TimeSpan.FromSeconds(o.T6Seconds));
                        await client.ConnectAsync(o.Host, o.Port, connect.Token);
                    }
                    client.NoDelay = true; state.Connected = true;
                    logger.LogInformation("HSMS connected");
                    await new HsmsSession(client.GetStream(), options, data, state, logger).RunAsync(stoppingToken);
                }
                catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested) { break; }
                catch (Exception ex) when (ex is IOException or SocketException or OperationCanceledException or TimeoutException or FormatException)
                { state.LastError = ex.Message; logger.LogWarning("HSMS session ended: {Reason}", ex.Message); }
                finally { state.Connected = state.Selected = state.Communicating = state.Online = false; }
                await Task.Delay(TimeSpan.FromSeconds(o.ReconnectSeconds), stoppingToken);
            }
        }
        finally { listener?.Stop(); }
    }
}
public sealed class HsmsSession(Stream stream, GatewayOptions options, DataState data, SecsState state, ILogger logger)
{
    private sealed record Pending(byte Stream, byte Function, byte SType, TaskCompletionSource<HsmsFrame> Completion);
    private readonly ConcurrentDictionary<uint, Pending> pending = new();
    private readonly SemaphoreSlim sendGate = new(1);
    private readonly GemModel model = new(options, data);
    private readonly object modelLock = new();
    private int counter;
    private readonly DateTimeOffset selectionStarted = DateTimeOffset.UtcNow;
    public async Task RunAsync(CancellationToken token)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(token);
        var reader = Receive(lifetime.Token);
        var maintenance = Maintain(lifetime.Token);
        try
        {
            var first = await Task.WhenAny(reader, maintenance);
            lifetime.Cancel();
            try { await Task.WhenAll(reader, maintenance); }
            catch { await first; throw; }
        }
        finally
        {
            lifetime.Cancel();
            foreach (var p in pending.Values) p.Completion.TrySetCanceled();
        }
    }
    private async Task Send(HsmsFrame message, CancellationToken token)
    {
        await sendGate.WaitAsync(token);
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(options.Secs.T8Seconds));
            await stream.WriteAsync(message.Encode(), timeout.Token);
        }
        finally { sendGate.Release(); }
    }
    private async Task<HsmsFrame> Transaction(HsmsFrame message, byte responseType, int seconds, CancellationToken token)
    {
        var p = new Pending(message.Stream, (byte)(message.Function + 1), responseType, new(TaskCreationOptions.RunContinuationsAsynchronously));
        if (!pending.TryAdd(message.SystemBytes, p)) throw new IOException("Duplicate HSMS system bytes.");
        try
        {
            await Send(message, token);
            return await p.Completion.Task.WaitAsync(TimeSpan.FromSeconds(seconds), token);
        }
        finally { pending.TryRemove(message.SystemBytes, out _); }
    }
    private uint NextSystem() => unchecked((uint)Interlocked.Increment(ref counter));
    private async Task Maintain(CancellationToken token)
    {
        if (options.Secs.Mode == "Active")
        {
            var reply = await Transaction(HsmsFrame.Control(1, NextSystem()), 2, options.Secs.T6Seconds, token);
            if (reply.Function != 0) throw new IOException("HSMS selection refused.");
            state.Selected = true;
        }
        var lastLinktest = DateTimeOffset.UtcNow;
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(options.Secs.EventIntervalMs));
        while (await timer.WaitForNextTickAsync(token))
        {
            if (!state.Selected) continue;
            if (DateTimeOffset.UtcNow - lastLinktest > TimeSpan.FromSeconds(30))
            {
                await Transaction(HsmsFrame.Control(5, NextSystem()), 6, options.Secs.T6Seconds, token);
                lastLinktest = DateTimeOffset.UtcNow;
            }
            SecsItem? ev;
            lock (modelLock) ev = model.CollectionEvent();
            if (ev == null) continue;
            var ack = await Transaction(new(options.Secs.DeviceId, 6, 11, true, 0, NextSystem(), ev.Encode()), 0, options.Secs.T3Seconds, token);
            var item = SecsItem.Decode(ack.Body);
            if (item.Format != SecsItem.BinaryFormat || !((byte[])item.Value).SequenceEqual(new byte[] { 0 }))
                logger.LogWarning("Host rejected S6F11 collection event");
        }
    }
    private async Task Receive(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            using var selectionTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            if (!state.Selected)
            {
                var remaining = TimeSpan.FromSeconds(options.Secs.T7Seconds) - (DateTimeOffset.UtcNow - selectionStarted);
                if (remaining <= TimeSpan.Zero) throw new TimeoutException("HSMS T7 selection timeout.");
                selectionTimeout.CancelAfter(remaining);
            }
            var frame = await HsmsFrame.ReadAsync(stream, options.Secs.T8Seconds, selectionTimeout.Token);
            if (frame.SType != 0)
            {
                if (frame.Session != 0xffff || frame.Body.Length != 0 || frame.ReplyExpected) throw new IOException("Invalid HSMS control frame.");
                if (frame.SType is 2 or 4 or 6)
                {
                    if (pending.TryGetValue(frame.SystemBytes, out var p) && p.SType == frame.SType)
                    {
                        if (frame.SType == 2 && frame.Function == 0) state.Selected = true;
                        p.Completion.TrySetResult(frame);
                    }
                    continue;
                }
                switch (frame.SType)
                {
                    case 1:
                        await Send(HsmsFrame.Control(2, frame.SystemBytes, state.Selected ? (byte)1 : (byte)0), token);
                        state.Selected = true; break;
                    case 3:
                        await Send(HsmsFrame.Control(4, frame.SystemBytes), token);
                        throw new IOException("HSMS deselected by host.");
                    case 5: await Send(HsmsFrame.Control(6, frame.SystemBytes), token); break;
                    case 9: throw new IOException("HSMS separated by host.");
                    default: throw new IOException("Unsupported HSMS control transaction.");
                }
                continue;
            }
            if (!state.Selected) throw new IOException("HSMS data before selection.");
            if (frame.Session != options.Secs.DeviceId) throw new IOException("HSMS device ID mismatch.");
            if (frame.Function % 2 == 0)
            {
                if (pending.TryGetValue(frame.SystemBytes, out var p) && p.SType == 0 && p.Stream == frame.Stream && p.Function == frame.Function)
                    p.Completion.TrySetResult(frame);
                continue;
            }
            (byte Function, SecsItem? Body) response;
            try
            {
                var item = frame.Body.Length == 0 ? null : SecsItem.Decode(frame.Body);
                lock (modelLock)
                {
                    response = model.Handle(frame.Stream, frame.Function, item);
                    state.Communicating = model.Communicating; state.Online = model.Online;
                }
            }
            catch (Exception ex) when (ex is FormatException or NullReferenceException)
            { logger.LogWarning("Rejected S{Stream}F{Function}: {Reason}", frame.Stream, frame.Function, ex.Message); response = (0, null); }
            if (frame.ReplyExpected)
                await Send(new(options.Secs.DeviceId, frame.Stream, response.Function, false, 0, frame.SystemBytes, response.Body?.Encode() ?? []), token);
        }
    }
}
