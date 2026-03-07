import type { BrokerEvent } from "../../api";
import type { BrokerEventRow } from "./types";

export type BrokerCursorEntry = {
  cursor_ts?: number;
  updated_unix_ms?: number;
};

export type BrokerCursorByScope = Record<string, BrokerCursorEntry>;

export type BrokerFanoutResult = {
  deployment_id: string;
  status: number;
  data: Record<string, unknown> | null;
};

export function asObjectRecord(value: unknown): Record<string, unknown> | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  return value as Record<string, unknown>;
}

export function asFiniteNumber(value: unknown): number | undefined {
  if (typeof value !== "number" || !Number.isFinite(value)) return undefined;
  return value;
}

export function normalizeDeploymentId(raw: unknown): string {
  const id = String(raw || "").trim();
  return id || "default";
}

export function extractBrokerEventsCursorByScope(prefs: unknown): BrokerCursorByScope {
  if (!prefs || typeof prefs !== "object") return {};
  const raw = (prefs as { broker_events_cursor?: unknown }).broker_events_cursor;
  if (!raw || typeof raw !== "object") return {};
  const byScope = (raw as { by_scope?: unknown }).by_scope;
  if (!byScope || typeof byScope !== "object") return {};
  const out: BrokerCursorByScope = {};
  for (const [key, value] of Object.entries(byScope)) {
    if (!value || typeof value !== "object") continue;
    const cursor = (value as BrokerCursorEntry).cursor_ts;
    if (typeof cursor !== "number" || !Number.isFinite(cursor) || cursor <= 0) continue;
    out[key] = {
      cursor_ts: cursor,
      updated_unix_ms:
        typeof (value as BrokerCursorEntry).updated_unix_ms === "number"
          ? (value as BrokerCursorEntry).updated_unix_ms
          : undefined,
    };
  }
  return out;
}

export function normalizeBrokerEventRow(parsed: unknown, fallbackType?: string): BrokerEventRow | null {
  const event = asObjectRecord(parsed);
  if (!event && !fallbackType) return null;
  const eventType = String(event?.type || fallbackType || "message");
  if (!eventType) return null;
  return {
    type: eventType,
    ts_unix_ms: typeof event?.ts_unix_ms === "number" ? event.ts_unix_ms : undefined,
    event_id: event?.event_id ? String(event.event_id) : undefined,
    trace_id: event?.trace_id ? String(event.trace_id) : undefined,
    payload: asObjectRecord(event?.payload) ?? undefined,
  };
}

export function normalizeBrokerReplayEvents(events: BrokerEvent[] | undefined): BrokerEventRow[] {
  if (!Array.isArray(events)) return [];
  return events
    .map((event) => normalizeBrokerEventRow(event, event?.type))
    .filter((row): row is BrokerEventRow => !!row);
}

export function normalizeBrokerFanoutResults(rows: unknown, fallbackIds?: string[]): BrokerFanoutResult[] | null {
  if (!Array.isArray(rows)) return null;
  return rows.map((row, idx) => {
    const rec = asObjectRecord(row) ?? {};
    const dep = normalizeDeploymentId(rec.deployment_id ?? rec.deploymentId ?? rec.deployment ?? fallbackIds?.[idx] ?? "");
    const statusRaw = rec.status;
    const parsedStatus = typeof statusRaw === "number" ? statusRaw : Number(statusRaw || 0);
    const data = asObjectRecord(rec.data ?? rec.response ?? rec.body) ?? rec;
    return {
      deployment_id: dep,
      status: Number.isFinite(parsedStatus) ? parsedStatus : 0,
      data,
    };
  });
}
