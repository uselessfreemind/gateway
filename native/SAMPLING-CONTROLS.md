# Connection monitoring, sampling controls and trends

## Retry policy

Each configured controller exposes `connectionState`, `retryAttempts` and `connectionError` in snapshots. States are Connecting, Connected, Reconnecting and Aborted. FINS/UDP status means successful read responsiveness; UDP has no connection handshake. Individual tag failures do not imply loss of transport.

The failed initial scan is followed by up to three reconnect attempts, at least one second apart. A transport-successful scan resets the budget. After the third failed reconnect, the controller remains Aborted with no PLC requests until manual retry or global sampling restart. Other controllers continue normally. Socket recovery uses the configured controller protocol without issuing PLC writes.

The WinUI connection list shows every controller (scroll to inspect long lists). Select a controller and press **Retry selected controller**, or press **Restart all sampling**. Existing Stop still stops the whole engine and drains archives.

C ABI additions:

- `scada_retry(handle, controllerId)` resets that controller's budget, closes its socket and wakes its coroutine.
- `scada_restart_sampling(handle)` broadcasts the same request to every controller, including aborted controllers.
- Return 0 means accepted, -1 means error; inspect `scada_last_error()`.

Controls run on the controller strands so an old read cannot overlap its replacement. Waiting archive-admission tickets can be cancelled without blocking the FIFO for other controllers; accepted archive records are preserved. Storage and HSMS stay running. Cancelled unaccepted scans are not guaranteed to be archived. Restart is an immediate asynchronous request, not a hard real-time guarantee of simultaneous network packets. Do not call controls concurrently with engine start/stop/close; snapshots and acquisition may run concurrently.

## Trend selection already implemented

The native UI already lets the operator select a controller/tag and minute/hour/day/month range, refreshing committed min/max/average trends at 5 Hz. This existing functionality is retained rather than duplicated. Tag selection now excludes Boolean values; current values include their configured CIP symbol or FINS memory address. Snapshots also include `address` and `analogue` metadata. The selected tag/range is the current trend registration, retained until selection or engine session changes. Arbitrary unconfigured address reads and multiple overlaid native charts are not implemented.

These charts use committed archive records, so they can lag current acquisition during archive pressure. They do not claim to render every 10 ms sample on screen. Bad/missing intervals remain gaps.

## Validation

CTest includes `scada_sampling_controls`, exercising silent FINS and disconnected CIP, exactly four failed attempts, indefinite pause, isolated manual retry, global recovery and address metadata through the actual UI-facing C ABI. Queue unit coverage includes cancelled FIFO tickets. The existing protocol, archive and UI bridge suites remain required. WinUI compilation is checked by the branch's Windows CI; interactive GUI acceptance still requires Windows hardware.
