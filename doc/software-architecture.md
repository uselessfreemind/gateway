
# Omron SCADA | Software structure and principles

C# / .NET 8 web-viewer backend • Architecture reference • 18 October 2026

Repository: uselessfreemind/gateway | Branch: main

Source baseline: f934521c77ac0cac4c7eaafc177b580938e14e06

## Purpose and boundary

Read-only monitoring of Omron CJ2 and NX controllers; timestamped current values, trend queries, one-calendar-month archives, and equipment-side data exchange with a factory host. The intended topology is up to 500 controllers with roughly 300–700 tags each.


Omron CJ2 / NX controllers — FINS/UDP memory reads | EtherNet/IP TCP explicit tag reads
   |
Acquisition engine — Grouped reads, deadlines, correlation, scaling and quality
   |
Current cache + bounded archive pipeline — Latest values for monitoring; committed records for history
   |
Viewer and factory host — REST / SignalR for future web UI | equipment HSMS-SS

## Read this distinction first

This branch implements the backend only; a browser UI has not been built. Its minimum configured poll interval is 20 ms. The later C++/WinUI 3 implementation is a separate branch.

SECS/GEM functionality is a documented subset. Neither complete SEMI E30 compliance nor physical-controller acceptance is established by simulator tests.


# 1 | Components and execution model

## Runtime structure

ASP.NET Core hosts acquisition, archive writing, retention, REST/SignalR and HSMS as cooperating background services. Each controller has an asynchronous scan loop. A global semaphore limits concurrent scans (default 64); a controller never overlaps its own scans.


Site JSON -> Gateway configuration — Direct JSON loading handles large tag catalogs; validate before start
   |
Independent asynchronous controller loops — Staggered starts; bounded global scan concurrency
   |
Latest cache + bounded Channel<Sample> — Queue admission waits when storage cannot keep up
   |
IArchiveStore + viewer and host services — SQLite or PostgreSQL; committed updates delivered through SignalR

## Source map

src/Gateway/Program.cs and Configuration.cs compose the service. Acquisition/ handles polling; Protocols/ contains FINS, EtherNet/IP and read planning; Archive/ implements storage and retention; Secs/ implements SECS items, HSMS framing and host state.

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
Single writer commits a batch — SQLite WAL or PostgreSQL binary COPY with daily partitions
   |
History and trend readers — Committed records; cursor paging and time-bucket aggregates
   |
Retention boundary: UTC now minus one calendar month — Queries hide expired rows immediately; physical purge startup/hourly

Month subtraction clamps the day at shorter months: 31 March maps to 28 February, or 29 February in a leap year. This is a calendar-month policy rather than a fixed 30-day expiry. Bad-quality samples are also archived, preserving evidence of communication failures.

SQLite WAL is the default; PostgreSQL is selectable through IArchiveStore. SQLite uses transactional writes and indexes. PostgreSQL uses binary COPY into daily range partitions, drops fully expired partitions and deletes boundary rows. The archive writer batches up to 5,000 samples.

SQLite deletes expired rows in chunks and reuses freed database pages; automatic deletion does not continuously shrink the database file. Backups need an independent expiry policy. History uses insertion-ID cursors, with at most 10,000 rows per page. Trends return min/max/mean and good/total counts in at most 2,000 buckets.

## Durability and shutdown

The queue is not a durable outage spool. Abrupt termination can lose queued data; synchronous=NORMAL can lose recent SQLite commits on power failure. Archive failure stops further acquisition instead of silently running without storage. Graceful shutdown cancels acquisition and drains accepted queue entries, subject to the host’s 30-second shutdown timeout.


# 4 | Viewer and factory-host interfaces

## Viewer structure

The future browser viewer can obtain catalog/status/current data from /api/controllers, /api/status and /api/latest, page raw values with /api/history/{controller}/{tag}, and request aggregates with /api/trend/{controller}/{tag}. /health/ready reports whether configured controllers supply good data.

SignalR /live supports controller subscriptions and emits samples after archive commit. It is not a durable message stream; viewers must resynchronize using history after a gap. Optional X-Api-Key protection also applies to transport requests. A rendered web UI is not included in this branch.

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

The recorded .NET suite passed 23 tests with PostgreSQL enabled. Small 2 x 700-tag runs checked real loopback FINS/CIP, SQLite/PostgreSQL archives, trends, API authentication, SignalR and HSMS. Maximum-topology checks used 500 x 700 tags at a 10,000 ms interval and verified all tag identities; they were short functional runs, not sustained timing benchmarks.

Even at one scan per second, 350,000 tags produce 907.2 billion samples over 30 days. An illustrative 40 bytes/sample is about 36 TB before indexes or replication. Controller count alone cannot size storage or guarantee throughput. Physical PLCs, a production host and a rendered browser viewer have not been accepted.

## Build and bring-up

Use .NET SDK 8.0.426: dotnet restore Gateway.sln --locked-mode; dotnet test Gateway.sln; dotnet run --project src/Gateway. Set GatewayConfig to an absolute site JSON path. Default startup has no controllers, HSMS disabled and HTTP on loopback:5080.

## Acceptance priorities

Verify actual CJ2/NX models, exposed tag types, FINS routing and word order. Agree the required sampling policy and host dictionary. Measure sustained scan/ingest/query rates and storage growth, then test network outages, database errors, restart and expiry. Synchronize gateway time and supervise the process. Keep industrial protocols on the approved factory network; their traffic is unencrypted here.

## Source and evidence index

README.md; docs/api.md; docs/host-interface.md; docs/validation.md; config/site.example.json; scripts/smoke.py; tests/Gateway.Tests/. The PDF describes the baseline printed on the cover; operational claims require validation against the deployed revision.
