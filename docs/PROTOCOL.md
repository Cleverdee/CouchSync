# CouchSync room protocol (v1)

Status: M2. Source of truth for client ↔ server messages. Change this file in the same commit as any protocol change.

## Transport
- WebSocket at `ws(s)://<server>/ws`. Text frames, one JSON object per frame, each with a string `type`.
- Max frame 16 KiB. Max 20 messages/s per connection (excess → `error{code:"rate_limited"}` + close).
- Unknown `type`, malformed JSON or wrong field types → `error{code:"bad_message"}` + close (code 1003).
- All times are **server time in milliseconds** (monotonic on the server: `performance.timeOrigin + performance.now()`), unless named `*Local`.
- All media positions are **seconds** (float) on the room timeline. In v1 the room timeline is the host's file.

## Session
1. Client connects and sends `hello`.
2. Server replies `welcome` (or `error` + close).
3. Client starts clock sync (`ping` burst), then exchanges messages freely.

### `hello` (C→S)
```json
{"type":"hello","protocol":1,"client":"couchsync/0.1.0","name":"Shaheer","room":"K7M2Q9XP","password":"optional"}
```
- `room`: an 8-char code to join, or `null` to create a new room (the creator becomes host). `password` sets the room password when creating, or is checked when joining.
- `name`: 1–32 chars after trimming. The server dedupes clashes by adding ` (2)`.

### `welcome` (S→C)
```json
{"type":"welcome","memberId":"m3","room":"K7M2Q9XP","serverTime":123456.7,"state":{...},"members":[...]}
```
`state` and `members` are the same shapes as the `state` and `members` messages.

### `error` (S→C)
`{"type":"error","code":"...","message":"human readable"}`. Codes:

| Code | Meaning | Closes connection? |
|---|---|---|
| `protocol_mismatch` | wrong protocol version | yes |
| `room_not_found` | no room with that code | yes |
| `bad_password` | wrong password | yes |
| `room_full` | room already has 16 members | yes |
| `bad_message` | malformed or unknown message | yes |
| `rate_limited` | over 20 messages/s | yes |
| `not_allowed` | e.g. a non-host sending an intent while controls are host-only | **no**; the message is ignored |

## Clock sync
- `ping` (C→S): `{"type":"ping","t0":<client local ms>}`
- `pong` (S→C): `{"type":"pong","t0":<echoed>,"t1":<server recv>,"t2":<server send>}`
- The client records `t3` on receipt. `offset = ((t1 - t0) + (t2 - t3)) / 2`, `rtt = (t3 - t0) - (t2 - t1)`.
- Client sends 8 pings ~50 ms apart after `welcome`, then one every 5 s. It keeps the last 16 samples and uses the offset of the lowest-RTT sample. `serverNow = localSteadyMs + offset`.
- Client local time must be `std::chrono::steady_clock`.

## Room state
```json
{"type":"state","seq":42,"playing":true,"position":1234.5,"serverTime":123456.7,"rate":1.0,
 "media":{"title":"Movie.mkv","duration":7260.1},
 "policy":{"controls":"all","waitMode":"wait"},
 "waitingFor":["m2"],"autoPaused":false,
 "cause":{"type":"seek","by":"m1"}}
```
- `seq` increases by 1 on every state change. Clients ignore a `state` whose `seq` is ≤ the last one applied.
- **Expected position** at server time `t`:
  `playing && t > serverTime ? position + (t - serverTime) / 1000 * rate : position`
  When `serverTime` is in the future (scheduled start), the position holds until then.
- `media`: `null` until the host sends `media.set`.
- `policy.controls`: `"all"` (anyone can play/pause/seek) or `"host"`. `policy.waitMode`: `"wait"` (pause for stalled members) or `"nowait"`.
- `cause.type`: `play | pause | seek | autopause | autoresume | policy | media | join`. `by` is a member id, or `null` for server-initiated causes. It's for UI toasts only.

## Intents (C→S)
Clients apply their own intent locally right away (optimistic), then reconcile with the resulting `state`.

