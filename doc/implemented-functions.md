# Implemented function inventory

| Function | main: C# / web | feature/cpp-winui3-scada: C++ / WinUI 3 |
|---|---|---|
| Read-only CJ2 acquisition | FINS/UDP grouped memory reads | FINS/UDP grouped memory reads |
| Read-only NX acquisition | EtherNet/IP explicit symbolic reads / multiple-service packets | EtherNet/IP explicit symbolic reads / multiple-service packets |
| Up to 500 configured controllers | Independent async loops, global concurrency bound; 20 ms minimum configuration | Asio coroutines/strands; 10 ms target configuration |
| Connection monitoring | Per-controller state, retry count/error API and web monitor | Per-controller snapshot fields and scrollable WinUI status list |
| Reconnect budget | Initial failure + 3 automatic retries; then manual retry required | Same policy |
| Global sampling restart | Web button / POST endpoint; cancels and replaces per-controller sessions | WinUI button / C ABI; wakes controller strands and resets sessions |
| Selectable analogue trends | Register up to 8 configured tags by ID/address; real latest values at 1 Hz; Bad/Stale gaps | Existing selected tag/range archive chart, refreshed 5 Hz; analogue filtering and address display |
| Historical values / trends | SQLite or PostgreSQL, cursor history and aggregate trend APIs | SQLite, C ABI history and aggregate trend queries |
| Retention | One UTC calendar month; startup/hourly physical cleanup | Same policy |
| Live display | Real controller monitor/trend register plus separately labeled illustrative System B screen | Current values and committed archive trend display |
| Factory host interface | Equipment-side HSMS-SS / SECS-GEM subset | Same subset in native C++ |
| Read-only protection | Sampling controls never write PLCs; S2F41 rejected | Same |

## Limits retained

Neither branch is a hard real-time system. Global restart requests are dispatched together but do not guarantee simultaneous PLC responses. The native maximum 500 x 700 tags at sustained 10 ms failed its previous capacity experiment; no new throughput claim is made. Physical Omron and production host acceptance remain outstanding. SECS/GEM is a documented subset, without full E30 compliance, alarms/program/spooling completeness or certification. EtherNet/IP implicit cyclic I/O is not implemented. No automatic archive decimation is applied; bounded queues exert backpressure. Archive queues are not durable outage spools. Interactive WinUI acceptance remains outstanding.
