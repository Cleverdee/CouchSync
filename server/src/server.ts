import { createServer as createHttpServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { WebSocket, WebSocketServer } from "ws";
import {
  MAX_FRAME_BYTES,
  MAX_MESSAGES_PER_SEC,
  PROTOCOL_VERSION,
  parseClientMessage,
  type ErrorCode,
  type RoomMessage,
  type ServerMessage,
} from "./protocol.js";
import type { Room } from "./room.js";
import { Rooms } from "./rooms.js";

export interface ServerOptions {
  port: number;
  host: string;
  /** Server clock in ms; defaults to performance.timeOrigin + performance.now(). */
  now?: () => number;
  /** Close connections that send no hello within this time (default 10 s). */
  helloTimeoutMs?: number;
  /** Max messages per second per connection (default 20). */
  maxMessagesPerSec?: number;
}

export interface ServerHandle {
  http: Server;
  wss: WebSocketServer;
  rooms: Rooms;
  port: number;
  close(): Promise<void>;
}

const defaultNow = (): number => performance.timeOrigin + performance.now();

/** Sliding-window limiter: at most `limit` events in any `windowMs`. */
class RateLimiter {
  private stamps: number[] = [];
  constructor(
    private readonly limit: number,
    private readonly windowMs: number,
    private readonly now: () => number,
  ) {}
  allow(): boolean {
    const t = this.now();
    while (this.stamps.length > 0 && this.stamps[0]! <= t - this.windowMs) this.stamps.shift();
    if (this.stamps.length >= this.limit) return false;
    this.stamps.push(t);
    return true;
  }
}

export function createServer(opts: ServerOptions): Promise<ServerHandle> {
  const { port, host } = opts;
  const now = opts.now ?? defaultNow;
  const helloTimeoutMs = opts.helloTimeoutMs ?? 10_000;
  const maxPerSec = opts.maxMessagesPerSec ?? MAX_MESSAGES_PER_SEC;

  const rooms = new Rooms({
    now,
    setTimeout: (fn, ms) => setTimeout(fn, ms),
    clearTimeout: (h) => clearTimeout(h as NodeJS.Timeout),
  });

  const http = createHttpServer((req, res) => {
    if (req.method === "GET" && req.url === "/healthz") {
      res.writeHead(200, { "Content-Type": "text/plain" });
      res.end("ok");
      return;
    }
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("not found");
  });

  const wss = new WebSocketServer({ server: http, path: "/ws", maxPayload: MAX_FRAME_BYTES });

  wss.on("connection", (ws) => {
    let binding: { room: Room; id: string } | null = null;
    let closing = false;
    const limiter = new RateLimiter(maxPerSec, 1000, now);

    const send = (msg: ServerMessage): void => {
      if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
    };
    const fail = (code: ErrorCode, message: string, closeCode: number): void => {
      if (closing) return;
      closing = true;
      send({ type: "error", code, message });
      ws.close(closeCode, code);
    };

    const helloTimer = setTimeout(() => {
      if (!closing) {
        closing = true;
        ws.close(1008, "hello timeout");
      }
    }, helloTimeoutMs);

    ws.on("message", (data, isBinary) => {
      if (closing) return;
      const t1 = now();
      if (!limiter.allow()) return fail("rate_limited", "too many messages", 1008);
      if (isBinary) return fail("bad_message", "binary frames not supported", 1003);

      const msg = parseClientMessage(data.toString());
      if (!msg) return fail("bad_message", "malformed or unknown message", 1003);

      if (!binding) {
        if (msg.type !== "hello") return fail("bad_message", "expected hello", 1003);
        if (msg.protocol !== PROTOCOL_VERSION) {
          return fail("protocol_mismatch", `server speaks protocol ${PROTOCOL_VERSION}`, 1008);
        }
        let room: Room | undefined;
        if (msg.room === null) {
          room = rooms.create(msg.password);
        } else {
          room = rooms.get(msg.room);
          if (!room) return fail("room_not_found", "no such room", 1008);
        }
        const res = room.join(msg.name, msg.password, { send });
        if (!res.ok) {
          if (room.size === 0) rooms.delete(room.code);
          return fail(res.code, res.message, 1008);
        }
        clearTimeout(helloTimer);
        binding = { room, id: res.memberId };
        return;
      }

      switch (msg.type) {
        case "hello":
          return fail("bad_message", "already said hello", 1003);
        case "ping":
          send({ type: "pong", t0: msg.t0, t1, t2: now() });
          return;
        default:
          binding.room.receive(binding.id, msg satisfies RoomMessage);
      }
    });

    ws.on("close", () => {
      clearTimeout(helloTimer);
      if (binding) binding.room.leave(binding.id);
      binding = null;
    });
    ws.on("error", () => ws.terminate());
  });

  return new Promise((resolve, reject) => {
    http.once("error", reject);
    http.listen(port, host, () => {
      http.off("error", reject);
      const bound = (http.address() as AddressInfo).port;
      resolve({
        http,
        wss,
        rooms,
        port: bound,
        close: () =>
          new Promise<void>((res, rej) => {
            for (const c of wss.clients) c.terminate();
            rooms.disposeAll();
            wss.close(() => {
              http.close((err) => (err ? rej(err) : res()));
              http.closeAllConnections();
            });
          }),
      });
    });
  });
}