| Message | Server effect (`now` = server time on receipt) |
|---|---|
| `{"type":"intent.play"}` | `position = expected(now)`, `playing = true`, `serverTime = now + 150` (scheduled start) |
| `{"type":"intent.pause","position":p}` | `playing = false`, `position = p`, `serverTime = now` |
| `{"type":"intent.seek","position":p}` | `position = p`, `serverTime = playing ? now + 150 : now` |

- `p` is clamped to `[0, media.duration]` when the duration is known.
- Intents are processed in arrival order. Each produces a new `seq`, so with simultaneous seeks the last one processed wins.
- An intent from a non-host while `policy.controls == "host"` gets `error{not_allowed}` and no state change.

## Member status
`{"type":"status","state":"ready","localPos":1234.4,"drift":-0.012}` (C→S)
- `state`: `ready` or `waiting` (loading, buffering, seeking; `aligning` arrives in M3). `drift`: seconds, local minus expected.
- Sent on every state change, and every 1 s otherwise.

### Wait-for-everyone (`waitMode: "wait"`)
- **Autopause:** a member switches to `waiting` while `playing` and the wait lasts over 500 ms. The server sets `playing = false`, `position = expected(now)`, `autoPaused = true`, `cause = autopause`.
- **Autoresume:** `autoPaused` is set and every member is `ready`. The server sets `playing = true`, `serverTime = now + 3000`, `autoPaused = false`, `cause = autoresume`. Clients show a 3-2-1 countdown until `serverTime`.
- **User action during autopause:** an `intent.play` clears `autoPaused` and starts as usual. Waiting members then catch up as in `nowait`.
- **`nowait` mode:** the server never autopauses. A member that falls behind seeks to catch up on its own.
- `waitingFor` always lists the members currently `waiting`.

## Members
`{"type":"members","members":[{"id":"m1","name":"Shaheer","host":true,"state":"ready","drift":0.004}]}` (S→C)
- Broadcast on join, leave, host change, and status change. Throttled to at most 2 per second per room.
- When the host leaves, the earliest-joined remaining member becomes host.
- The room is deleted when its last member leaves.

## Host-only messages (C→S)
`not_allowed` if sent by a non-host.
- `{"type":"media.set","title":"Movie.mkv","duration":7260.1}`: sets `state.media`. A new file also resets `position = 0`, `playing = false`.
- `{"type":"intent.setPolicy","controls":"host","waitMode":"nowait"}`: both fields are optional.

## Chat
- C→S: `{"type":"chat","text":"..."}`, where `text` is 1–500 chars.
- S→C: `{"type":"chat","from":"m1","name":"Shaheer","text":"...","serverTime":123456.7}`, broadcast to everyone including the sender.

## Room codes and links
- 8 characters from `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0/O/1/I). Codes are case-insensitive on input.
- Invite link: `couchsync://join/<server-host[:port]>/<code>`.

## Clarifications
- A new room starts at `seq = 1` (`cause = join`, `by` = the creator). Later joins don't bump `seq`; the joiner gets the current state in `welcome`.
- A new member's status is `ready` until it sends `status`.
- Autopause only triggers for a wait that begins while `playing`. Members already waiting when an `intent.play` overrides an autopause are not re-counted until they become `ready` and wait again.
- A change in `waitingFor` while playing or autoPaused produces a new `seq`, which reuses the previous `cause`.
- `media.set` always resets `position = 0` and `playing = false`, and clears `autoPaused`. `title` must be 1–512 chars.
- Switching to `nowait`, a pause, or `media.set` clears `autoPaused`.
- Chat `text` length is 1–500 chars, untrimmed. Name dedupe is case-insensitive.
- Close codes: `bad_message` → 1003. Other fatal errors and the 10 s hello timeout → 1008. Frames over 16 KiB → 1009 with no `error` message.
- Join checks run in this order: `room_not_found`, `bad_password` (only if the room has a password), `room_full`.
- `ping` counts toward the rate limit. A second `hello` gets `bad_message`.

## Reserved (later milestones)
`signal` (M4 WebRTC signalling) and `media.fingerprint` (M3). The server rejects them with `bad_message` until implemented.
