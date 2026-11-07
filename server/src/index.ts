import { createServer } from "./server.js";

const port = Number(process.env.PORT ?? 8787);
const host = process.env.HOST ?? "0.0.0.0";

const server = await createServer({ port, host });
console.log(`couchsync-server listening on ${host}:${server.port}`);

let stopping = false;
const shutdown = (signal: string): void => {
  if (stopping) return;
  stopping = true;
  console.log(`${signal} received, shutting down`);
  server.close().then(
    () => process.exit(0),
    (err) => {
      console.error(err);
      process.exit(1);
    },
  );
};
process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
