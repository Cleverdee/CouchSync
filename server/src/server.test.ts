import { test } from "node:test";
import assert from "node:assert/strict";
import { WebSocket } from "ws";
import { createServer } from "./server.js";

test("GET /healthz returns ok", async () => {
  const s = await createServer({ port: 0, host: "127.0.0.1" });
  try {
    const res = await fetch(`http://127.0.0.1:${s.port}/healthz`);
    assert.equal(res.status, 200);
    assert.equal(await res.text(), "ok");
  } finally {
    await s.close();
  }
});

test("ws client receives welcome", async () => {
  const s = await createServer({ port: 0, host: "127.0.0.1" });
  const ws = new WebSocket(`ws://127.0.0.1:${s.port}`);
  try {
    const msg = await new Promise<string>((resolve, reject) => {
      ws.once("message", (d) => resolve(d.toString()));
      ws.once("error", reject);
    });
    const m = JSON.parse(msg);
    assert.equal(m.type, "welcome");
    assert.equal(typeof m.serverTime, "number");
    assert.equal(typeof m.version, "string");
  } finally {
    ws.terminate();
    await s.close();
  }
});

test("close() shuts down cleanly", async () => {
  const s = await createServer({ port: 0, host: "127.0.0.1" });
  const ws = new WebSocket(`ws://127.0.0.1:${s.port}`);
  await new Promise((r) => ws.once("open", r));
  const closed = new Promise((r) => ws.once("close", r));
  await s.close();
  await closed;
  assert.equal(s.http.listening, false);
  await assert.rejects(fetch(`http://127.0.0.1:${s.port}/healthz`));
});
