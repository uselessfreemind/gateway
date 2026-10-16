# Native SCADA engine and WinUI 3 client

## Scope and current readiness

The backend is C++20. Its Linux build, real loopback PLC protocol tests, archive operations, equipment-side HSMS transactions and the C ABI used by the UI are tested. The **WinUI 3 C++/WinRT project is source-complete for the included monitoring screens but has not been compiled or launched on Windows** in this environment. Run the Windows build/CI and desktop acceptance checks before treating the UI as ready.

A 10 ms period is accepted and scheduled using steady-clock deadlines; it is **not a guaranteed hard real-time cycle**. In the measured 500-controller × 700-tag simulated workload the archive was overloaded and the 10 ms end-to-end requirement was not met. This is an initial implementation, not a production acceptance claim for that workload. Exact CJ2/NX models, Ethernet units, firmware, layouts and factory-host requirements still need commissioning.

## Build and run

### Linux portable engine

```bash
bash native/scripts/build-linux.sh
native/build/scada_gateway --demo --controllers 2 --tags 20 --interval 10 --run-for 5
native/build/scada_gateway --config /absolute/path/site.json
```

CMake 3.24+, C++20 GCC/Clang, make and Tcl are required. Python 3 enables integration tests. The build downloads standalone Asio 1.30.2, nlohmann/json 3.11.3 and official SQLite 3.50.4 sources with pinned SHA-256 hashes. SQLite's official source generates its own amalgamation using make/Tcl; no downloaded binary is trusted without TLS/hash verification.

The CLI runs until Ctrl+C, or for the specified `--run-for` seconds. `--summary result.json` writes final statuses, latest samples, runtime archive counts and persistent row count. `--database path` overrides the database. The demo generates sine-wave values in process; use protocol integration tests for actual wire-code validation. Relative database paths resolve against the process working directory.

### Windows WinUI 3 desktop

Install Visual Studio 2022 with **Desktop development with C++**, **Windows application development / WinUI C++ tools**, MSVC v143 and Windows SDK 10.0.22621 or newer; also provide CMake, Git, NuGet CLI and Python 3 on PATH. The Visual C++ x64 runtime is needed on deployment machines.

```powershell
./native/scripts/build-windows.ps1 -Configuration Release
./native/winui/out/x64/Release/Scada.Desktop.exe
```

The script obtains vcpkg at tag 2025.06.13 unless `VCPKG_ROOT` is already provided. Its manifest baseline is fixed at `ef7dbf94b9198bc58f45951adcf1f041fcbc5ea0`. Existing vcpkg checkouts are reused without resetting them. CMake builds `scada_engine.dll` and its C import library for x64, runs CTest, then NuGet/MSBuild restores and compiles the unpackaged, self-contained Windows App SDK project. Pinned UI dependencies: Windows App SDK 1.7.260224002 and C++/WinRT 2.0.250303.1. No .NET runtime is required by the native engine/client.

**Run demo** starts 2 × 20 simulated tags at a 10 ms target, stored under `%LOCALAPPDATA%\OmronScada\scada-demo.db`. To use hardware, enter an absolute UTF-8 site configuration path and select **Start site**. The JSON example contains placeholder factory addresses and is never started automatically. Stop/window close drains accepted archive samples on a background thread. GUI refresh is 5 Hz independently of acquisition; rendering all values at 100 Hz would waste resources and is not required to acquire at 100 Hz.

Select a controller to see its current tag values, quality and units. Select a tag and minute/hour/day/month range for archived trends. The chart shows mean lines and min/max whiskers, and breaks lines at missing/bad-quality intervals. Scan duration, scheduling jitter, missed deadlines, failed reads, archive backpressure and HSMS connection state are visible. Bad/Stale values are gaps, never fabricated zeroes.

## Acquisition design

- One coroutine and strand per controller, sharing a configurable Asio I/O pool (default four threads). UDP/TCP requests and their cancellation deadlines run on that same strand. There are no overlapping reads for a single controller and no one-thread-per-controller dependency.
- Persistent CIP sessions, bounded frame parsing, correlation checks and automatic reconnect on the next scan. A grouped read has a configurable `timeoutMs`; a failed transport marks remaining tags in that scan Bad instead of issuing hundreds more timeouts.
- Read plans and CIP multiple-service packets are compiled before I/O starts, avoiding repeated symbolic-path parsing on the acquisition threads.
- Absolute steady-clock scan scheduling. Missed periods are counted/skipped instead of accumulating overlapping scans. On Windows the engine requests 1 ms timer resolution while running and restores it on stop; Windows scheduling, PLC response time and I/O still prevent hard real-time guarantees.
- CJ2 default: FINS **UDP** Memory Area Read `0101`, D/CIO/W/H/A areas, bits and 16/32/64-bit numeric values. Reads group nearby addresses up to 400 words/bits. Set source/destination routing fields and verify `lowWordFirst` against a known PLC value. FINS/TCP is not implemented.
- NX default: EtherNet/IP TCP RegisterSession/SendRRData, scalar symbolic Read Tag `0x4C` and Multiple Service Packet `0x0A`. BOOL, INT, UINT, DINT, UDINT, REAL/LREAL, structure members and one-dimensional array indices. Conservative 480-byte request/response budget. Unsupported types and per-tag errors retain Bad quality.
- No PLC writes, mode changes or downloads. No implicit/cyclic EtherNet/IP I/O, EDS-based assembly mapping, backplane routing, fragmented tags, strings or whole structures/arrays. Exact NX/CJ2 unit support must be verified on physical devices. NX variables must be configured for network visibility with matching types.
- Current values are published immediately after a scan's reads; archival admission is separate and bounded. FIFO archive tickets prevent a fast controller from repeatedly overtaking another ready scan under pressure. While waiting for archive space a controller yields its coroutine; the I/O pool continues servicing other connections. Backpressure increases the scan duration and missed-deadline counters. No archival decimation/deadband is silently applied.
- Timestamps are gateway UTC read-completion times, not PLC source timestamps. A grouped or multi-request scan is not a simultaneous atomic snapshot of all PLC memory.

