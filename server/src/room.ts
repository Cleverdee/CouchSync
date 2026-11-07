// Pure room logic: no sockets, no global clock. All time/timers are injected.
import {
  MAX_MEMBERS,
  type Cause,
  type ErrorCode,
  type MemberInfo,
  type MemberState,
  type Media,
  type Policy,
  type RoomMessage,
  type ServerMessage,
  type StateMsg,
  type WelcomeMsg,
} from "./protocol.js";

export interface RoomDeps {
  now(): number;
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
}

export interface Client {
  send(msg: ServerMessage): void;
}

export const START_DELAY_MS = 150;
export const AUTOPAUSE_AFTER_MS = 500;
export const AUTORESUME_DELAY_MS = 3000;
export const MEMBERS_MIN_INTERVAL_MS = 500;

interface Member {
  id: string;
  name: string;
  client: Client;
  status: MemberState;
  drift: number;
  waitTimer: unknown;
}

export type JoinResult =
  | { ok: true; memberId: string }
  | { ok: false; code: ErrorCode; message: string };

export class Room {
  private members = new Map<string, Member>(); // insertion order = join order
  private nextId = 1;
  private seq = 1;
  private playing = false;
  private position = 0;
  private serverTime: number;
  private media: Media | null = null;
  private policy: Policy = { controls: "all", waitMode: "wait" };
  private autoPaused = false;
  private cause: Cause = { type: "join", by: null };
  private hostId: string | null = null;
  private lastMembersSent = -Infinity;
  private membersTimer: unknown = null;
  private disposed = false;

  constructor(
    readonly code: string,
    private readonly password: string | undefined,
    private readonly deps: RoomDeps,
    private readonly onEmpty: () => void = () => {},
  ) {
    this.serverTime = deps.now();
  }

  get size(): number {
    return this.members.size;
  }

  // ---- membership -------------------------------------------------------

  join(name: string, password: string | undefined, client: Client): JoinResult {
    if (this.password !== undefined && this.password !== "" && password !== this.password) {
      return { ok: false, code: "bad_password", message: "wrong room password" };
    }
    if (this.members.size >= MAX_MEMBERS) {
      return { ok: false, code: "room_full", message: "room is full" };
    }
    const member: Member = {
      id: `m${this.nextId++}`,
      name: this.uniqueName(name),
      client,
      status: "ready",
      drift: 0,
      waitTimer: null,
    };
    this.members.set(member.id, member);
    if (this.hostId === null) {
      this.hostId = member.id;
      this.cause = { type: "join", by: member.id };
    }
    const welcome: WelcomeMsg = {
      type: "welcome",
      memberId: member.id,
      room: this.code,
      serverTime: this.deps.now(),
      state: this.snapshot(),
      members: this.memberList(),
    };
    client.send(welcome);
    this.scheduleMembers();
    return { ok: true, memberId: member.id };
  }

  leave(memberId: string): void {
    const m = this.members.get(memberId);
    if (!m) return;
    this.clearWaitTimer(m);
    this.members.delete(memberId);
    if (this.members.size === 0) {
      this.dispose();
      this.onEmpty();
      return;
    }
    if (this.hostId === memberId) {
      this.hostId = this.members.keys().next().value ?? null;
    }
    this.reevaluateWaiting();
    this.scheduleMembers();
  }

  dispose(): void {
    this.disposed = true;
    for (const m of this.members.values()) this.clearWaitTimer(m);
    if (this.membersTimer !== null) {
      this.deps.clearTimeout(this.membersTimer);
      this.membersTimer = null;
    }
  }

  // ---- message handling -------------------------------------------------

  receive(memberId: string, msg: RoomMessage): void {
    const m = this.members.get(memberId);
    if (!m || this.disposed) return;
    const isHost = memberId === this.hostId;

    switch (msg.type) {
      case "status":
        this.onStatus(m, msg.state, msg.drift);
        return;
      case "chat":
        this.broadcast({
          type: "chat",
          from: m.id,
          name: m.name,
          text: msg.text,
          serverTime: this.deps.now(),
        });
        return;
      case "media.set":
        if (!isHost) return this.deny(m);
        this.media = { title: msg.title, duration: msg.duration };
        this.playing = false;
        this.position = 0;
        this.serverTime = this.deps.now();
        this.autoPaused = false;
        this.commit({ type: "media", by: m.id });
        return;
      case "intent.setPolicy":
        if (!isHost) return this.deny(m);
        if (msg.controls !== undefined) this.policy.controls = msg.controls;
        if (msg.waitMode !== undefined) this.policy.waitMode = msg.waitMode;
        if (this.policy.waitMode === "nowait") this.autoPaused = false;
        this.commit({ type: "policy", by: m.id });
        return;
      case "intent.play": {
        if (!this.mayControl(isHost)) return this.deny(m);
        const now = this.deps.now();
        this.position = this.expected(now);
        this.playing = true;
        this.serverTime = now + START_DELAY_MS;
        this.autoPaused = false;
        this.commit({ type: "play", by: m.id });
        return;
      }
      case "intent.pause": {
        if (!this.mayControl(isHost)) return this.deny(m);
        this.playing = false;
        this.position = this.clamp(msg.position);
        this.serverTime = this.deps.now();
        this.autoPaused = false;
        this.commit({ type: "pause", by: m.id });
        return;
      }
      case "intent.seek": {
        if (!this.mayControl(isHost)) return this.deny(m);
        const now = this.deps.now();
        this.position = this.clamp(msg.position);
        this.serverTime = this.playing ? now + START_DELAY_MS : now;
        this.commit({ type: "seek", by: m.id });
        return;
      }
    }
  }

