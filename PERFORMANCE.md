# Headless Linux native capture and navigation

Live validation: 2026-10-11, Docker Desktop Linux/amd64 on the development
Windows host, Debian slim/Xvfb, pinned Pepper 32.0.0.371, 1280x800 at low quality.
The authenticated account stayed on map 2-2 with automation disabled. Measurements
used the real native mapping, command socket and Rust GameClient, without STATE
or REFERENCE polling in the production capture path.

| Measurement | Scalar native reads | Batched native reads |
|---|---:|---:|
| Native capture median | 467 us | 182 us |
| Native capture p95 | 896 us | 266 us |
| Native capture samples | 1072 | 1093 |
| Flash ENTER_FRAME FPS median | 60.0 | 59.94 |
| Flash FPS sampled range | 47.86–61.26 | 48.95–60.64 |
| Native publication rate | 54.42 Hz | 54.61 Hz |
| Observed snapshot age median / p95 | 1 / 3 ms | 1 / 2 ms |
| Rust GameClient::status median | 30.95 us | 22.38 us |
| Rust GameClient::status p95 | 68.87 us | 42.43 us |

The optimized status test had 4241/4241 ready samples. Native capture and status
read latency are different measurements; neither is the client FPS. The earlier
40 Hz observation described publication cadence, not 40 us of capture cost.
Capture is approximately 2.6 times faster with batching. It remains above the
user-reported Windows capture cost of 40 us; this test does not establish parity.
Serial live samples had 5–6 entities (scalar sample: 6); scheduling, rendering
and changing game state affect the results. This is a quiet-map measurement,
not a dense combat benchmark. Warm-up and loading can reduce FPS substantially.
Native timing now uses microseconds instead of rounding to whole milliseconds.

## Geometry

The shared injected bridge observes NOA circles, rectangles, polygons and
inverted zones from the game's public properties. Pepper caches navigation on
the Flash game thread every 300 ms; the host pulls that cached observation and
publishes an atomic, private sidecar file. Native hot capture joins it only when
its map matches and its original observation timestamp is no older than 750 ms.
The per-capture memory cache uses fault-safe page copies and never retains AVM
bytes across captures. Hot positions/health remain native memory reads.

The live map had no NOA obstacles, so a valid empty geometry list was delivered
with three portals. Rust deserialization confirmed geometry_ready=true. Shape,
bounds, inversion, stale/map-mismatched/truncated sidecars, inaccessible pointers
and cross-page reads have regression coverage. The runtime's existing obstacle
routing test confirms a rectangle detour and rejection of unavailable geometry.
A live CAPTURE 0/1 fault test confirmed that stale geometry changes to not-ready
while native position frames continue, then fresh geometry recovers. This did
not move the account through a live obstacle or start an objective module.

## Reproduce

Build/run using the sibling botd README. With automation disabled:

```sh
docker cp xenomit-botd/container/perf-probe.js BOT_CONTAINER:/tmp/perf-probe.js
docker exec -e ELECTRON_RUN_AS_NODE=1 -e XENOMIT_PERF_WARMUP_MS=30000 BOT_CONTAINER \
  /opt/xenomit-flash-client/browser/node_modules/electron/dist/electron /tmp/perf-probe.js
```

The probe samples for 20 seconds after warm-up; XENOMIT_PERF_SAMPLE_MS changes
that interval. Start a separate serial run with XENOMIT_FLASH_SCALAR_READS=1 to
compare the old field-by-field native reads. Do not enable that diagnostic in
normal use. METRICS returns measured Flash FPS and navigation cost.
Compile botd's examples/flash_benchmark.rs on Linux and run it in the container
with the host PID from /tmp/xenomit_flash_PID.sock to measure the actual Rust
status path and verify typed geometry. The probe contains no account credentials.

```sh
node browser/test/host-origin.test.js
node browser/test/navigation.test.js
ctest --test-dir build/cmake --output-on-failure
```
