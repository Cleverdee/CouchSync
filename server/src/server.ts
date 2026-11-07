import { createServer as createHttpServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { readFileSync } from "node:fs";
import { WebSocketServer } from "ws";

const pkg = JSON.parse(
  readFileSync(new URL("../package.json", import.meta.url), "utf8"),
) as { version: string };

export interface ServerOptions {
  port: number;
  host: string;
}

export interface ServerHandle {
  http: Server;
  wss: WebSocketServer;
  port: number;
  close(): Promise<void>;
}

export function createServer({ port, host }: ServerOptions): Promise<ServerHandle> {
  const http = createHttpServer((req, res) => {
    if (req.method === "GET" && req.url === "/healthz") {
      res.writeHead(200, { "Content-Type": "text/plain" });
      res.end("ok");
      return;
    }
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("not found");
  });

  const wss = new WebSocketServer({ server: http });

  wss.on("connection", (ws, req) => {
    console.log(`ws connected from ${req.socket.remoteAddress}`);
    ws.send(JSON.stringify({ type: "welcome", serverTime: Date.now(), version: pkg.version }));
    // TODO(M2): parse protocol messages, rooms, sync. For now, close on any message.
    ws.on("message", () => ws.close(1003, "unsupported"));
    ws.on("close", () => console.log("ws disconnected"));
  });

  return new Promise((resolve, reject) => {
    http.once("error", reject);
    http.listen(port, host, () => {
      http.off("error", reject);
      const bound = (http.address() as AddressInfo).port;
      resolve({
        http,
        wss,
        port: bound,
        close: () =>
          new Promise<void>((res, rej) => {
            for (const c of wss.clients) c.terminate();
            wss.close(() => {
              http.close((err) => (err ? rej(err) : res()));
              http.closeAllConnections();
            });
          }),
      });
    });
  });
}
