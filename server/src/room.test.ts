import { test } from "node:test";
import assert from "node:assert/strict";
import { Room, type Client } from "./room.js";
import { Rooms } from "./rooms.js";
import type { ErrorMsg, MembersMsg, ServerMessage, StateMsg } from "./protocol.js";

class FakeClock {
  t = 1_000_000;
  private timers: { id: number; at: number; fn: () => void }[] = [];
  private nextId = 1;
  now = (): number => this.t;
  setTimeout = (fn: () => void, ms: number): unknown => {
    const id = this.nextId++;
    this.timers.push({ id, at: this.t + ms, fn });
    return id;
  };
  clearTimeout = (h: unknown): void => {
    this.timers = this.timers.filter((x) => x.id !== h);
  };
  advance(ms: number): void {
    const target = this.t + ms;
    for (;;) {
      const due = this.timers.filter((x) => x.at <= target).sort((a, b) => a.at - b.at || a.id - b.id)[0];
      if (!due) break;
      this.timers = this.timers.filter((x) => x !== due);
      this.t = due.at;
      due.fn();
    }
    this.t = target;
  }
}

class TestClient implements Client {
  msgs: ServerMessage[] = [];
  send(msg: ServerMessage): void {
    this.msgs.push(msg);
  }
  of<T extends ServerMessage["type"]>(type: T): Extract<ServerMessage, { type: T }>[] {
    return this.msgs.filter((m) => m.type === type) as Extract<ServerMessage, { type: T }>[];
  }
  lastState(): StateMsg {
    const s = this.of("state");
    return s[s.length - 1] ?? (this.of("welcome")[0]!.state as StateMsg);
  }
  lastMembers(): MembersMsg {
    const s = this.of("members");
    return s[s.length - 1]!;
  }
  errors(): ErrorMsg[] {
    return this.of("error");
  }
}

function setup(password?: string) {
  const clock = new FakeClock();
  let emptied = 0;
  const room = new Room("ABCDEFGH", password, clock, () => emptied++);
  const add = (name: string, pw?: string) => {
    const c = new TestClient();
    const r = room.join(name, pw, c);
    assert.ok(r.ok);
    return { c, id: r.memberId };
  };
  return { clock, room, add, emptied: () => emptied };
}

function withMedia(duration = 100) {
  const s = setup();
  const host = s.add("Host");
  const guest = s.add("Guest");
  s.room.receive(host.id, { type: "media.set", title: "Movie.mkv", duration });
  return { ...s, host, guest };
}

test("play schedules start 150 ms ahead from expected position; pause and seek effects", () => {
  const { clock, room, host } = withMedia();
  room.receive(host.id, { type: "intent.seek", position: 10 });
  let s = host.c.lastState();
  assert.equal(s.position, 10);
  assert.equal(s.playing, false);
  assert.equal(s.serverTime, clock.t);
  assert.deepEqual(s.cause, { type: "seek", by: host.id });

  room.receive(host.id, { type: "intent.play" });
  s = host.c.lastState();
  assert.equal(s.playing, true);
  assert.equal(s.position, 10);
  assert.equal(s.serverTime, clock.t + 150);
  assert.equal(s.cause.type, "play");

  // 1000 ms after the scheduled start, position is 11; a second play re-bases on that.
  clock.advance(1150);
  room.receive(host.id, { type: "intent.play" });
  s = host.c.lastState();
  assert.ok(Math.abs(s.position - 11) < 1e-9);
  assert.equal(s.serverTime, clock.t + 150);

  // Seek while playing is scheduled too.
  room.receive(host.id, { type: "intent.seek", position: 50 });
  s = host.c.lastState();
  assert.equal(s.position, 50);
  assert.equal(s.serverTime, clock.t + 150);
  assert.equal(s.playing, true);

  clock.advance(10);
  room.receive(host.id, { type: "intent.pause", position: 42 });
  s = host.c.lastState();
  assert.equal(s.playing, false);
  assert.equal(s.position, 42);
  assert.equal(s.serverTime, clock.t);
  assert.equal(s.cause.type, "pause");
});

