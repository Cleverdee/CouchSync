# couchsync-server

Room server for CouchSync (M0 skeleton: `/healthz` + WebSocket welcome message).

```
npm ci            # or npm install
npm run build     # tsc -> dist/
npm start         # PORT (default 8787), HOST (default 0.0.0.0)
npm test          # build + node:test
docker build -t couchsync-server .
docker run --rm -p 8787:8787 couchsync-server
curl localhost:8787/healthz   # -> ok
```