## Archive and retention

SQLite WAL transactions, independent read-only viewer connections, a dedicated writer, up to 10,000-row batches, a bounded FIFO admission queue, indexes by controller/tag/time and time-only retention. The queue must fit at least one controller's scan. The live cache is separate from committed history; UI trend queries read committed rows. On archive failure the error is surfaced and further acquisition is stopped rather than silently continuing without persistence.

Every accepted sample (including Bad quality) is stored. Physical deletion runs at startup and hourly using **UTC minus one calendar month**, with day-of-month clamping at month boundaries. Queries immediately exclude expired rows. Freed SQLite pages are reused; the file is not continuously vacuumed/shrunk. Backups require their own retention rules. The queue is in memory, so abrupt termination can lose uncommitted samples. SQLite uses synchronous=NORMAL; a sudden power loss can also lose recent commits, although WAL recovery preserves database consistency. Graceful stop drains accepted samples; reads still awaiting queue admission when stopped are not accepted archive records. Live snapshots, archive records and host reports have deliberately different timing.

History is insertion-ID paginated (limit 1–10,000); trends return total/good counts and min/max/average for up to 2000 epoch-aligned buckets. C ABI functions return UTF-8 JSON and must be freed with `scada_free`; do not free DLL allocations in the caller's CRT. Handles must outlive their calls. `scada_stop` is idempotent and joins acquisition/drains the archive before return. Concurrent calls to start/stop/close on one handle are not supported; snapshots/queries may run alongside acquisition.

### The 500 × 700 × 100 Hz limit

That workload is **35 million samples/second**, or **90.72 trillion samples over 30 days**. Even an illustrative 40 bytes/sample would consume roughly 3.6 PB, before larger indexes/replication. Explicit CIP messaging would require many request/response round trips per NX each 10 ms; PLC servicing limits can dominate regardless of the PC implementation.

The SQLite row archive is a baseline for development/moderate workloads and **cannot sustain this full target in the tested environment**. The code reports that overload instead of pretending the target was achieved. Production work needs an agreed critical-tag set/sample policy or a suitably provisioned partitioned/compressed archive, plus network/PLC capacity tests. For NX 10 ms data, configured cyclic input assemblies/tag data links are preferable to many explicit reads; implementing that path requires the actual assemblies/connection parameters and tag layouts. Do not assume polling read-only symbolic tags provides cyclic I/O performance.

## Equipment-side SECS/GEM subset

HSMS-SS TCP Active/Passive, one host connection, Select/Deselect/Separate/Linktest, 1 MiB frame limits, system-byte matching, T3/T6 transaction expiry, T5 reconnect delay, T7 selection deadline and a bounded T8 partial-frame/write deadline. T8 is applied to a complete partial frame, stricter than a reset-per-character timer. Linktests occur every 30 seconds. Queued writes are serialized.

Implemented primary/secondary pairs: S1F1/2, S1F13/14 (communication establishment), S1F3/4 (status values), S1F11/12 (names/units), S1F15/16 (offline), S1F17/18 (online), S2F33/34 (define reports), S2F35/36 (link CEID 1001), S2F37/38 (enable), equipment S6F11 with host S6F12 acknowledgement. S2F41 remote commands are rejected with HCACK 1. Unsupported/malformed W-bit primaries are aborted with SxF0.

Identifiers are scalar U4; SVIDs are globally unique positive 32-bit IDs. Values are scaled F8 (BOOL is 0/1); unavailable, Bad or stale values use L[0]. The host must agree to this data dictionary. MDLN `OmronNative`, SOFTREV `0.1.0`. Report setup follows the shapes documented in the original [host interface](../docs/host-interface.md); supported message semantics match except MDLN and native millisecond timer configuration.

CEID 1001 is a periodic **current-value snapshot**, not a lossless transmission of every archive row. Establish communications/online, define reports, link event 1001 and enable it to begin S6F11. One outstanding event waits for S6F12 before the next. Definitions, links, event enablement and online state are connection-local; the host must reinitialize after reconnect. No persistent GEM state/spooling, alarms, equipment constants, process programs, trace commands or full E30 LOCAL/REMOTE state model. **Full GEM compliance/certification is not claimed.** Obtain the host's required message set and acceptance scripts before integration.

## Testing and operations

```bash
ctest --test-dir native/build --output-on-failure
native/build/scada_gateway --demo --controllers 500 --tags 700 --interval 10 \
  --run-for 5 --database native/build/stress.db --summary native/build/stress.json
```

CTest includes core codec/bounds/calendar retention/FIFO tests, real FINS/CIP wire simulators plus active/passive HSMS report exchange, and the actual UI-facing DLL C ABI. The WinUI XML can be parsed on Linux, but compilation and interaction need Windows. See [validation results](VALIDATION.md).

Keep industrial protocols on an approved isolated network; these protocols do not provide encryption/authentication here. Back up the database and tag dictionary, monitor storage/retention errors, synchronize the PC clock, provision service supervision and validate graceful/crash restart behavior. This is monitoring software, not a safety PLC or control loop. No network policy is broadened automatically to factory destinations.
