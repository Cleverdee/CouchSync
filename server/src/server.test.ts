import { test } from "node:test";
import assert from "node:assert/strict";
import { WebSocket } from "ws";
import { createServer, type ServerOptions } from "./server.js";

const start = (o: Partial<ServerOptions> = {}) => createServer({ port: 0, host: "127.0.0.1", ...o });

type Msg = { type: string; [k: string]: any };

class Conn {
  ws: WebSocket;
  private buf: Msg[] = [];
  private waiters: { pred: (m: Msg) => boolean; resolve: (m: Msg) => void }[] = [];
  closeCode: Promise<number>;

  constructor(port: number, path = "/ws") {
    this.ws = new WebSocket(`ws://127.0.0.1:${port}${path}`);
    this.ws.on("message", (d) => {
      const m = JSON.parse(d.toString()) as Msg;
      const i = this.waiters.findIndex((w) => w.pred(m));
      if (i >= 0) this.waiters.splice(i, 1)[0]!.resolve(m);
      else this.buf.push(m);
    });
    this.closeCode = new Promise((r) => this.ws.once("close", (c) => r(c)));
  }
  open(): Promise<void> {
    return new Promise((res, rej) => {
      this.ws.once("open", () => res());
      this.ws.once("error", rej);
    });
  }
  send(m: unknown): void {
    this.ws.send(typeof m === "string" ? m : JSON.stringify(m));
  }
  next(type: string, pred: (m: Msg) => boolean = () => true): Promise<Msg> {
    const p = (m: Msg) => m.type === type && pred(m);
    const i = this.buf.findIndex(p);
    if (i >= 0) return Promise.resolve(this.buf.splice(i, 1)[0]!);
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error(`timeout waiting for ${type}`)), 3000);
      this.waiters.push({
        pred: p,
        resolve: (m) => {
          clearTimeout(t);
          resolve(m);
        },
      });
    });
  }
}

async function join(port: number, name: string, room: string | null, extra: object = {}) {
  const c = new Conn(port);
  await c.open();
  c.send({ type: "hello", protocol: 1, client: "test", name, room, ...extra });
  return c;
}

test("GET /healthz returns ok", async () => {
  const s = await start();
  try {
    const res = await fetch(`http://127.0.0.1:${s.port}/healthz`);
    assert.equal(res.status, 200);
    assert.equal(await res.text(), "ok");
  } finally {
    await s.close();
  }
});

test("close() shuts down cleanly", async () => {
  const s = await start();
  const c = new Conn(s.port);
  await c.open();
  await s.close();
  await c.closeCode;
  assert.equal(s.http.listening, false);
  await assert.rejects(fetch(`http://127.0.0.1:${s.port}/healthz`));
});

test("create room, join case-insensitively, intent broadcasts state to both", async () => {
  const s = await start();
  try {
    const a = await join(s.port, "Alice", null);
    const w1 = await a.next("welcome");
    assert.equal(w1.memberId, "m1");
    assert.match(w1.room, /^[23456789ABCDEFGHJKLMNPQRSTUVWXYZ]{8}$/);
    assert.equal(w1.state.media, null);
    assert.equal(w1.members.length, 1);
    assert.equal(w1.members[0].host, true);

    const b = await join(s.port, "Bob", w1.room.toLowerCase());
    const w2 = await b.next("welcome");
    assert.equal(w2.room, w1.room);
    assert.equal(w2.memberId, "m2");
    assert.equal(w2.members.length, 2);

    a.send({ type: "media.set", title: "Movie.mkv", duration: 100 });
    await a.next("state", (m) => m.media !== null);
    await b.next("state", (m) => m.media !== null);

    b.send({ type: "intent.seek", position: 42 });
    const sa = await a.next("state", (m) => m.cause.type === "seek");
    const sb = await b.next("state", (m) => m.cause.type === "seek");
    assert.equal(sa.position, 42);
    assert.equal(sa.seq, sb.seq);
    assert.deepEqual(sa.cause, { type: "seek", by: "m2" });
    a.ws.close();
    b.ws.close();
  } finally {
    await s.close();
  }
});

