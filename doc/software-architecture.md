
# Omron SCADA | Software structure and principles

C++20 engine and WinUI 3 desktop • Architecture reference • 18 October 2026

Repository: uselessfreemind/gateway | Branch: feature/cpp-winui3-scada

Source baseline: 91765b1080c95a6e33627be1046fbe31555a2662

## Purpose and boundary

Read-only monitoring of Omron CJ2 and NX controllers; timestamped current values, trend queries, one-calendar-month archives, and equipment-side data exchange with a factory host. The intended topology is up to 500 controllers with roughly 300–700 tags each.


Omron CJ2 / NX controllers — FINS/UDP memory reads | EtherNet/IP TCP explicit tag reads
   |
Acquisition engine — Grouped reads, deadlines, correlation, scaling and quality
   |
Current cache + bounded archive pipeline — Latest values for monitoring; committed records for history
   |
Viewer and factory host — WinUI 3 via C ABI | equipment HSMS-SS

## Read this distinction first

The native scheduler accepts a 10 ms target. The measured maximum workload did not sustain it. This is an asynchronous monitoring system, with observable missed deadlines, rather than a hard real-time control loop.

SECS/GEM functionality is a documented subset. Neither complete SEMI E30 compliance nor physical-controller acceptance is established by simulator tests.


# 1 | Components and execution model

## Runtime structure

scada_core is the portable C++20 library. scada_engine exports a shared-library C ABI to the WinUI 3 process. scada_gateway is an alternative command-line host for the same engine. The desktop embeds the engine; it does not call the legacy C# HTTP service.


Site JSON -> validated configuration — Compile memory read plans and CIP packets before I/O starts
   |
Asio I/O pool (default 4 threads) — One coroutine and strand per controller; persistent sockets
   |
Scan completion -> current cache — No overlapping scan for a controller; engineering values and quality
   |
FIFO queue -> dedicated archive writer — Bounded admission; SQLite transactions; independent reader connections

Absolute steady-clock deadlines determine the next scan. Missed periods are counted and skipped; work is not overlapped to catch up. Windows requests 1 ms timer resolution while running, but OS scheduling and PLC/network time still constrain delivery.

## Source map

native/src/engine.cpp: lifecycle, scheduling and queue admission; protocol.cpp: FINS/CIP wire clients; archive.cpp: SQLite and retention; secs.cpp: HSMS/GEM; c_api.cpp: DLL bridge; model.cpp: configuration and values. Public contracts are in native/include/scada/.

## Configuration principles

Controller IDs, addresses, tag types, scaling, units and globally unique positive SVIDs form the site dictionary. Validate routing, IPs and types before acquisition. Configuration changes require restart. Timestamp values are gateway UTC read-completion times; multiple read requests are not an atomic PLC snapshot.


# 2 | Read-only controller communication

## CJ2: FINS over Ethernet/UDP


Tag address -> grouped memory request — D / CIO / W / H / A; nearby addresses, at most 400 words or bits
   |
FINS Memory Area Read 0101 -> UDP endpoint — Configured source/destination network, node and unit
   |
Reply -> correlation and decode — Check service ID, end code, bounds and configured word order
   |
Engineering sample — raw x scale + offset; finite value or Bad/null

Supported values include bits, signed/unsigned 16/32-bit integers and 32/64-bit floating point. Confirm low-word-first settings using a known PLC value. FINS source nodes must be unique and reachable. FINS/TCP is not implemented.

## NX: EtherNet/IP explicit messaging

The client opens TCP, registers an encapsulation session, and uses SendRRData. CIP Read Tag (0x4C) requests are grouped using Multiple Service Packet (0x0A). Request and estimated response payloads use a conservative 480-byte budget. Session/context, packet structure and returned types are checked.

Supported symbolic targets are scalar BOOL, INT, UINT, DINT, UDINT, REAL and LREAL, structure members and one-dimensional array elements. NX variables must be exposed for network access with matching types. Implicit cyclic I/O, fragmented reads, routing, strings and whole structures/arrays are outside this implementation.

## Failure and quality

A request deadline bounds waiting. Transport failure marks remaining tags in the scan Bad and the next scan attempts recovery; a per-tag CIP error is isolated to that tag. Successful reads are scaled into engineering units. Nonfinite or unavailable values become null. Latest values become Stale when their age exceeds twice the polling interval plus the request timeout.

The wire clients issue read services only. Host remote commands are rejected, and no PLC write, mode-change or program-download path is provided. Real Omron CPU/Ethernet-unit firmware still requires commissioning.


# 3 | Data lifecycle, archive and retention


PLC reply -> current sample — Controller/tag IDs, UTC timestamp, engineering value, quality
   |
Bounded in-memory queue — Backpressure slows scans; no deliberate deadband or decimation
   |
Single writer commits a batch — SQLite WAL
   |
History and trend readers — Committed records; cursor paging and time-bucket aggregates
   |
Retention boundary: UTC now minus one calendar month — Queries hide expired rows immediately; physical purge startup/hourly

