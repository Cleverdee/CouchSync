# couchsync-server

Room server for CouchSync (M2). Implements `docs/PROTOCOL.md` v1: rooms, shared play state, clock sync (`ping`/`pong`), wait-for-everyone autopause, chat. Only runtime dependency is `ws`.

```
npm ci            # or npm install
npm run build     # tsc -> dist/
npm start         # PORT (default 8787), HOST (default 0.0.0.0)
npm test          # build + node:test
docker build -t couchsync-server .
docker run --rm -p 8787:8787 couchsync-server
curl localhost:8787/healthz   # -> ok
```

WebSocket endpoint: `ws://<host>:<port>/ws`. The client must send `hello` within 10 s.

Layout:
- `src/protocol.ts` message types and hand-written validation
- `src/room.ts` pure room logic (injectable clock/timers, no sockets)
- `src/rooms.ts` room registry and code generation
- `src/server.ts` HTTP + ws wiring, frame-size and rate limits
