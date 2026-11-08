# CouchSync — design doc (draft v0)

Status: **design phase, nothing built yet.** This file is the source of truth for the design.
Owner: Shaheer. Last updated: 2026-11-08.

---

## 1. Goal

A desktop media player with watch-together built in, that feels **smooth** where Syncplay feels janky.

### Success criteria (v1)
- Plays any common video file (MKV/MP4/AVI, HEVC/AV1/H.264, all common audio and subtitle formats) with hardware decoding.
- Host creates a room, sends a link, friends join in one click.
- With everyone on the **same file**, playback stays within ~50 ms of each other, with no visible stutter while correcting.
- Pause / play / seek by anyone is reflected for everyone within ~300 ms.
- With **different files of the same movie** (different encode, release group, 23.976 vs 25 fps), the app finds the offset automatically and keeps everyone in sync.
- A friend **without the file** can watch the host's file streamed to them.

### Out of scope for v1
- Accounts, friends lists, public room browser.
- Mobile apps, browser version.
- Streaming services (Netflix etc.). Local files only.
- Screen sharing of arbitrary windows.

---

## 2. Why Syncplay feels janky (what we're fixing)

1. **External player.** It controls mpv/VLC/MPC over IPC and polls them for position, so its view of playback is laggy and coarse.
2. **Seek-only correction.** When drift passes a threshold it seeks, which causes visible jumps and rewinds.
3. **No clock model.** Positions are compared without proper latency/clock-offset compensation.
4. **Weak file handling.** Files are compared by name/size/duration, not by content. Different files aren't aligned at all.

---

## 3. Architecture

```
┌──────────────────────────── CouchSync client (Qt 6 / C++) ─────────────────────────────┐
│  UI (QML)  ── Player (libmpv render API) ── SyncEngine ── RoomClient (WebSocket)        │
│                     │                           │                │                      │
│                     │                     Aligner (Chromaprint)  │                      │
│                     └──── HostStream / ViewerStream (libdatachannel + local HTTP) ──────┤
└────────────────────────────────────────────────────────────────────────────────────────┘
                                   │  WebSocket (JSON)
                       ┌───────────▼───────────┐          ┌──────────────┐
                       │  Room server (Node/TS) │          │ TURN (coturn)│  only when P2P fails
                       │  rooms, clock, signal  │          └──────────────┘
                       └────────────────────────┘
```

### Components
- **Player**: libmpv embedded through the render API (`mpv_render_context`, OpenGL) in a Qt Quick item. All mpv features (hwdec, subtitles, audio tracks, shaders) come for free. Observed properties: `time-pos`, `pause`, `speed`, `paused-for-cache`, `seeking`, `duration`, `eof-reached`.
- **SyncEngine**: keeps the local player matched to the room timeline (section 5).
- **RoomClient**: WebSocket connection to the room server; clock sync; sends/receives room events.
- **Aligner**: audio fingerprinting and file-to-file time mapping (section 6).
- **HostStream / ViewerStream**: streaming the host's file to viewers (section 7).
- **Room server**: small Node.js + TypeScript service. Holds room state in memory, timestamps and orders events, relays WebRTC signalling. Stateless across restarts (rooms are short-lived). Self-hostable as one Docker container.

### Stack
| Part | Choice | Why |
|---|---|---|
| Client UI | Qt 6 (QML) + C++20 | Clean libmpv embedding, native performance, cross-platform |
| Player | libmpv (Windows: shinchiro builds) | Every codec, hwdec, subtitles, mature |
| Fingerprinting | Chromaprint + FFmpeg (libav*) | Proven audio fingerprinting, fast decode |
| P2P | libdatachannel (WebRTC data channels) | Small C++ lib, NAT traversal via ICE/STUN/TURN |
| WebSocket | Qt WebSockets | Already in Qt |
| Build | CMake + vcpkg | Standard for Qt/C++ on Windows |
| Server | Node.js + TypeScript + `ws` | Small, easy to self-host |
| TURN | coturn | Standard; only used when direct P2P fails |