test("seq increases by exactly 1 per state change", () => {
  const { room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.play" });
  room.receive(guest.id, { type: "intent.seek", position: 5 });
  room.receive(host.id, { type: "intent.pause", position: 6 });
  room.receive(host.id, { type: "intent.setPolicy", controls: "host" });
  const seqs = guest.c.of("state").map((s) => s.seq);
  assert.ok(seqs.length >= 5);
  for (let i = 1; i < seqs.length; i++) assert.equal(seqs[i], seqs[i - 1]! + 1);
  assert.equal(guest.c.of("welcome")[0]!.state.seq + 1, seqs[0]);
});

test("position is clamped to [0, duration] when duration is known", () => {
  const s = setup();
  const host = s.add("Host");
  s.room.receive(host.id, { type: "intent.seek", position: 500 });
  assert.equal(host.c.lastState().position, 500); // duration unknown
  s.room.receive(host.id, { type: "intent.seek", position: -5 });
  assert.equal(host.c.lastState().position, 0);
  s.room.receive(host.id, { type: "media.set", title: "m", duration: 100 });
  s.room.receive(host.id, { type: "intent.seek", position: 500 });
  assert.equal(host.c.lastState().position, 100);
  s.room.receive(host.id, { type: "intent.pause", position: 1e9 });
  assert.equal(host.c.lastState().position, 100);
  s.room.receive(host.id, { type: "intent.pause", position: -1 });
  assert.equal(host.c.lastState().position, 0);
});

test("host-only controls: non-host gets not_allowed, no state change, still a member", () => {
  const { room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.setPolicy", controls: "host" });
  const before = host.c.lastState();
  room.receive(guest.id, { type: "intent.play" });
  room.receive(guest.id, { type: "intent.pause", position: 3 });
  room.receive(guest.id, { type: "intent.seek", position: 3 });
  assert.equal(guest.c.errors().length, 3);
  assert.ok(guest.c.errors().every((e) => e.code === "not_allowed"));
  assert.deepEqual(host.c.lastState(), before);
  assert.equal(room.size, 2);
  // Host still controls.
  room.receive(host.id, { type: "intent.play" });
  assert.equal(host.c.lastState().playing, true);
  // Chat still works for the guest.
  room.receive(guest.id, { type: "chat", text: "hi" });
  assert.equal(host.c.of("chat").length, 1);
});

test("host-only messages: media.set and setPolicy are not_allowed for non-host even with controls=all", () => {
  const { room, host, guest } = withMedia();
  const before = host.c.lastState();
  room.receive(guest.id, { type: "media.set", title: "x", duration: 1 });
  room.receive(guest.id, { type: "intent.setPolicy", waitMode: "nowait" });
  assert.equal(guest.c.errors().length, 2);
  assert.deepEqual(host.c.lastState(), before);
});

test("autopause after >500 ms waiting while playing; autoresume at +3000 ms when all ready", () => {
  const { clock, room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.seek", position: 10 });
  room.receive(host.id, { type: "intent.play" });
  clock.advance(150);
  const startTime = clock.t;
  room.receive(guest.id, { type: "status", state: "waiting", localPos: 0, drift: 0 });
  clock.advance(499);
  assert.equal(host.c.lastState().playing, true);
  clock.advance(1);
  let s = host.c.lastState();
  assert.equal(s.playing, false);
  assert.equal(s.autoPaused, true);
  assert.deepEqual(s.cause, { type: "autopause", by: null });
  assert.deepEqual(s.waitingFor, [guest.id]);
  assert.ok(Math.abs(s.position - 10.5) < 1e-9, `position ${s.position}`);
  assert.equal(s.serverTime, startTime + 500);

  clock.advance(2000);
  room.receive(guest.id, { type: "status", state: "ready", localPos: 10.5, drift: 0 });
  s = host.c.lastState();
  assert.equal(s.playing, true);
  assert.equal(s.autoPaused, false);
  assert.equal(s.serverTime, clock.t + 3000);
  assert.deepEqual(s.cause, { type: "autoresume", by: null });
  assert.deepEqual(s.waitingFor, []);
  assert.ok(Math.abs(s.position - 10.5) < 1e-9);
});

test("a wait shorter than 500 ms does not autopause", () => {
  const { clock, room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.play" });
  room.receive(guest.id, { type: "status", state: "waiting", localPos: 0, drift: 0 });
  clock.advance(400);
  room.receive(guest.id, { type: "status", state: "ready", localPos: 0, drift: 0 });
  clock.advance(5000);
  assert.equal(host.c.lastState().playing, true);
  assert.equal(host.c.lastState().autoPaused, false);
});

test("autoresume waits for every member, including one that leaves", () => {
  const s = setup();
  const a = s.add("A");
  const b = s.add("B");
  const c = s.add("C");
  s.room.receive(a.id, { type: "intent.play" });
  s.room.receive(b.id, { type: "status", state: "waiting", localPos: 0, drift: 0 });
  s.room.receive(c.id, { type: "status", state: "waiting", localPos: 0, drift: 0 });
  s.clock.advance(500);
  assert.equal(a.c.lastState().autoPaused, true);
  s.room.receive(b.id, { type: "status", state: "ready", localPos: 0, drift: 0 });
  assert.equal(a.c.lastState().autoPaused, true);
  s.room.leave(c.id);
  assert.equal(a.c.lastState().autoPaused, false);
  assert.equal(a.c.lastState().playing, true);
});

test("intent.play during autopause clears autoPaused; waiting members do not re-pause", () => {
  const { clock, room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.play" });
  room.receive(guest.id, { type: "status", state: "waiting", localPos: 0, drift: 0 });
  clock.advance(500);
  assert.equal(host.c.lastState().autoPaused, true);

  room.receive(host.id, { type: "intent.play" });
  let s = host.c.lastState();
  assert.equal(s.playing, true);
  assert.equal(s.autoPaused, false);
  assert.equal(s.serverTime, clock.t + 150);
  assert.equal(s.cause.type, "play");
  assert.deepEqual(s.waitingFor, [guest.id]);

  clock.advance(5000);
  assert.equal(host.c.lastState().playing, true);
  // Guest becoming ready later does not trigger an autoresume.
  const seq = host.c.lastState().seq;
  room.receive(guest.id, { type: "status", state: "ready", localPos: 0, drift: 0 });
  s = host.c.lastState();
  assert.notEqual(s.cause.type, "autoresume");
  assert.equal(s.playing, true);
  assert.ok(s.seq >= seq);
});

test("nowait mode never autopauses", () => {
  const { clock, room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.setPolicy", waitMode: "nowait" });
  room.receive(host.id, { type: "intent.play" });
  room.receive(guest.id, { type: "status", state: "waiting", localPos: 0, drift: 0 });
  clock.advance(10_000);
  const s = host.c.lastState();
  assert.equal(s.playing, true);
  assert.equal(s.autoPaused, false);
  assert.deepEqual(s.waitingFor, [guest.id]);
});

test("host promotion: earliest-joined remaining member; room deleted when empty", () => {
  const s = setup();
  const a = s.add("A");
  const b = s.add("B");
  const c = s.add("C");
  s.clock.advance(1000);
  s.room.leave(a.id);
  s.clock.advance(1000);
  const members = c.c.lastMembers().members;
  assert.deepEqual(
    members.map((m) => [m.id, m.host]),
    [
      [b.id, true],
      [c.id, false],
    ],
  );
  // New host can use host-only messages.
  s.room.receive(b.id, { type: "media.set", title: "x", duration: 5 });
  assert.equal(c.c.lastState().media?.title, "x");
  assert.equal(s.emptied(), 0);
  s.room.leave(b.id);
  assert.equal(s.emptied(), 0);
  s.room.leave(c.id);
  assert.equal(s.emptied(), 1);
});

test("registry removes a room when its last member leaves; lookup is case-insensitive", () => {
  const clock = new FakeClock();
  const rooms = new Rooms(clock);
  const room = rooms.create();
  assert.match(room.code, /^[23456789ABCDEFGHJKLMNPQRSTUVWXYZ]{8}$/);
  const c = new TestClient();
  const r = room.join("A", undefined, c);
  assert.ok(r.ok);
  assert.equal(rooms.get(room.code.toLowerCase()), room);
  room.leave(r.memberId);
  assert.equal(rooms.get(room.code), undefined);
  assert.equal(rooms.size, 0);
});

test("media.set resets position and playing, and clears autoPaused", () => {
  const { clock, room, host, guest } = withMedia();
  room.receive(host.id, { type: "intent.seek", position: 30 });
  room.receive(host.id, { type: "intent.play" });
  clock.advance(1000);
  room.receive(host.id, { type: "media.set", title: "Other.mkv", duration: 50 });
  const s = guest.c.lastState();
  assert.equal(s.position, 0);
  assert.equal(s.playing, false);
  assert.equal(s.autoPaused, false);
  assert.deepEqual(s.media, { title: "Other.mkv", duration: 50 });
  assert.deepEqual(s.cause, { type: "media", by: host.id });
});

test("names are deduped with ' (2)', ' (3)'", () => {
  const s = setup();
  s.add("Sam");
  s.add("Sam");
  const third = s.add("sam");
  const names = third.c.of("welcome")[0]!.members.map((m) => m.name);
  assert.deepEqual(names, ["Sam", "Sam (2)", "sam (3)"]);
});

test("password: required and checked when set; ignored when room has none", () => {
  const s = setup("secret");
  s.add("Host", "secret");
  const bad = new TestClient();
  const r1 = s.room.join("X", "nope", bad);
  assert.deepEqual(r1.ok ? null : r1.code, "bad_password");
  const r2 = s.room.join("X", undefined, new TestClient());
  assert.deepEqual(r2.ok ? null : r2.code, "bad_password");
  assert.equal(bad.msgs.length, 0);
  assert.ok(s.room.join("Y", "secret", new TestClient()).ok);
  assert.equal(s.room.size, 2);

  const open = setup();
  open.add("Host");
  assert.ok(open.room.join("Z", "whatever", new TestClient()).ok);
});

test("room_full at 16 members", () => {
  const s = setup();
  for (let i = 0; i < 16; i++) s.add(`P${i}`);
  const r = s.room.join("extra", undefined, new TestClient());
  assert.equal(r.ok ? null : r.code, "room_full");
  assert.equal(s.room.size, 16);
  // A slot frees up.
  s.room.leave("m5");
  assert.ok(s.room.join("extra", undefined, new TestClient()).ok);
});

test("members broadcasts are throttled to at most 2 per second per room", () => {
  const { clock, room, host, guest } = withMedia();
  clock.advance(1000);
  const count = () => host.c.of("members").length;
  const base = count();
  for (let i = 0; i < 10; i++) {
    room.receive(guest.id, { type: "status", state: "ready", localPos: i, drift: i / 100 });
    clock.advance(10);
  }
  assert.equal(count() - base, 1); // leading edge only so far
  clock.advance(500);
  assert.equal(count() - base, 2); // one trailing flush with latest data
  assert.equal(host.c.lastMembers().members[1]!.drift, 0.09);
  clock.advance(5000);
  assert.equal(count() - base, 2);
});

test("chat is broadcast to everyone including the sender", () => {
  const { clock, room, host, guest } = withMedia();
  room.receive(guest.id, { type: "chat", text: "hello" });
  for (const c of [host.c, guest.c]) {
    const m = c.of("chat")[0]!;
    assert.deepEqual(m, { type: "chat", from: guest.id, name: "Guest", text: "hello", serverTime: clock.t });
  }
});