test("ping returns pong with echoed t0 and ordered server times", async () => {
  const s = await start();
  try {
    const a = await join(s.port, "Alice", null);
    const w = await a.next("welcome");
    a.send({ type: "ping", t0: 12345.5 });
    const p = await a.next("pong");
    assert.equal(p.t0, 12345.5);
    assert.equal(typeof p.t1, "number");
    assert.equal(typeof p.t2, "number");
    assert.ok(p.t2 >= p.t1);
    assert.ok(Math.abs(p.t1 - w.serverTime) < 5000);
    a.ws.close();
  } finally {
    await s.close();
  }
});

test("bad_message closes with 1003: malformed JSON, unknown type, wrong field types, pre-hello", async () => {
  const s = await start();
  try {
    const cases: [string, (c: Conn) => void][] = [
      ["malformed json", (c) => c.send("{nope")],
      ["unknown type", (c) => c.send({ type: "teleport" })],
      ["wrong field type", (c) => c.send({ type: "intent.seek", position: "10" })],
      ["second hello", (c) => c.send({ type: "hello", protocol: 1, name: "x", room: null })],
    ];
    for (const [label, act] of cases) {
      const c = await join(s.port, "Alice", null);
      await c.next("welcome");
      act(c);
      const e = await c.next("error");
      assert.equal(e.code, "bad_message", label);
      assert.equal(await c.closeCode, 1003, label);
    }
    // Non-hello first message.
    const c = new Conn(s.port);
    await c.open();
    c.send({ type: "ping", t0: 1 });
    assert.equal((await c.next("error")).code, "bad_message");
    assert.equal(await c.closeCode, 1003);
  } finally {
    await s.close();
  }
});

test("reserved signal and media.fingerprint are rejected with bad_message", async () => {
  const s = await start();
  try {
    for (const m of [
      { type: "signal", to: "m2", data: {} },
      { type: "media.fingerprint", data: "x" },
    ]) {
      const c = await join(s.port, "Alice", null);
      await c.next("welcome");
      c.send(m);
      assert.equal((await c.next("error")).code, "bad_message");
      assert.equal(await c.closeCode, 1003);
    }
  } finally {
    await s.close();
  }
});

test("rate limit: more than 20 messages/s -> rate_limited + close", async () => {
  const s = await start();
  try {
    const a = await join(s.port, "Alice", null);
    await a.next("welcome");
    for (let i = 0; i < 40; i++) a.send({ type: "ping", t0: i });
    const e = await a.next("error");
    assert.equal(e.code, "rate_limited");
    await a.closeCode;
  } finally {
    await s.close();
  }
});

test("hello errors: protocol_mismatch, room_not_found, bad_password", async () => {
  const s = await start();
  try {
    const m = await join(s.port, "X", null, { protocol: 2 });
    assert.equal((await m.next("error")).code, "protocol_mismatch");
    await m.closeCode;

    const nf = await join(s.port, "X", "ZZZZZZZZ");
    assert.equal((await nf.next("error")).code, "room_not_found");
    await nf.closeCode;

    const host = await join(s.port, "Host", null, { password: "pw" });
    const w = await host.next("welcome");
    const bad = await join(s.port, "X", w.room, { password: "wrong" });
    assert.equal((await bad.next("error")).code, "bad_password");
    await bad.closeCode;
    const good = await join(s.port, "X", w.room, { password: "pw" });
    assert.equal((await good.next("welcome")).members.length, 2);
    host.ws.close();
    good.ws.close();
  } finally {
    await s.close();
  }
});

test("no hello within the timeout closes the connection", async () => {
  const s = await start({ helloTimeoutMs: 100 });
  try {
    const c = new Conn(s.port);
    await c.open();
    await c.closeCode;
  } finally {
    await s.close();
  }
});

test("only /ws accepts WebSocket upgrades", async () => {
  const s = await start();
  try {
    const c = new Conn(s.port, "/");
    await assert.rejects(c.open());
  } finally {
    await s.close();
  }
});

test("room is removed from the registry when the last client disconnects", async () => {
  const s = await start();
  try {
    const a = await join(s.port, "Alice", null);
    await a.next("welcome");
    assert.equal(s.rooms.size, 1);
    a.ws.close();
    await a.closeCode;
    await new Promise((r) => setTimeout(r, 50));
    assert.equal(s.rooms.size, 0);
  } finally {
    await s.close();
  }
});
