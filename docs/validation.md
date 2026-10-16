# Validation record

Validated in the cloud workspace on 2026-10-16 using .NET SDK 8.0.426, runtime 8.0.32, Linux x64. No physical PLC or factory host was connected. CI configuration was written but has not been run on GitHub.

| Check | Observed result |
|---|---|
| Locked dependency restore and Debug build through `bash scripts/install.sh` | Passed; zero warnings and errors |
| Automated suite with isolated PostgreSQL enabled | **23 passed, 0 failed, 0 skipped** |
| Small SQLite end-to-end run: 2 controllers × 700 tags, 1000 ms interval | Passed; 1400 current Good values, archive and trends, authentication, real SignalR delivery, HSMS selection/communication/status; FINS disconnect changed values to Bad/null |
| Small PostgreSQL end-to-end run: 2 × 700 tags | Passed; same acquisition, archive, viewer and host paths |
| Maximum SQLite topology: 500 × 700 tags, 10000 ms interval | Passed; 350,000 current Good values; final run approximately 12.1 seconds, 351,400 committed samples at the health snapshot, queue depth zero; all 350,000 tag identities confirmed in SQLite after graceful shutdown |
| Maximum PostgreSQL topology: 500 × 700 tags, 10000 ms interval | Passed; 350,000 current Good values; approximately 14.1 seconds, 506,800 committed samples and queue depth 100,000 at the health snapshot |
| Default configuration using `scripts/run.sh` | Started; `/health` 200; `/health/ready` 503 as expected because no physical controllers are configured |

The automated tests exercise FINS address limits, response correlation, end codes, word order, timeout cancellation and grouped 700-tag reads; CIP paths, type checks, registration and multiple-service replies; 500-controller configuration limits; bounded queue backpressure; SQLite transactions, pagination, trends and expiry; PostgreSQL binary COPY, daily partition creation/deletion and boundary retention; SECS item parsing limits; equipment report setup, stale-data handling, and rejection of remote commands; HSMS TCP selection, status requests and S6F11/S6F12 event acknowledgement; and active-selection timeout handling.

The original maximum-topology attempt exceeded its startup deadline because IConfiguration binding scanned a very large tag-key tree. Large site files now use direct JSON deserialization. A lightweight status endpoint avoids returning the whole tag catalog on each monitoring request. Both maximum-topology runs passed after those changes.

These are short simulator integration checks, not sustained benchmarks. The Python simulator, archive database and gateway share the same machine. All 500 logical controllers use two loopback protocol simulators; this does not emulate 500 independent physical network paths or PLC CPU loads. The topology uses 250 FINS controllers with contiguous UInt16 words and 250 EtherNet/IP controllers with scalar REAL tags, 700 tags per controller. Hardware packet limits, individual Omron firmware behavior, routed networks and long symbol names were not validated.

Queue pressure is observable and intentional: an earlier SQLite burst and the PostgreSQL maximum run filled the 100,000-sample test queue, applying acquisition backpressure. The code does not guarantee 1-second scans across all 350,000 tags, simultaneous atomic PLC snapshots, or a production storage rate. Live and SECS event delivery are current-data interfaces, not guaranteed lossless transmission of every archive row.

Outstanding commissioning: load the real CJ2/NX tag maps, verify FINS routing and word order, confirm NX Read Tag/Multiple Service support, agree required scan intervals and archive capacity, run sustained load and failure tests, and validate the documented HSMS/SECS subset against the factory host. Full SEMI E30 compliance is not claimed. UI implementation remains deferred.
