import type { TeamRunHandoffEventRecord } from "./teamRunStatusTypes";

const HANDOFF_STATES = new Set(["proposed", "accepted", "declined", "cancelled"]);

export const parseLineList = (raw: string): string[] =>
  String(raw || "")
    .split(/[\n,]+/)
    .map((item) => item.trim())
    .filter(Boolean);

export function normalizeHandoffEventRecord(raw: any): TeamRunHandoffEventRecord {
  if (!raw || typeof raw !== "object") return {};
  const obj = raw as Record<string, unknown>;
  const kind = typeof obj.kind === "string" && obj.kind.trim() ? obj.kind.trim().toLowerCase() : "role";
  const state = typeof obj.state === "string" && obj.state.trim() ? obj.state.trim().toLowerCase() : "proposed";
  return {
    handoff_id: typeof obj.handoff_id === "string" ? obj.handoff_id.trim() : undefined,
    kind,
    state: HANDOFF_STATES.has(state) ? state : "proposed",
    from_role: typeof obj.from_role === "string" ? obj.from_role.trim() : undefined,
    to_role: typeof obj.to_role === "string" ? obj.to_role.trim() : undefined,
    reason: typeof obj.reason === "string" ? obj.reason.trim() : undefined,
    message: typeof obj.message === "string" ? obj.message.trim() : undefined,
    source_deployment_id:
      typeof obj.source_deployment_id === "string" ? obj.source_deployment_id.trim() : undefined,
    source_session_id: typeof obj.source_session_id === "string" ? obj.source_session_id.trim() : undefined,
    target_deployment_id:
      typeof obj.target_deployment_id === "string" ? obj.target_deployment_id.trim() : undefined,
    target_session_id: typeof obj.target_session_id === "string" ? obj.target_session_id.trim() : undefined,
    ts_unix_ms: typeof obj.ts_unix_ms === "number" ? obj.ts_unix_ms : undefined,
    event_index: typeof obj.event_index === "number" ? obj.event_index : undefined,
    data: obj.data && typeof obj.data === "object" && !Array.isArray(obj.data) ? (obj.data as Record<string, unknown>) : undefined,
  };
}
