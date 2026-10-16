# Native validation — 2026-10-16

Environment: Linux x64, GCC 14.2.0, CMake 4.4.4; SQLite generated from official 3.50.4 source. Physical Omron controllers and factory SECS hosts were not available. WinUI 3 compilation and execution require Windows and were **not performed** in this cloud environment.

| Check | Observed result |
|---|---|
| CMake Debug / Release native builds | Passed. GCC emitted an upstream SQLite optimized `strlen` warning in the Release build; no native application warnings were observed. |
| Release CTest | **3/3 suites passed**: core, wire protocols, UI C ABI. Latest run approximately 9.6 seconds. |
| AddressSanitizer + UndefinedBehaviorSanitizer | **2/2 selected suites passed**: instrumented native core/unit and full PLC/HSMS integration. UI C ABI was tested in the normal build, not loaded into the unsanitized Python host in this run. |
| 2 controllers × 700 tags, 10 ms target | FINS/UDP and CIP multiple-service values validated through actual loopback sockets; all tag identities persisted; clean shutdown row counts agree. Both HSMS active and passive sessions exchanged S1 status and host-defined, enabled S6F11 reports with S6F12 acknowledgement. Remote PLC commands rejected. |
| Silent FINS endpoint | Read timeout observed, Bad/null values archived, missed-deadline counters incremented, shutdown completed. |
| UI bridge | DLL startup/demo, filtered current data, history pagination, trends, idempotent stop, error return and DLL-owned allocation/free passed through Python ctypes. |
| WinUI project | XAML/project/manifest XML parsed successfully; pinned NuGet package versions confirmed available. This does not establish C++/WinRT compilation, runtime activation or usable GUI behavior. |

Core checks cover FINS read-only opcode/address bounds, word order and correlation, CIP paths and typed values, SECS nested parsing and HSMS frames, 500 × 700-tag configuration validation, leap-year calendar-month expiry, raw archive pagination, bad-quality values, trends, FIFO queue ordering, and concurrent viewers seeing whole committed transactions rather than partial writes.

## Maximum network topology experiment

Command (optional stress test, excluded from routine CTest):

```bash
python native/tests/scale.py native/build/scada_gateway
```

Final Release run, 250 FINS + 250 EtherNet/IP clients, 700 tags/controller, **10 ms requested interval**, 8-second acquisition window and graceful drain:

- **500 controllers completed scans; all 350,000 latest tag identities acquired.**
- **Zero failed reads**; all 350,000 tag identities verified in the archive.
- 2,226,000 accepted samples committed; 14.57 seconds total including initialization, drain and validation.
- At least 6 completed scans/controller.
- 367,870 missed periods across controllers; 674,577 backpressure retry events.
- Maximum last-scan duration approximately **1,284.6 ms**.

**The 500-controller, 700-tag, sustained 10 ms requirement failed this capacity experiment.** Passing topology/functional checks does not change that timing result. SQLite row insertion, sample construction/copying and scheduling pressure dominate this workload. The archive needs a different capacity design and the critical-tag/scan requirements need agreement before claiming production readiness. For physical NX devices, cyclic input assemblies/tag data links should be considered once the actual connection/layout details are available.

The Python fixtures represent 500 clients using two loopback endpoints, not 500 independent PLC CPUs and switches. The shared FINS simulator initially overflowed its default UDP receive buffer during concurrent startup; Linux UDP RcvbufErrors confirmed packet drops and only FINS clients were affected. Increasing that **test server's** receive buffer to represent 250 independent PLC buffers resolved the harness failures without weakening the zero-failed-read assertion. The native clients still use their configured timeouts.

A separate in-process simulated workload (500 × 700, 5 seconds, Debug build) also acquired all tag identities and completed at least two scans/controller, but incurred archive pressure and missed deadlines. This is a synthetic scheduling/archive check, not protocol or hardware performance evidence.

The build now precomputes scan plans and CIP packets before I/O begins, avoiding repeated symbolic-path parsing on the I/O threads and reducing startup contention. Viewers use separate read-only WAL connections so long archive queries do not hold the writer's application mutex.

## Remaining acceptance work

1. Build the WinUI desktop on Windows (script/CI provided), launch it, run the demo, inspect controller/tag selection and all chart ranges, and verify shutdown with pending archive work.
2. Test actual CJ2/NX Ethernet units and firmware, FINS nodes/routing/word order, exposed NX tag types, network loss/reconnect, and time synchronization.
3. Establish whether 10 ms applies to all 300–700 tags or a smaller critical set. Obtain NX cyclic connection/assembly layouts if that path is required. Measure sustained scan, PLC servicing, wire bandwidth and archive rates against the selected target.
4. Agree storage policy/capacity: 35 million samples/second at the maximum stated topology would produce 90.72 trillion rows in 30 days. The current SQLite baseline is insufficient for that target.
5. Validate the documented SECS/GEM subset against the real host's required messages and data dictionary. Complete any additional GEM capabilities before claiming compliance; no SEMI certification is claimed here.

GitHub workflow execution could not be inspected from this cloud instance: the GitHub API request returned Forbidden. A workflow file being present is not a successful CI run. Review the branch's Actions results after push.