  // ---- state ------------------------------------------------------------

  snapshot(): StateMsg {
    return {
      type: "state",
      seq: this.seq,
      playing: this.playing,
      position: this.position,
      serverTime: this.serverTime,
      rate: 1,
      media: this.media ? { ...this.media } : null,
      policy: { ...this.policy },
      waitingFor: this.waitingFor(),
      autoPaused: this.autoPaused,
      cause: { ...this.cause },
    };
  }

  memberList(): MemberInfo[] {
    return [...this.members.values()].map((m) => ({
      id: m.id,
      name: m.name,
      host: m.id === this.hostId,
      state: m.status,
      drift: m.drift,
    }));
  }

  private expected(t: number): number {
    return this.playing && t > this.serverTime
      ? this.position + ((t - this.serverTime) / 1000) * 1
      : this.position;
  }

  private clamp(p: number): number {
    const hi = this.media ? this.media.duration : Infinity;
    return Math.min(Math.max(p, 0), hi);
  }

  private mayControl(isHost: boolean): boolean {
    return isHost || this.policy.controls === "all";
  }

  private waitingFor(): string[] {
    return [...this.members.values()].filter((m) => m.status === "waiting").map((m) => m.id);
  }

  private commit(cause: Cause): void {
    this.seq++;
    this.cause = cause;
    this.broadcast(this.snapshot());
  }

  // ---- wait-for-everyone ------------------------------------------------

  private onStatus(m: Member, state: MemberState, drift: number): void {
    const prevState = m.status;
    const prevDrift = m.drift;
    m.status = state;
    m.drift = drift;
    if (prevState !== state) {
      this.clearWaitTimer(m);
      // Only a wait that begins while playing can trigger an autopause.
      if (state === "waiting" && this.playing) {
        m.waitTimer = this.deps.setTimeout(() => this.onWaitTimer(m), AUTOPAUSE_AFTER_MS);
      }
      this.reevaluateWaiting();
    }
    if (prevState !== state || prevDrift !== drift) this.scheduleMembers();
  }

  private onWaitTimer(m: Member): void {
    m.waitTimer = null;
    if (this.disposed || !this.members.has(m.id) || m.status !== "waiting") return;
    if (!this.playing || this.policy.waitMode !== "wait") return;
    const now = this.deps.now();
    this.position = this.expected(now);
    this.playing = false;
    this.serverTime = now;
    this.autoPaused = true;
    this.commit({ type: "autopause", by: null });
  }

  /** After a status/leave change: autoresume if possible, else refresh waitingFor. */
  private reevaluateWaiting(): void {
    const everyoneReady = [...this.members.values()].every((m) => m.status === "ready");
    if (this.autoPaused && this.policy.waitMode === "wait" && everyoneReady) {
      this.playing = true;
      this.serverTime = this.deps.now() + AUTORESUME_DELAY_MS;
      this.autoPaused = false;
      this.commit({ type: "autoresume", by: null });
    } else if (this.playing || this.autoPaused) {
      // waitingFor changed; publish it with the existing cause.
      this.commit(this.cause);
    }
  }

  private clearWaitTimer(m: Member): void {
    if (m.waitTimer !== null) {
      this.deps.clearTimeout(m.waitTimer);
      m.waitTimer = null;
    }
  }

  // ---- output -----------------------------------------------------------

  private deny(m: Member): void {
    m.client.send({
      type: "error",
      code: "not_allowed",
      message: "not allowed in this room's current policy",
    });
  }

  private broadcast(msg: ServerMessage): void {
    for (const m of this.members.values()) m.client.send(msg);
  }

  /** members broadcast: at most one per MEMBERS_MIN_INTERVAL_MS (leading + trailing). */
  private scheduleMembers(): void {
    if (this.membersTimer !== null || this.disposed) return;
    const wait = this.lastMembersSent + MEMBERS_MIN_INTERVAL_MS - this.deps.now();
    if (wait <= 0) {
      this.sendMembers();
    } else {
      this.membersTimer = this.deps.setTimeout(() => {
        this.membersTimer = null;
        if (!this.disposed) this.sendMembers();
      }, wait);
    }
  }

  private sendMembers(): void {
    this.lastMembersSent = this.deps.now();
    this.broadcast({ type: "members", members: this.memberList() });
  }

  private uniqueName(name: string): string {
    const taken = new Set([...this.members.values()].map((m) => m.name.toLowerCase()));
    if (!taken.has(name.toLowerCase())) return name;
    for (let n = 2; ; n++) {
      const candidate = `${name} (${n})`;
      if (!taken.has(candidate.toLowerCase())) return candidate;
    }
  }
}
