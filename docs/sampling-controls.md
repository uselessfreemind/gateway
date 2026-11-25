# Sampling controls and live trends

Each controller exposes Connecting, Connected, Reconnecting, Aborted and Restarting status through `/api/connections` and the web viewer. FINS status measures read responsiveness, not a UDP handshake. Tag errors do not automatically mean a disconnected transport.

After an initial transport failure, three automatic reconnect scans are allowed, separated by at least one second. A successful transport scan resets the retry budget. Failure of all three retries pauses polling indefinitely. Manual retry in the controller monitor starts a new budget. It is also available as `POST /api/controllers/{id}/retry`. Global restart uses `POST /api/sampling/restart`: cancellation is broadcast to all loops, and each disposes its old client before starting another, without restarting storage or HSMS. It also releases aborted controllers. This requests immediate cancellation; operating-system scheduling and controller response times cannot guarantee simultaneous packets. Samples already accepted for archival are preserved; unaccepted work can be cancelled. Retry/restart counters reset with each new sampling session.

POST requests require `X-Sampling-Control: 1` to prevent cross-origin form submissions, in addition to the existing optional `X-Api-Key`. No CORS access is enabled. These operations only change gateway sampling and never write PLC memory.

The viewer's new live monitor loads the configured controller/tag catalog. Choose an analogue tag by ID and its CIP symbol/FINS address, then Register trend (up to eight). It charts the latest distinct real samples, polls once a second, retains 120 displayed points and shows Bad/Stale values as gaps. BOOL tags are excluded. This is a display-rate trend, not every acquisition sample; the existing archive/trend APIs provide committed historical aggregates. Registration lasts for the current browser session. Arbitrary unconfigured addresses cannot be read from the browser.

The older System B illustration remains explicitly simulated. The new controller monitor and registered charts use only real backend data. No factory address is contacted unless configured.
