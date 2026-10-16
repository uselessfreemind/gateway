# Omron SCADA gateway

C# / .NET 8 backend for read-only Omron acquisition, live web-viewer data, calendar-month archives, and equipment-side SECS/HSMS communication. The UI is intentionally deferred.

## Run

```bash
dotnet restore Gateway.sln --locked-mode
dotnet test Gateway.sln
dotnet run --project src/Gateway
```

In this cloud workspace, first run `source scripts/dotnet-env.sh`. The service binds to loopback port 5080. The default configuration has **no controllers** and SECS is disabled, so starting it does not contact factory equipment. `/health/ready` returns 503 until configured controllers supply good data.

For a site configuration, copy `config/site.example.json` to a local file, replace its sample addresses and tags, and set `GatewayConfig` to the absolute path before starting. Configuration is validated at startup; changes require a restart. Do not commit site secrets. Scalar `Gateway__...` environment variables override configuration, e.g. `Gateway__MaxConcurrentPolls`. Controller/tag arrays must be supplied in the site JSON file, which is deserialized directly for large-catalog startup performance.

```bash
export GatewayConfig=/absolute/path/site.json
# Set GatewayApiKey securely if enabling access outside a trusted local machine.
dotnet run --project src/Gateway
```

For PostgreSQL, set `Gateway:ArchiveProvider` to `Postgres` and provide `Gateway__PostgresConnectionString` through a secure environment binding. `compose.yaml` provides a local PostgreSQL 16 instance; it requires `POSTGRES_PASSWORD`. Run `docker compose up -d --wait` after setting that variable. The service creates the `scada` schema, table, indexes and daily partitions automatically. Use a dedicated database and a role permitted to manage that schema. SQLite is the default for development and smaller workloads.

## What is implemented

- Up to **500 configured controllers**, independent asynchronous polling, bounded concurrent scans and archive queue, no overlapping scans, timeouts, reconnection, quality flags, scan-duration/overrun counters, scaling and engineering units.
- **CJ2:** FINS/UDP Memory Area Read (`0101`) on Ethernet. D, CIO, W, H and A memory; bit, signed/unsigned 16/32-bit and 32/64-bit floating-point values. Contiguous/small-gap reads are grouped into requests of at most 400 words or bits.
- **NX:** EtherNet/IP TCP explicit symbolic Read Tag (`0x4C`) and Multiple Service Packet (`0x0A`), using RegisterSession and SendRRData. BOOL, INT, UINT, DINT, UDINT, REAL and LREAL scalar tags, structure members and one-dimensional array elements. Requests and estimated responses are limited to 480 CIP bytes. No implicit cyclic I/O, routing through a backplane, fragmented tag reads, strings, or whole structures/arrays.
- No controller write services or remote PLC commands. Invalid tags and failed reads produce null values with Bad quality. Stale values are excluded from SECS status reporting and marked Stale by the latest API.
- SQLite WAL transactions or PostgreSQL binary COPY with daily partitions. Every acquired sample, including Bad quality, is archived; queue pressure slows acquisition instead of deliberately dropping samples.
- Physical expiry runs at startup and hourly, using **UTC now minus one calendar month**. Queries exclude expired rows immediately. PostgreSQL drops fully expired daily partitions and deletes the boundary rows; SQLite deletes in batches and reuses freed database pages. Filesystem space from SQLite is reused rather than automatically compacted. Backups have their own retention policy.
- Read-only REST APIs and SignalR subscriptions for the future browser viewer. Raw history has cursor pagination; trend queries return min/max/average and good/total sample counts in up to 2000 buckets.
- Equipment-side **SECS-II / HSMS-SS subset** with active/passive TCP, select/deselect/separate/linktest, T3/T5 (reconnect)/T6/T7/T8 handling, status variables, communication/online state, host-defined reports, event links, report enabling and acknowledged S6F11 transmission. See [the host interface](host-interface.md).

## Controller commissioning

Use FINS for CJ2 initially and symbolic EtherNet/IP for NX. Exact support depends on CPU, Ethernet unit and firmware. On NX, expose the intended variables for network access in Sysmac Studio and confirm their CIP types. Configure FINS source/destination node, network and unit numbers to match the PLC routing table; the source node must be unique and reachable from the PLC. NAT and FINS automatic IP/node conversion can require PLC configuration. Confirm multiword order with a known REAL/DINT value; `LowWordFirst` defaults to true.

The provided clients are validated against simulators, **not physical Omron hardware**. The configured 20 ms minimum interval is a scheduler limit, not a guaranteed scan period. This is polling plus live delivery, not a hard real-time system. PLC scan load, address layout, UDP losses and supported CIP services determine achievable rates. Poll timestamps are gateway UTC read-completion times, not synchronized PLC scan timestamps; a multi-request scan is not atomic.

## Capacity and reliability

500 × 700 tags is 350,000 values per scan. At a one-second scan interval, 30 days produces approximately **907.2 billion samples** before indexes and replication. Even an illustrative 40 bytes/sample is about 36 TB; actual rows/indexes can cost much more. Do not size a deployment from controller count alone. Use PostgreSQL, measure sustained ingest and query rates, provision storage, and establish the required scan interval before deployment. No archival decimation or deadband is silently applied.

`MaxConcurrentPolls` bounds concurrent controller scans (default 64). Grouping reduces requests substantially for contiguous CJ2 addresses; sparse layouts and long NX symbol names cost more. `TimeoutMs` applies per grouped request. On a transport failure, remaining tags in that controller's scan are marked Bad and the next scan reconnects. A CIP embedded tag failure affects that tag; other embedded replies are still decoded. Check `/api/status` for completed scans, failed reads, scan times and overruns, and `/health` for archive counts and queue depth. A process being alive does not establish PLC readiness.

The in-memory queue is bounded and **not a durable outage spool**. A database failure stops the host rather than silently continuing without an archive. Committed transactions survive restarts; queued samples can be lost on abrupt termination. Graceful shutdown stops acquisition and drains accepted queue entries (up to the 30-second host shutdown timeout). Production deployment still needs service supervision, storage monitoring, backups, time synchronization, and measured soak/failure tests. Multiple gateway instances must not poll/archive the same source unintentionally.

FINS, EtherNet/IP and HSMS are unencrypted on factory networks. Keep them on an approved isolated network. The HTTP API defaults to loopback; for remote access, use TLS at a reverse proxy and configure `GatewayApiKey` (header `X-Api-Key`, including SignalR transport requests). No CORS access is enabled by default. No network policy is automatically expanded to factory addresses.

## Validation

```bash
dotnet test Gateway.sln
python scripts/smoke.py
python scripts/smoke.py --controllers 500 --tags 700 --interval 10000 --timeout 120
```

The Python smoke runner starts local FINS and EtherNet/IP simulators and a temporary gateway. It checks acquisition, archives, trends, authentication, HSMS selection/status and a real SignalR long-polling subscription. The small run also disconnects FINS and verifies Bad/null quality. All network traffic remains on loopback. The large topology check is not a sustained capacity benchmark.

PostgreSQL integration is optional in the default suite and explicitly reported as skipped without its connection variable. Use an **isolated test database**, because retention tests drop expired partitions:

```bash
# Set GATEWAY_TEST_POSTGRES and Gateway__PostgresConnectionString securely first.
dotnet test Gateway.sln
python scripts/smoke.py --postgres
```

See [API details](api.md) and [validation results](validation.md). No UI, physical-hardware acceptance, full SEMI E30 certification or production throughput guarantee is claimed.
