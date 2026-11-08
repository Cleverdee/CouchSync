# CouchSync client (M0: libmpv in Qt Quick)

## Windows build

1. Install Visual Studio 2022 Build Tools with the "Desktop development with C++" workload, plus CMake (>= 3.21) and 7-Zip.
2. Install Qt 6.8 (online installer) with the **MSVC 2022 64-bit** kit.
3. Open an "x64 Native Tools Command Prompt for VS 2022" (or a VS Developer PowerShell with x64) and fetch libmpv:
   ```
   powershell -ExecutionPolicy Bypass -File client\tools\fetch-mpv.ps1
   ```
   This downloads a pinned shinchiro build into `client/third_party/mpv` (gitignored) and generates `mpv.lib`.
4. Configure and build (from `client/`):
   ```
   cmake --preset windows-msvc -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
   cmake --build --preset windows-msvc-release
   ```
   The post-build step copies `libmpv-2.dll` and runs `windeployqt` (disable with `-DCOUCHSYNC_DEPLOY=OFF`).
5. Run:
   ```
   build\windows-msvc\Release\couchsync.exe path\to\file.mkv
   ```

Space toggles pause. The console shows mpv logs and a `hwdec-current:` line so you can verify hardware decoding.

Linux/macOS: install libmpv development files (pkg-config `mpv`) and Qt 6.5+, then configure with plain CMake.

## Sync simulator (`couchsync_sim`)

Headless console app (Qt Core/Network only, no GUI, no libmpv) that runs fake clients against a real room server in real time and asserts sync quality against ground truth. Built by default (`-DCOUCHSYNC_BUILD_SIM=OFF` to skip); it also builds on its own: `cmake --build --preset windows-msvc-release --target couchsync_sim`.

```
cd server && npm run build && PORT=8787 node dist/index.js      # separate terminal
client\build\windows-msvc\Release\couchsync_sim.exe [--server ws://127.0.0.1:8787/ws] [--scenario file.json] [--seed N] [--csv out.csv] [--verbose]
```

- Each fake client connects through its own TCP delay proxy (one-way latency, jitter, and loss modelled as retransmit delay spikes that hold back everything behind them), has a skewed local clock and a fake player with media-clock skew, and runs the same 100 ms controller loop as `SyncController` on top of the real `SyncEngine`/`ClockSync`.
- An unimpaired observer connection provides the ground truth (the server's room state and a precise server clock); drift is measured as model position minus the server's expected position at true server time, not the engine's own estimate.
- The built-in scenario takes about 85 s. `scenarios/ideal.json` (no jitter, skew or loss) is a sanity check for the simulator itself. Exit code 0 = all assertions passed, 1 = assertion failed, 2 = setup error.
