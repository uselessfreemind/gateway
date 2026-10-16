# Web-viewer backend contract

All timestamps are UTC Unix milliseconds. All endpoints require `X-Api-Key` when `GatewayApiKey` is configured. There are no PLC write endpoints.

| Endpoint | Result |
|---|---|
| `GET /health` | Process/archive state, configured controller count, queue depth, archive count, HSMS state. This is liveness/diagnostics, not readiness. |
| `GET /health/ready` | 200 only when every configured tag has recent Good data; otherwise 503. Empty configuration is not ready. |
| `GET /api/status` | Lightweight controller scan/failure counters and last-attempt/success timestamps. |
| `GET /api/controllers` | Full configuration metadata and tag catalog; potentially large at maximum topology. |
| `GET /api/latest?controller=cj2-001` | Latest samples, optionally filtered by controller. No sample is returned before the first scan. A Bad or Stale value is null. |
| `GET /api/history/{controller}/{tag}?from=...&to=...&limit=1000&afterId=0` | `{samples, nextAfterId}`; limit 1–10000. Continue with the same range and `afterId=nextAfterId` until an empty page. Ordered by archive insertion ID, which is monotonic within a database. |
| `GET /api/trend/{controller}/{tag}?from=...&to=...&bucketMs=1000` | Epoch-aligned buckets containing `timestampMs,count,goodCount,min,max,average`; empty intervals are omitted, bad-only buckets have null statistics. At most 2000 buckets; minimum 100 ms bucket. |
| `/live` | ASP.NET Core SignalR hub. Invoke `Subscribe(controllerId)` / `Unsubscribe(controllerId)`; receive `samples` arrays only for subscribed controllers. |

Example browser integration for a future same-origin viewer:

```javascript
// With @microsoft/signalr installed by the future frontend.
const connection = new signalR.HubConnectionBuilder()
  .withUrl('/live', { transport: signalR.HttpTransportType.LongPolling,
                     headers: { 'X-Api-Key': runtimeApiKey } })
  .withAutomaticReconnect().build();
connection.on('samples', batch => updateChart(batch));
connection.onreconnected(async () => {
  await connection.invoke('Subscribe', 'cj2-001');
  await reloadRecentHistory();
});
await connection.start();
await connection.invoke('Subscribe', 'cj2-001');
```

Browser WebSocket APIs cannot set arbitrary authentication headers. With this header-based API-key scheme, use long polling as shown or supply a properly authenticated reverse proxy; do not put an API key in a URL. An API key distributed to a browser is shared access, not per-user identity; production viewer authorization should be implemented by the hosting application.

Live batches contain **committed** samples, but provide no durable delivery acknowledgement. Slow clients can miss live batches and must resync through history after reconnects. Do not subscribe every browser to all 350,000 tags. Display Bad/Stale samples as gaps, not zeroes, and use the last update timestamp to mark stale displayed data between server events. Scaling applies before archive/SECS/live output. Historic records retain tag identity and value, but not configuration versions or engineering-unit changes.
