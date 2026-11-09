# Progress

## M0 — Skeleton: DONE (2026-11-08)

### Done
- Server skeleton (`server/`): Node/TS + `ws`, `GET /healthz`, placeholder `welcome` message, `node:test` tests (3 passing), multi-stage Dockerfile.
- Client skeleton (`client/`): CMake + Qt 6.8.3 QML window, `MpvItem` (libmpv OpenGL render API in a `QQuickFramebufferObject`, `hwdec=auto-safe`), plays a file passed on the command line, Space toggles pause.
- `client/tools/fetch-mpv.ps1`: pinned shinchiro libmpv build (20261107), SHA256-checked, generates the MSVC import lib.
- CI workflow (`.github/workflows/ci.yml`): Windows client build, server tests, Docker build.
- Local Windows build works (VS 2026, Qt 6.8.3 via aqtinstall in `E:\Qt`, Ninja via pip). Video plays correctly. Hardware decoding confirmed on RX 6600 XT: `hwdec-current: d3d11va-copy`.

### CI
- Repo: https://github.com/Cleverdee/CouchSync. First CI run green (client-windows + server/Docker).

### Fixes so far
- `loadFile` is held until mpv's render context exists. Loading earlier left mpv with no video output (audio only).
- `MPV_RENDER_PARAM_FLIP_Y` is 0, because `QQuickFramebufferObject` already presents the FBO the right way up.
- `src/player` added to include dirs for Qt's generated QML type registration.

### Known issues / notes
- Rebuilding requires Ninja on PATH (`...\Python312\Lib\site-packages\ninja\data\bin`) and a vcvars64 shell.
- The exe must be closed before rebuilding (LNK1104 otherwise).
- hwdec is copy-back (`d3d11va-copy`) because rendering goes through OpenGL. Revisit zero-copy (Qt D3D11 RHI or GL/DX interop) if 4K playback is heavy.
- CMake author warnings (QTP0004, CMP0156) are harmless.
- Open design questions are listed in docs/DESIGN.md (timeline semantics, protocol gaps, drift hysteresis).

## M1 — Good standalone player: DONE (2026-11-08, manually tested by user)
- Open via Ctrl+O / button / drag-drop / command line; controls bar (play/pause, seek, volume 0–130 + mute, fullscreen, auto-hide after 2 s).
- Audio/subtitle track popups, external subs (dialog + drop), sub delay ±0.1 s.
- Qt keys forwarded to mpv default bindings (`keypress`); `f` and Ctrl+O handled by the app.
- Resume position via mpv watch-later in `<AppData>/CouchSync/CouchSync/watch_later`.
- Dark Basic-style theme; extras: single click = pause (250 ms delay), wheel = volume.
- Partial: dropping video + subs together loads the video only.

## M2 — Rooms + sync (same file): IN PROGRESS
- Done: docs/PROTOCOL.md v1; server rooms (29 tests); Qt-free sync core `client/src/sync` (100 checks incl. closed-loop sim; ctest in CI); client integration: `src/net/WsConn` (custom RFC 6455 client), `RoomClient`, `src/player/SyncController` (local-vs-engine action detection), join dialog, room panel, toasts, countdown, Ctrl+D sync readout.
- CLI for testing: `couchsync [file] --server URL --create NAME | --join CODE NAME [--password PW] [--log-sync]`. Run the server with `cd server && npm start` (port 8787).
- Also done: speed retarget rate-limit (fixed stuck −25 ms drift); skew-aware ClockSync (weighted LSQ fit, <2 ms error); adaptive ping burst and clock-ready gating (`PingSchedule`); `jumpSeekThresholdS` 0.25 s; headless simulator `couchsync_sim` in CI (`--seed 1`). Default scenario passes 30/30 seeds, worst steady p95 33 ms.
- Next:
  1. ~~User manual test: two instances on one PC~~ — passed 2026-11-09 (user: "works well").
  2. Two-PC test over the internet. This needs a reachable server (port-forward, tunnel, or VPS).
  3. Swap WsConn for QtWebSockets (needs the user's OK for the download).
  4. Optional: smoother local position estimate (mpv time-pos has ~10 ms steps and ±15 ms jitter).

## After M2 (agreed)
- Release workflow (tag `v*` → build → GitHub Release with zip + SHA256) and in-app update-available banner. Full auto-update comes in M6.
