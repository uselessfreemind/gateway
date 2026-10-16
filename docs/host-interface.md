# Equipment-side SECS/HSMS interface, version 0.1

This gateway is equipment; the factory computer is the host. HSMS-SS runs on one TCP connection. Passive listens on `Secs.Host:Port`; Active connects there and selects. Only one host is served at a time. The default binds to loopback. Set DeviceId to the host's configured 0–32767 session ID.

## Implemented messages

| Host request | Equipment response / behavior |
|---|---|
| S1F1 | S1F2 `L[A MDLN, A SOFTREV]` |
| S1F13 `L[]` | S1F14 `L[B COMMACK=0,L[A MDLN,A SOFTREV]]`; establishes communications |
| S1F3 `L[U4 SVID...]` | S1F4 `L[value...]`; empty requested list selects all SVIDs |
| S1F11 `L[U4 SVID...]` | S1F12 `L[L[U4 SVID,A SVNAME,A UNITS]...]`; empty list selects all |
| S1F15 | S1F16 B OFLACK=0, stops event transmission |
| S1F17 | S1F18 B ONLACK=0, allows event transmission |
| S2F33 `L[U4 DATAID,L[L[U4 RPTID,L[U4 SVID...]]...]]` | S2F34 B DRACK; define reports. Empty definitions clears all reports/links. Empty VID list removes that report. Delete before redefining an existing report. |
| S2F35 `L[U4 DATAID,L[L[U4 CEID,L[U4 RPTID...]]]]` | S2F36 B LRACK; link reports to CEID 1001. Empty outer list clears links. |
| S2F37 `L[BOOLEAN CEED,L[U4 CEID...]]` | S2F38 B ERACK; enable/disable event 1001. Empty CEID list applies to all supported events. |
| S2F41 | S2F42 `L[B HCACK=1,L[]]`; remote commands are rejected, protecting read-only PLC access |
| S6F12 B ACKC6 | Acknowledges equipment-originated S6F11; a nonzero ACK is logged |

Unsupported W-bit primary requests receive SxF0. Malformed supported requests are aborted. This implementation accepts U4 identifiers only; negotiate this data dictionary with the host. Other SECS item encodings and the full message universe are not implemented.

Each configured tag has a globally unique positive **SVID**, an ASCII display name `controllerId.tagId`, and units. Values are engineering-scaled **F8** scalars, including BOOL represented as 0/1. Unknown, unavailable, Bad or stale data uses **L[0]**, not a fabricated zero. Agree this convention with the factory host. For very large catalogs, request SVIDs in batches to stay within the one-MiB HSMS frame limit.

**CEID 1001: periodic current-data snapshot.** After S1F13, S1F17, S2F33, S2F35 and S2F37, equipment sends:

```text
S6F11 W
L[3]
  U4 DATAID=0
  U4 CEID=1001
  L[number of linked reports]
    L[2]
      U4 RPTID
      L[number of report values]
        F8 value (or L[0] if unavailable)
```

`EventIntervalMs` defaults to 1000. There is at most one outstanding S6F11; the host must respond within T3. Transmission intervals can lengthen while awaiting replies. Events convey current values, not a lossless feed of every archived sample. Retrieve full historical data through the archive API. Report definitions, links, enabling, communication state and online state reset on reconnect; the host must reinitialize them. No store-and-forward event spool is implemented.

HSMS supports Select.req/rsp, Deselect.req/rsp, Linktest.req/rsp and Separate.req, system-byte correlation, frame-size limits, selection timeout T7, transaction deadlines T3/T6, reconnect delay T5 and a bounded frame transfer using T8. T8 is enforced as a whole partial-frame deadline, which is stricter than resetting an inter-character timer. Linktests run every 30 seconds while selected. Connection loss or transaction expiry reconnects after the configured delay.

This is a documented **SECS/GEM subset**, not a complete SEMI E30 implementation or certification. Alarm management, equipment constants, trace setup, process programs, spooling, terminal messages, clock synchronization, persistent GEM state and the complete GEM control-state model are outside this version. The supported offline/online flag gates data events; it does not model all LOCAL/REMOTE and equipment-initiated transitions. Obtain the factory host's required message set and acceptance scripts before claiming interoperability or compliance.
