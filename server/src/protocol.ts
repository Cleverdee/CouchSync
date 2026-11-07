// Message types and hand-written validation for docs/PROTOCOL.md (v1).

export const PROTOCOL_VERSION = 1;
export const MAX_FRAME_BYTES = 16 * 1024;
export const MAX_MESSAGES_PER_SEC = 20;
export const MAX_MEMBERS = 16;
export const MAX_NAME_LEN = 32;
export const MAX_CHAT_LEN = 500;
export const MAX_TITLE_LEN = 512;

export type ErrorCode =
  | "protocol_mismatch"
  | "room_not_found"
  | "bad_password"
  | "room_full"
  | "bad_message"
  | "rate_limited"
  | "not_allowed";

export type Controls = "all" | "host";
export type WaitMode = "wait" | "nowait";
export type MemberState = "ready" | "waiting";
export type CauseType =
  | "play"
  | "pause"
  | "seek"
  | "autopause"
  | "autoresume"
  | "policy"
  | "media"
  | "join";

export interface Media {
  title: string;
  duration: number;
}
export interface Policy {
  controls: Controls;
  waitMode: WaitMode;
}
export interface Cause {
  type: CauseType;
  by: string | null;
}

export interface StateMsg {
  type: "state";
  seq: number;
  playing: boolean;
  position: number;
  serverTime: number;
  rate: number;
  media: Media | null;
  policy: Policy;
  waitingFor: string[];
  autoPaused: boolean;
  cause: Cause;
}

export interface MemberInfo {
  id: string;
  name: string;
  host: boolean;
  state: MemberState;
  drift: number;
}
export interface MembersMsg {
  type: "members";
  members: MemberInfo[];
}
export interface WelcomeMsg {
  type: "welcome";
  memberId: string;
  room: string;
  serverTime: number;
  state: StateMsg;
  members: MemberInfo[];
}
export interface ErrorMsg {
  type: "error";
  code: ErrorCode;
  message: string;
}
export interface PongMsg {
  type: "pong";
  t0: number;
  t1: number;
  t2: number;
}
export interface ChatOutMsg {
  type: "chat";
  from: string;
  name: string;
  text: string;
  serverTime: number;
}
export type ServerMessage =
  | StateMsg
  | MembersMsg
  | WelcomeMsg
  | ErrorMsg
  | PongMsg
  | ChatOutMsg;

export interface HelloMsg {
  type: "hello";
  protocol: number;
  client?: string;
  name: string;
  room: string | null;
  password?: string;
}
export interface PingMsg {
  type: "ping";
  t0: number;
}
export interface IntentPlayMsg {
  type: "intent.play";
}
export interface IntentPauseMsg {
  type: "intent.pause";
  position: number;
}
export interface IntentSeekMsg {
  type: "intent.seek";
  position: number;
}
export interface StatusMsg {
  type: "status";
  state: MemberState;
  localPos: number;
  drift: number;
}
export interface MediaSetMsg {
  type: "media.set";
  title: string;
  duration: number;
}
export interface SetPolicyMsg {
  type: "intent.setPolicy";
  controls?: Controls;
  waitMode?: WaitMode;
}
export interface ChatInMsg {
  type: "chat";
  text: string;
}
export type ClientMessage =
  | HelloMsg
  | PingMsg
  | IntentPlayMsg
  | IntentPauseMsg
  | IntentSeekMsg
  | StatusMsg
  | MediaSetMsg
  | SetPolicyMsg
  | ChatInMsg;

/** Messages handled by a Room (everything after hello, except ping). */
export type RoomMessage = Exclude<ClientMessage, HelloMsg | PingMsg>;

type Obj = Record<string, unknown>;
const isObj = (v: unknown): v is Obj => typeof v === "object" && v !== null && !Array.isArray(v);
const isNum = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);

/** Returns the validated message, or null when malformed / unknown type. */
export function parseClientMessage(raw: string): ClientMessage | null {
  let v: unknown;
  try {
    v = JSON.parse(raw);
  } catch {
    return null;
  }
  if (!isObj(v) || typeof v.type !== "string") return null;

  switch (v.type) {
    case "hello": {
      if (!isNum(v.protocol)) return null;
      if (typeof v.name !== "string") return null;
      const name = v.name.trim();
      if (name.length < 1 || name.length > MAX_NAME_LEN) return null;
      if (v.room !== null && typeof v.room !== "string") return null;
      if (v.password !== undefined && typeof v.password !== "string") return null;
      if (v.client !== undefined && typeof v.client !== "string") return null;
      return {
        type: "hello",
        protocol: v.protocol,
        client: v.client,
        name,
        room: v.room,
        password: v.password,
      };
    }
    case "ping":
      return isNum(v.t0) ? { type: "ping", t0: v.t0 } : null;
    case "intent.play":
      return { type: "intent.play" };
    case "intent.pause":
      return isNum(v.position) ? { type: "intent.pause", position: v.position } : null;
    case "intent.seek":
      return isNum(v.position) ? { type: "intent.seek", position: v.position } : null;
    case "status": {
      if (v.state !== "ready" && v.state !== "waiting") return null;
      if (!isNum(v.localPos) || !isNum(v.drift)) return null;
      return { type: "status", state: v.state, localPos: v.localPos, drift: v.drift };
    }
    case "media.set": {
      if (typeof v.title !== "string" || v.title.length < 1 || v.title.length > MAX_TITLE_LEN) {
        return null;
      }
      if (!isNum(v.duration) || v.duration < 0) return null;
      return { type: "media.set", title: v.title, duration: v.duration };
    }
    case "intent.setPolicy": {
      const out: SetPolicyMsg = { type: "intent.setPolicy" };
      if (v.controls !== undefined) {
        if (v.controls !== "all" && v.controls !== "host") return null;
        out.controls = v.controls;
      }
      if (v.waitMode !== undefined) {
        if (v.waitMode !== "wait" && v.waitMode !== "nowait") return null;
        out.waitMode = v.waitMode;
      }
      return out;
    }
    case "chat": {
      if (typeof v.text !== "string" || v.text.length < 1 || v.text.length > MAX_CHAT_LEN) {
        return null;
      }
      return { type: "chat", text: v.text };
    }
    default:
      // Includes reserved `signal` and `media.fingerprint` (M3/M4).
      return null;
  }
}
