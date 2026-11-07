# CouchSync — roadmap

Each milestone ends with something usable. Don't start a milestone until the previous one meets its "done when".

## M0 — Project skeleton
- CMake + vcpkg project, Qt 6 QML app window, libmpv rendering a local file.
- Node/TS server skeleton with `ws`, Dockerfile.
- CI (GitHub Actions): build client on Windows, run server tests.

**Done when:** the app opens and plays an MKV with hwdec; CI is green.

## M1 — A good player on its own
- Open file / drag-and-drop, play/pause/seek bar, volume, fullscreen.
- Audio and subtitle track selection, external subtitles, subtitle delay.
- mpv keybindings, remember position per file.

**Done when:** usable as an everyday player.

## M2 — Rooms + sync (same file)
- Room server: create/join, room state, sequencing, clock ping/pong.
- Client: RoomClient, clock sync, SyncEngine (speed correction, seek fallback, scheduled play, wait-for-everyone).
- Invite link / code, members panel, chat.
- Headless sync simulator in CI.

**Done when:** two PCs on different networks stay within ~50 ms for a full movie with no visible stutter; pause/seek propagate within ~300 ms. **This milestone alone beats Syncplay.**

## M3 — Different files (alignment)
- Background fingerprinting + cache, all audio tracks.
- Constant offset → speed ratio → piecewise alignment.
- Alignment card, confidence, manual nudge, "scene not in your version" handling.
- Aligner test set.

**Done when:** a 23.976 fps WEB-DL and a 25 fps PAL copy of the same movie sync automatically.

## M4 — Stream from host (direct)
- WebRTC data channel via libdatachannel, signalling through the server, STUN/TURN.
- Host Range server + viewer local proxy, mpv plays the proxied URL.

**Done when:** a friend without the file watches a 1080p WEB-DL from the host with seeking working.

## M5 — Stream from host (transcoded)
- FFmpeg GPU transcoding (AMF/NVENC/QSV), presets, bandwidth test.

**Done when:** a 4K remux plays for a friend on a 10 Mbps connection.

## M6 — Release polish
- Windows installer, auto-update, settings, reactions, (voice chat?), self-hosting guide, public server.