---

## 4. Rooms and protocol

- **Room**: random 8-char code + invite link `couchsync://join/<server>/<code>` (also works as a pasted code). Optional room password.
- **Roles**: host (created the room) and members. Default: anyone can play/pause/seek; host can lock controls to host-only.
- **Server is authoritative** for the room timeline. Clients send *intents*; the server stamps them with server time, assigns a sequence number and broadcasts the new state.

### Room state
```json
{
  "seq": 42,
  "playing": true,
  "position": 4523.180,      // room-timeline seconds at serverTime
  "serverTime": 1762550000123,
  "rate": 1.0,
  "media": { "title": "...", "duration": 7234.5, "fingerprintId": "..." },
  "waitingFor": []            // member ids still buffering/aligning
}
```
Expected room position at any server time `t`: `position + (playing ? (t - serverTime)/1000 * rate : 0)`.

### Messages (JSON over WebSocket)
- Client → server: `hello`, `ping`, `intent.play`, `intent.pause`, `intent.seek {position}`, `status {state, localPos, drift}`, `chat`, `signal` (WebRTC SDP/ICE), `media.fingerprint`.
- Server → client: `welcome`, `pong`, `state` (full room state), `members`, `chat`, `signal`, `media.fingerprint`.

Full schema goes in `docs/PROTOCOL.md` when M1 starts.

---

## 5. Sync engine

### 5.1 Clock sync
NTP-style: client sends `ping{t0}`, server replies `pong{t0, t1=serverRecv, t2=serverSend}`, client records `t3`.
- `offset = ((t1 - t0) + (t2 - t3)) / 2`, `rtt = (t3 - t0) - (t2 - t1)`
- Burst of 8 pings on join, then one every 5 s. Keep the last ~32 samples and fit offset and clock skew, weighting the **lowest-RTT** samples most (filters out jitter); see the Decisions log.
- Use a monotonic local clock (`std::chrono::steady_clock`), never wall-clock.

### 5.2 Drift correction
Every ~100 ms: `drift = localPos(mapped to room timeline) − expectedRoomPos`.

| |drift| | Action |
|---|---|
| < 40 ms | Nothing |
| 40 ms – 2 s | Adjust mpv `speed` to `1 − clamp(drift × k, ±5%)` until back under 15 ms, then return to 1.0. Pitch correction on (mpv default `audio-pitch-correction=yes`). |
| > 2 s | Seek to expected position (+ predicted seek time), then fine-tune with speed |

All thresholds configurable. `k` tuned so a 1 s drift is recovered in ~20–30 s at ≤5% speed change.
mpv `video-sync=display-resample` is worth testing for smoother small speed changes.

### 5.3 Play / pause / seek
- Local action → send intent → **apply immediately locally** (optimistic) → server's `state` confirms or corrects.
- Remote action → apply. For `play`, the server schedules the start slightly in the future (`serverTime + 150 ms`), so everyone starts together instead of in order of latency.
- Conflicts (two people seek at once): highest server sequence wins.

### 5.4 Buffering and readiness
- A client that stalls (`paused-for-cache`) or is still aligning reports `status: waiting`.
- Room policy (host setting): **wait for everyone** (default: room auto-pauses, resumes when all ready, with a 3-2-1 overlay) or **don't wait** (the stalled client catches up by seeking).
- Joining mid-movie: new member seeks to the expected position, then joins.

---

## 6. Different files: audio-fingerprint alignment

