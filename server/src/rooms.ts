import { randomInt } from "node:crypto";
import { Room, type RoomDeps } from "./room.js";

export const ROOM_CODE_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
export const ROOM_CODE_LEN = 8;

export function generateCode(): string {
  let s = "";
  for (let i = 0; i < ROOM_CODE_LEN; i++) s += ROOM_CODE_ALPHABET[randomInt(ROOM_CODE_ALPHABET.length)];
  return s;
}

export class Rooms {
  private rooms = new Map<string, Room>();

  constructor(
    private readonly deps: RoomDeps,
    private readonly makeCode: () => string = generateCode,
  ) {}

  get size(): number {
    return this.rooms.size;
  }

  create(password?: string): Room {
    let code = this.makeCode();
    while (this.rooms.has(code)) code = this.makeCode();
    const room = new Room(code, password, this.deps, () => {
      if (this.rooms.get(code) === room) this.rooms.delete(code);
    });
    this.rooms.set(code, room);
    return room;
  }

  /** Case-insensitive lookup. */
  get(code: string): Room | undefined {
    return this.rooms.get(code.trim().toUpperCase());
  }

  delete(code: string): void {
    this.rooms.delete(code);
  }

  disposeAll(): void {
    for (const r of this.rooms.values()) r.dispose();
    this.rooms.clear();
  }
}