Month subtraction clamps the day at shorter months: 31 March maps to 28 February, or 29 February in a leap year. This is a calendar-month policy rather than a fixed 30-day expiry. Bad-quality samples are also archived, preserving evidence of communication failures.

SQLite uses WAL and synchronous=NORMAL. The dedicated writer batches up to 10,000 rows. FIFO admission tickets prevent a ready controller being repeatedly overtaken. Viewer queries use separate read-only connections and see committed transactions without holding the writer’s application mutex.

SQLite deletes expired rows in chunks and reuses freed database pages; automatic deletion does not continuously shrink the database file. Backups need an independent expiry policy. History uses insertion-ID cursors, with at most 10,000 rows per page. Trends return min/max/mean and good/total counts in at most 2,000 buckets.

## Durability and shutdown

The queue is not a durable outage spool. Abrupt termination can lose queued data; synchronous=NORMAL can lose recent SQLite commits on power failure. Archive failure stops further acquisition instead of silently running without storage. Graceful shutdown cancels acquisition and drains accepted queue entries. Pending reads not yet admitted are not archived.


# 4 | Viewer and factory-host interfaces

## Viewer structure

The C++/WinRT WinUI 3 application calls scada_open/scada_demo and reads UTF-8 JSON through scada_snapshot, scada_history and scada_trend. The opaque engine handle must outlive calls; returned strings must be released by scada_free. Stop is idempotent; start/stop/close must not run concurrently on a handle.

A 5 Hz dispatcher refresh is independent of the 10 ms acquisition target. Background work owns the engine through RAII; busy gates avoid overlapping refreshes and generation checks discard results from obsolete selections. Closing the window waits asynchronously for engine stop and archive drain.

Controller selection limits current-value display. Tag and minute/hour/day/month controls choose archive queries. The Canvas chart plots the mean, min/max whiskers and gaps for bad or missing data. Scan time, jitter, missed deadlines, failed reads, queue pressure and HSMS state explain freshness.

## Equipment-side HSMS / SECS sequence


TCP connection -> HSMS Select — Active or passive; one host; bounded frames and correlated system bytes
   |
Host establishes communications and online state — S1F13/F14 then S1F17/F18
   |
Host configures reports and event 1001 — S2F33/F34 -> S2F35/F36 -> S2F37/F38
   |
Equipment sends periodic S6F11 W — Current-value snapshot; one outstanding event; host ACK S6F12

S1F3/F4 reads values; S1F11/F12 returns names/units; S1F1/F2 identifies software; S1F15/F16 goes offline. S2F41 remote commands receive rejection HCACK=1. IDs are U4, values are engineering F8 (BOOL 0/1), and unavailable/Bad/Stale values are L[0].

T3/T6 bound transactions, T5 delays reconnect, T7 bounds selection and T8 bounds a partial frame; linktests monitor liveness. Report definitions and online state reset after reconnect. Events are snapshots, not a lossless archive transfer. Alarms, spooling, programs, constants and full E30 state behavior are outside the subset.


# 5 | Capacity, validation and operation

## What the measurements establish

The recorded 500-controller simulation used 250 FINS and 250 CIP clients, 700 tags each, with a 10 ms target. All 350,000 tag identities were acquired and archived with zero failed reads. However, it recorded 367,870 missed periods, 674,577 backpressure retries and a maximum last-scan duration near 1,284.6 ms. The target timing was not achieved.

It committed 2,226,000 samples in 14.57 seconds including initialization, acquisition, drain and validation. Two shared loopback simulators do not model 500 physical PLC CPUs or network paths. At 500 x 700 x 100 Hz the requirement is 35 million samples/second, or 90.72 trillion samples in 30 days; an illustrative 40 bytes/sample already means about 3.6 PB before indexes.

Core, wire-protocol and C ABI CTest suites passed. GitHub Actions run 38994549265 at baseline 91765b1 also passed Linux validation and Windows native tests plus the WinUI 3 build. This supersedes the earlier Windows-build caveat in native/README.md and native/VALIDATION.md. Interactive desktop use remains unverified.

## Build and bring-up

Native: bash native/scripts/build-linux.sh; on Windows run ./native/scripts/build-windows.ps1 -Configuration Release. Start the desktop demo before supplying an absolute site JSON path. The native README describes pinned dependencies and the Windows prerequisites.

## Acceptance priorities

Verify actual CJ2/NX models, exposed tag types, FINS routing and word order. Agree the required sampling policy and host dictionary. Measure sustained scan/ingest/query rates and storage growth, then test network outages, database errors, restart and expiry. Synchronize gateway time and supervise the process. Keep industrial protocols on the approved factory network; their traffic is unencrypted here.

## Source and evidence index

native/README.md; native/VALIDATION.md; native/config/site.example.json; native/tests/; .github/workflows/native.yml; native/winui/Scada.Desktop/. The PDF describes the baseline printed on the cover; operational claims require validation against the deployed revision.