Goal: map any position in *my* file to the room timeline (the host's file) and back.

### 6.1 Fingerprinting
- Decode the chosen audio track with FFmpeg to mono 11025 Hz, feed Chromaprint, keep the **raw** fingerprint (one 32-bit value per ~0.124 s → ~58k values / ~230 KB for 2 h).
- Runs in the background when a file is opened (target: < 60 s for a 2 h movie; decode audio only, skip video).
- Cache by file hash (size + hash of first/last few MB) in the user's app-data folder.
- Fingerprints are sent through the server; no media is sent.

### 6.2 Alignment
1. **Constant offset**: slide one fingerprint over the other, score = matching bits (popcount of XOR) over the overlap; best peak = offset.
2. **Speed ratio**: also test common ratios (25/23.976, 24/23.976, 25/24 and inverses) by resampling the fingerprint index; keep the best.
3. **Piecewise (different cuts)**: split my fingerprint into ~60 s windows, find each window's best match in the host's; keep high-confidence matches; enforce a monotonic mapping; result = list of anchor segments `(myStart, myEnd, roomStart, rate)`.
   - Positions inside a matched segment map linearly.
   - Positions in the host's timeline with **no counterpart** in my file (scene missing from my cut): I hold on a "this scene isn't in your version" card and rejoin at the next segment.
   - Extra scenes in **my** file: skipped automatically.
4. **Confidence**: show "Aligned ✓ (offset +3.2 s, PAL speed)" or "Low confidence — check sync" with a manual nudge control (±0.1 s / ±1 s) that's sent to nobody but me.

### 6.3 Different audio languages
Dual-audio files are common (e.g. Hindi + English tracks). Fingerprints only match on the same-language track, so:
- Each client fingerprints **every** audio track; alignment uses the best-matching pair of tracks, regardless of which track each person is listening to.
- If no track pair matches (completely different dubs), fall back to: duration-based guess + manual nudge. (Later: music/effects-only matching is a research item.)

---

## 7. Streaming from the host (friends without the file)

### Mode A — Direct (original file)
- Host runs a local HTTP server with Range support over the file.
- Viewers connect to the host over a **WebRTC data channel** (libdatachannel; STUN for NAT traversal; TURN relay fallback). Signalling goes through the room server.
- Viewer runs a local HTTP proxy (`http://127.0.0.1:<port>/stream`) that forwards Range requests over the data channel, and mpv just opens that URL. Seeking works because Range works.
- Needs host upload ≥ file bitrate (plus margin) **per viewer**. Fine for 1080p web releases (~3–8 Mbps), not for 4K remuxes.

### Mode B — Transcoded
- Host runs FFmpeg to produce a lower-bitrate stream (H.264 or HEVC via the GPU encoder: AMF on AMD, NVENC on NVIDIA, QSV on Intel), as fMP4/HLS segments served through the same tunnel.
- Presets: 720p 2.5 Mbps / 1080p 5 Mbps / custom. Subtitles sent as separate text tracks, not burned in.
- Seeking restarts the encoder at the new position.
- Bandwidth test on join suggests a mode/preset.

### NAT and relays
- Default: public STUN. TURN only when direct fails; relayed video costs bandwidth on whoever runs the TURN server, so the official server has a TURN rate limit and self-hosters can add their own.

---

## 8. UI (v1)

- **Player window**: video, minimal overlay controls, mpv keybindings (configurable).
- **Room panel** (side, collapsible): members with status (synced ✓ / buffering / aligning / drift), chat, reactions, invite button, room settings (host).
- **Join dialog**: open an invite link or paste a code → choose "I have the file" (open file) or "Stream from host".
- **Alignment card**: progress while fingerprinting, then result + manual nudge.
- Dark theme, clean, minimal.

---

## 9. Testing

- **Sync simulator** (headless): N fake clients with configurable latency, jitter, clock skew and packet loss against the real server; asserts convergence time and max drift. Runs in CI.
- **Aligner test set**: pairs of the same short clip in different encodes, 25 vs 23.976 fps, with cut/added scenes; asserts recovered offsets within 50 ms.
- **Manual matrix**: two PCs on different networks (one behind CGNAT/mobile hotspot) for P2P/TURN.

---

## 10. Open questions

1. ~~Name~~ — working name **CouchSync** (check GitHub/trademark before going public).
2. Official public server: who hosts it, and where (latency to Pakistan + friends abroad)?
3. Voice chat in v1 or later?
4. License: GPL-3.0 proposed (simplest with mpv/FFmpeg GPL builds). Confirm.
5. Should the host's file be the room timeline, or should the room timeline be "the movie" so the host can change files mid-session?
6. ~~Auto-updates (Windows installer + updater) for v1?~~ Yes, see the Decisions log (2026-11-08).

---

## 11. Decisions log
- 2026-11-08: Desktop app, Windows first. Qt 6 + libmpv client; Node/TS room server.
- 2026-11-08: Speed-based drift correction with clock sync; seek only for large drift.
- 2026-11-08: Different files aligned by Chromaprint audio fingerprints, incl. speed ratio and piecewise cuts.
- 2026-11-08: Host streaming over WebRTC data channels; direct mode first, transcode mode second.
- 2026-11-08: Public GitHub repo; build moves to Claude Code.
- 2026-11-08: Qt comes from the Qt installer / aqtinstall (locally) and install-qt-action (CI), not vcpkg — building Qt via vcpkg is too slow. vcpkg stays for other C++ deps (Chromaprint, FFmpeg, libdatachannel).
- 2026-11-08: libmpv on Windows from a pinned shinchiro build (`client/tools/fetch-mpv.ps1`), MSVC import lib generated from its exports.
- 2026-11-08: v1 room timeline = the host's file (open question 5 deferred; revisit when host file-switching is needed).
- 2026-11-08: Drift correction has a buffer: start speed correction when |drift| > 40 ms, stop when |drift| < 15 ms. Speed = 1 − clamp(k·drift, ±5%) with k = 0.5/s. Seek when |drift| > 2 s, targeting expected + predicted seek time (EMA of measured seek durations, initial 300 ms, clamped to 0–2 s). No correction while seeking or buffering. Wait 500 ms after a seek before re-evaluating. Drift is smoothed by a median of the last 5 samples.
- 2026-11-08: Host controls in the protocol: `intent.setPolicy` (host only), `policy` in room state, defined `hello`/`welcome` payloads. Full schema in docs/PROTOCOL.md.
- 2026-11-08: Right after a room state change (remote seek/play/join), seek if |drift| > 0.25 s (`jumpSeekThresholdS`, lowered from 0.5 s on 2026-11-08: borderline 0.5 s offsets took more than 10 s to slew in the simulator) instead of waiting for 2 s. Otherwise a 1 s remote jump would take ~20 s to catch up by speed. Gradual drift still uses speed up to 2 s.
- 2026-11-08: Auto-updates in v1. Right after M2: tag-triggered GitHub Release workflow plus an in-app "new version available" banner (GitHub releases API). M6: full one-click/silent update on top of the installer, with checksum verification.
- 2026-11-08: Clock sync tracks skew. The old estimate (offset of the lowest-RTT sample of the last 16) ignored the clocks' rate difference: with pings every 5 s the chosen sample could be ~80 s old, so at 200 ppm the offset was off by up to 15–18 ms and jumped when the sample was replaced. `ClockSync` now fits offset(t) = a + b·t over the last 32 samples by weighted least squares (weights from RTT excess, a noise floor from the RTT spread and residual scatter, head-of-line-blocked pongs down-weighted, skew shrunk by a 300 ppm prior and clamped to ±1000 ppm, old samples faded out so eviction doesn't step). Same API; `offset()` is the offset at the newest sample.
- 2026-11-08: Simulator steady-state criteria are p95 < 40 ms and max < 50 ms (were 30/80, stricter than the controller: it only corrects above 40 ms by design, and the goal in section 1 is "within ~50 ms"). Convergence (50 ms within 10 s) and propagation (300 ms + one-way latency) are unchanged.
- 2026-11-08: The jump threshold (0.25 s) also applies when a `play` state arrives after its scheduled start (player still paused). That case used the 2 s threshold, so a play delayed 1 s by a loss spike took ~20 s of speed-up to catch up.
