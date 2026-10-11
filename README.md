# Xenomit Flash Client for Linux

Linux x64 host for Pepper Flash. It loads the game movie, publishes the same version 2 live frame as the Windows client's native memory capture, and serves the Flash command bridge on `/tmp/xenomit_flash_<pid>.sock`.

## Requirements

- Linux x64, X11 or XWayland, and a C++17 toolchain with CMake
- pnpm, for the Electron 11 host
- The pinned Pepper plugin at `runtime/libpepflashplayer.so` (PPAPI Flash `32.0.0.371`). The SHA-256 in `runtime/pepper.sha256` must match. The plugin is not part of this repository.

## Build

```bash
bash scripts/build.sh
pnpm install --dir browser
chmod +x xenomit_flash_client
```

## Run

`xenomit-bot` on Linux starts `xenomit_flash_client` with `--launch-stdin --backend native` and writes the same six bootstrap records as the Windows host: movie URL, page base, session id, FlashVars, width, height. The launcher creates `/dev/shm/xenomit_state_<pid>` (or `/tmp` when `/dev/shm` is unavailable) and preloads `libxenomit_flash_hook.so`.

`XENOMIT_FLASH_CLIENT_EXE` selects another launcher. `XENOMIT_FLASH_PLUGIN_PATH` selects another copy of the pinned plugin. `XENOMIT_FLASH_KEEP_VISIBLE=1` keeps the window mapped.

The hook walks AVM objects on the plugin game thread and writes the frame. `connect_flash_native` reads that file for snapshots and opens the command socket for everything else: reflected `STATE`, `INSPECT`, `INVOKE` / `FINVOKE`, `gameCommand`, `clientFeatures`, and `clientEvents`. Those calls run in the injected `DobotBridge` on the Flash thread. The injector is `build/xenomit_flash_inject`, built from the sibling `xenomit-flash-client` bridge, and the host splices it into the movie before Pepper sees the original URL. Playback still needs the plugin file and a display; a headless build only compiles the hook, the injector, and the Electron shell.

## Container hosting

The sibling `xenomit-botd/Dockerfile` builds a Debian slim runtime with Xvfb,
tini, the native hook and injected bridge. Mount the pinned Pepper plugin and
account credentials separately; see the botd README for the build/run commands.
The host document is intercepted and served locally under the authenticated
page origin because Pepper disables ExternalInterface for `file://` documents.
The `pepper-native` bridge mode uses native frame capture and avoids redundant
synchronous outgoing JavaScript telemetry while preserving reflected commands
and the ENTER_FRAME FIFO. A cached `navigationAS` observation carries map NOA
circles, rectangles, polygons and inverted zones every 300 ms. The host validates
and atomically writes those observations beside the state file; native capture
merges only matching-map navigation captured within 750 ms. Missing, truncated,
stale or invalid geometry keeps movement gated; a verified empty obstacle list
is ready. Native positions and health never use slow AS entity serialization.

Run the host-origin regression with `node browser/test/host-origin.test.js`.

Native reads copy each AVM page once per capture through the fault-safe Linux
reader, and discard the page cache before the next capture. Capture cost is
measured with a microsecond monotonic clock (the older code rounded to whole
milliseconds). `XENOMIT_FLASH_SCALAR_READS=1` disables batching for diagnostics.
`METRICS` on the command socket returns actual Flash ENTER_FRAME FPS and cached
navigation cost; configured 60 FPS is a target, not a measured result.

Run `node browser/test/navigation.test.js` for shape/freshness validation and
`ctest --test-dir build/cmake --output-on-failure` for native read/cache tests.
