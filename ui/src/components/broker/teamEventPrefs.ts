import type { BrokerEvent } from "../../api";
import type { BrokerEventRow, TeamCursorEntry } from "./types";

export type TeamCursorByScope = Record<string, TeamCursorEntry>;

export function extractTeamEventsCursorByScope(prefs: unknown): TeamCursorByScope {
  if (!prefs || typeof prefs !== "object") return {};
  const raw = (prefs as { team_events_cursor?: unknown }).team_events_cursor;
  if (!raw || typeof raw !== "object") return {};
  const byScope = (raw as { by_scope?: unknown }).by_scope;
  if (!byScope || typeof byScope !== "object") return {};
  const out: TeamCursorByScope = {};
  for (const [key, value] of Object.entries(byScope)) {
    if (!value || typeof value !== "object") continue;
    const cursor = (value as TeamCursorEntry).cursor_ts;
    if (typeof cursor !== "number" || !Number.isFinite(cursor) || cursor <= 0) continue;
    out[key] = {
      cursor_ts: cursor,
      updated_unix_ms:
        typeof (value as TeamCursorEntry).updated_unix_ms === "number"
          ? (value as TeamCursorEntry).updated_unix_ms
          : undefined,
    };
  }
  return out;
}

export function normalizeBrokerReplayEvents(events: BrokerEvent[] | undefined): BrokerEventRow[] {
  if (!Array.isArray(events)) return [];
  return events.map((event) => ({
    type: String(event?.type || ""),
    ts_unix_ms: typeof event?.ts_unix_ms === "number" ? event.ts_unix_ms : undefined,
    event_id: event?.event_id ? String(event.event_id) : undefined,
    trace_id: event?.trace_id ? String(event.trace_id) : undefined,
    payload: event?.payload && typeof event.payload === "object" ? event.payload : undefined,
  }));
}
