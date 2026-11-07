# CouchSync

**Watch movies with friends, properly in sync.** CouchSync is a desktop media player (built on mpv, so it plays just about anything) with watch-together built in. Invite friends with a link. They can either stream what you're playing, or open their own copy of the same movie — even a different release, encode or frame rate — and CouchSync lines it up automatically.

> **Status: design phase.** Nothing is built yet. See [`docs/DESIGN.md`](docs/DESIGN.md) for how it will work and [`docs/ROADMAP.md`](docs/ROADMAP.md) for the build order.

## Why another one?

Syncplay works, but it feels janky:

| Syncplay | CouchSync |
|---|---|
| Remote-controls an external player and polls it | The player is built in, so it knows the exact playback position |
| Fixes drift by seeking (stutters, rewinds) | Fixes small drift by speeding up or slowing down by a few percent, so nobody notices; seeks only for big jumps |
| No clock sync between people | NTP-style clock sync, so "position 12:04" means the same moment for everyone |
| Different files: just a name/size warning | Different files are aligned by audio fingerprint (different encodes, PAL speed-up, even different cuts) |
| Everyone needs their own file | Friends without the file can stream it from the host |

## Planned features

- Plays everything mpv plays, with hardware decoding
- Rooms with invite links / codes, no account needed
- Smooth sync: clock sync + speed-based drift correction + "wait for everyone" on buffering
- **Same file or a similar file**: automatic alignment by audio fingerprint, with a manual nudge as fallback
- **Stream from host**: send your original file, or a lower-bitrate version transcoded on the fly with your GPU
- Text chat, reactions, optional voice chat
- Per-person subtitles and audio tracks
- Self-hostable server

## Platform

Windows first. Linux and macOS later (the stack is cross-platform).

## License

GPL-3.0 (see [`LICENSE`](LICENSE)). CouchSync builds on mpv, FFmpeg, Qt and Chromaprint.

## Disclaimer

CouchSync is a player and sync tool. It doesn't provide, index or host any content. You're responsible for what you play and share.
