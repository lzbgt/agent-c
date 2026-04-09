import type { AgentEvent } from "../api";
import type { SceneEntity } from "../components/SceneView";

type UnknownRecord = Record<string, unknown>;

export type HistoryEntry = {
  ts_unix_ms: number;
  prompt?: string;
  assistant_text?: string;
  events: AgentEvent[];
  ok?: boolean;
  job_id?: string;
  job_status?: string;
  live?: boolean;
  trace_id?: string;
  tools?: string;
  yolo?: boolean;
  host_policy?: string;
  effective_automation_profile?: string;
  automation_profile?: string;
  model?: string;
  base_url?: string;
};

export type DbMessageRow = {
  role?: string;
  content?: string;
  created_unix_ms?: number;
  content_truncated?: boolean;
  mm_json?: string;
  mm_bytes?: number;
};

export type DbRunSummaryRow = {
  run_id?: number;
  id?: number;
  ts_unix_ms?: number;
  ok?: boolean;
  error?: string;
};

export type DbToolRecordRow = {
  tool_name?: string;
  arguments_json?: string;
  result_text?: string;
  result_for_prompt_text?: string;
  result_truncated_for_prompt?: boolean;
  ts_unix_ms?: number;
  created_unix_ms?: number;
  updated_unix_ms?: number;
};

export type DbRunDetailRow = {
  ok: boolean;
  run?: DbRunSummaryRow;
  tool_records: DbToolRecordRow[];
};

export type DbUiActionRow = {
  id?: number;
  run_id?: number;
  ts_unix_ms?: number;
  tool_call_id?: string;
  type?: string;
  title?: string;
  message?: string;
  path?: string;
  mime?: string;
  repeat?: number;
  autoplay?: boolean;
  action?: UnknownRecord;
  action_json?: string;
};

export type DbClientEventRow = {
  id?: number;
  ts_unix_ms?: number;
  type?: string;
  data?: UnknownRecord;
  data_json?: string;
};

export type SessionArtifactRow = {
  artifact: unknown;
  path?: string;
  resolved_path?: string;
  tool_call_id?: string;
  kind?: string;
  title?: string;
};

export type ParsedToolArguments = {
  cmd?: string;
  argv?: string[];
};

export type SessionSceneSnapshot = {
  updated_unix_ms?: number;
  scene: Record<string, SceneEntity>;
};

export type PersistedConversationItem =
  | { kind: "message"; ts: number; message: DbMessageRow }
  | { kind: "tool_record"; ts: number; run?: DbRunSummaryRow; runId?: number; details?: DbRunDetailRow; toolRecord: DbToolRecordRow };

const isUnknownRecord = (value: unknown): value is UnknownRecord =>
  Boolean(value) && typeof value === "object" && !Array.isArray(value);

const asTrimmedString = (value: unknown): string | undefined => {
  if (typeof value !== "string") return undefined;
  const trimmed = value.trim();
  return trimmed || undefined;
};

const asFiniteNumber = (value: unknown): number | undefined => {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string" && value.trim()) {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return undefined;
};

const asOptionalBoolean = (value: unknown): boolean | undefined => {
  if (typeof value === "boolean") return value;
  if (typeof value === "number") return value !== 0;
  if (typeof value === "string") {
    const lowered = value.trim().toLowerCase();
    if (!lowered) return undefined;
    if (lowered === "true" || lowered === "1") return true;
    if (lowered === "false" || lowered === "0") return false;
  }
  return undefined;
};

const parseUnknownRecordJson = (value: unknown): UnknownRecord | undefined => {
  if (typeof value !== "string") return undefined;
  const trimmed = value.trim();
  if (!trimmed) return undefined;
  try {
    const parsed: unknown = JSON.parse(trimmed);
    return isUnknownRecord(parsed) ? parsed : undefined;
  } catch {
    return undefined;
  }
};

const toAgentEvent = (value: unknown): AgentEvent | null => {
  if (!isUnknownRecord(value)) return null;
  const type = asTrimmedString(value.type);
  if (!type) return null;
  return {
    type,
    trace_id: asTrimmedString(value.trace_id),
    data: value.data,
  };
};

export const normalizeAgentEvents = (value: unknown): AgentEvent[] =>
  Array.isArray(value)
    ? value.map((event) => toAgentEvent(event)).filter((event): event is AgentEvent => event !== null)
    : [];

const toHistoryEntry = (value: unknown): HistoryEntry | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    ts_unix_ms: asFiniteNumber(value.ts_unix_ms) ?? 0,
    prompt: asTrimmedString(value.prompt),
    assistant_text: typeof value.assistant_text === "string" ? value.assistant_text : undefined,
    events: normalizeAgentEvents(value.events),
    ok: asOptionalBoolean(value.ok),
    job_id: asTrimmedString(value.job_id),
    job_status: asTrimmedString(value.job_status),
    live: value.live === true ? true : undefined,
    trace_id: asTrimmedString(value.trace_id),
    tools: asTrimmedString(value.tools),
    yolo: typeof value.yolo === "boolean" ? value.yolo : undefined,
    host_policy: asTrimmedString(value.host_policy),
    effective_automation_profile: asTrimmedString(value.effective_automation_profile),
    automation_profile: asTrimmedString(value.automation_profile),
    model: asTrimmedString(value.model),
    base_url: asTrimmedString(value.base_url),
  };
};

export const normalizeHistoryEntries = (value: unknown): HistoryEntry[] =>
  Array.isArray(value) ? value.map((entry) => toHistoryEntry(entry)).filter((entry): entry is HistoryEntry => entry !== null) : [];

const toDbMessageRow = (value: unknown): DbMessageRow | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    role: asTrimmedString(value.role),
    content: typeof value.content === "string" ? value.content : undefined,
    created_unix_ms: asFiniteNumber(value.created_unix_ms),
    content_truncated: asOptionalBoolean(value.content_truncated),
    mm_json: typeof value.mm_json === "string" ? value.mm_json : undefined,
    mm_bytes: asFiniteNumber(value.mm_bytes),
  };
};

export const normalizeDbMessageRows = (value: unknown): DbMessageRow[] =>
  Array.isArray(value) ? value.map((row) => toDbMessageRow(row)).filter((row): row is DbMessageRow => row !== null) : [];

const toDbRunSummaryRow = (value: unknown): DbRunSummaryRow | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    run_id: asFiniteNumber(value.run_id),
    id: asFiniteNumber(value.id),
    ts_unix_ms: asFiniteNumber(value.ts_unix_ms),
    ok: asOptionalBoolean(value.ok),
    error: typeof value.error === "string" ? value.error : undefined,
  };
};

export const normalizeDbRunSummaryRows = (value: unknown): DbRunSummaryRow[] =>
  Array.isArray(value) ? value.map((row) => toDbRunSummaryRow(row)).filter((row): row is DbRunSummaryRow => row !== null) : [];

export const getDbRunNumericId = (row: DbRunSummaryRow): number | null => {
  if (typeof row.run_id === "number" && Number.isFinite(row.run_id)) return row.run_id;
  if (typeof row.id === "number" && Number.isFinite(row.id)) return row.id;
  return null;
};

const toDbToolRecordRow = (value: unknown): DbToolRecordRow | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    tool_name: asTrimmedString(value.tool_name),
    arguments_json: typeof value.arguments_json === "string" ? value.arguments_json : undefined,
    result_text: typeof value.result_text === "string" ? value.result_text : undefined,
    result_for_prompt_text: typeof value.result_for_prompt_text === "string" ? value.result_for_prompt_text : undefined,
    result_truncated_for_prompt: asOptionalBoolean(value.result_truncated_for_prompt),
    ts_unix_ms: asFiniteNumber(value.ts_unix_ms),
    created_unix_ms: asFiniteNumber(value.created_unix_ms),
    updated_unix_ms: asFiniteNumber(value.updated_unix_ms),
  };
};

export const normalizeDbRunDetailRow = (value: unknown): DbRunDetailRow | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    ok: value.ok === true,
    run: toDbRunSummaryRow(value.run) || undefined,
    tool_records: Array.isArray(value.tool_records)
      ? value.tool_records
          .map((record) => toDbToolRecordRow(record))
          .filter((record): record is DbToolRecordRow => record !== null)
      : [],
  };
};

const toDbUiActionRow = (value: unknown): DbUiActionRow | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    id: asFiniteNumber(value.id),
    run_id: asFiniteNumber(value.run_id),
    ts_unix_ms: asFiniteNumber(value.ts_unix_ms),
    tool_call_id: asTrimmedString(value.tool_call_id),
    type: asTrimmedString(value.type),
    title: asTrimmedString(value.title),
    message: typeof value.message === "string" ? value.message : undefined,
    path: asTrimmedString(value.path),
    mime: asTrimmedString(value.mime),
    repeat: asFiniteNumber(value.repeat),
    autoplay: asOptionalBoolean(value.autoplay),
    action: isUnknownRecord(value.action) ? value.action : parseUnknownRecordJson(value.action_json),
    action_json: typeof value.action_json === "string" ? value.action_json : undefined,
  };
};

export const normalizeDbUiActionRows = (value: unknown): DbUiActionRow[] =>
  Array.isArray(value) ? value.map((row) => toDbUiActionRow(row)).filter((row): row is DbUiActionRow => row !== null) : [];

const toDbClientEventRow = (value: unknown): DbClientEventRow | null => {
  if (!isUnknownRecord(value)) return null;
  return {
    id: asFiniteNumber(value.id),
    ts_unix_ms: asFiniteNumber(value.ts_unix_ms),
    type: asTrimmedString(value.type),
    data: isUnknownRecord(value.data) ? value.data : parseUnknownRecordJson(value.data_json),
    data_json: typeof value.data_json === "string" ? value.data_json : undefined,
  };
};

export const normalizeDbClientEventRows = (value: unknown): DbClientEventRow[] =>
  Array.isArray(value)
    ? value.map((row) => toDbClientEventRow(row)).filter((row): row is DbClientEventRow => row !== null)
    : [];

export const normalizeSessionArtifactRows = (value: unknown): SessionArtifactRow[] => {
  if (!Array.isArray(value)) return [];
  return value.flatMap((row) => {
    if (!isUnknownRecord(row)) return [];
    const nestedData = isUnknownRecord(row.data) ? row.data : null;
    const artifact = nestedData?.artifact ?? row.artifact ?? row;
    if (artifact === undefined) return [];
    const artifactRecord = isUnknownRecord(artifact) ? artifact : null;
    return [
      {
        artifact,
        path: asTrimmedString(artifactRecord?.path),
        resolved_path: asTrimmedString(artifactRecord?.resolved_path),
        tool_call_id:
          asTrimmedString(nestedData?.tool_call_id) ??
          asTrimmedString(row.tool_call_id) ??
          asTrimmedString(artifactRecord?.tool_call_id),
        kind: asTrimmedString(artifactRecord?.kind) ?? asTrimmedString(row.kind),
        title: asTrimmedString(artifactRecord?.title) ?? asTrimmedString(row.title),
      },
    ];
  });
};

const toSceneEntity = (value: unknown, fallbackId?: string): SceneEntity | null => {
  if (!isUnknownRecord(value)) return null;
  const id = asTrimmedString(value.id) ?? asTrimmedString(fallbackId);
  const kind = asTrimmedString(value.kind);
  if (!id || !kind) return null;
  return {
    id,
    kind,
    title: asTrimmedString(value.title),
    props: isUnknownRecord(value.props) ? value.props : undefined,
    created_ms: asFiniteNumber(value.created_ms),
    updated_ms: asFiniteNumber(value.updated_ms),
  };
};

export const normalizeSceneEntityStore = (value: unknown): Record<string, SceneEntity> => {
  if (!isUnknownRecord(value)) return {};
  const next: Record<string, SceneEntity> = {};
  for (const [key, raw] of Object.entries(value)) {
    const entity = toSceneEntity(raw, key);
    if (!entity) continue;
    next[entity.id] = entity;
  }
  return next;
};

export const normalizeSessionSceneSnapshot = (value: unknown): SessionSceneSnapshot | null => {
  if (!isUnknownRecord(value) || value.ok !== true) return null;
  return {
    updated_unix_ms: asFiniteNumber(value.updated_unix_ms),
    scene: normalizeSceneEntityStore(value.scene),
  };
};

export const parseToolArgumentsJson = (raw: string): ParsedToolArguments | null => {
  const trimmed = String(raw || "").trim();
  if (!trimmed) return null;
  try {
    const parsed: unknown = JSON.parse(trimmed);
    if (!isUnknownRecord(parsed)) return null;
    const argv = Array.isArray(parsed.argv)
      ? parsed.argv.map((value) => (typeof value === "string" ? value : "")).filter(Boolean)
      : undefined;
    return {
      cmd: asTrimmedString(parsed.cmd),
      argv: argv && argv.length > 0 ? argv : undefined,
    };
  } catch {
    return null;
  }
};

export const buildPersistedConversationItems = (
  dbMessages: DbMessageRow[],
  dbRuns: DbRunSummaryRow[],
  dbRunDetailsById: Record<number, DbRunDetailRow>,
): PersistedConversationItem[] => {
  const items: PersistedConversationItem[] = [];
  for (const message of dbMessages) {
    const ts = typeof message.created_unix_ms === "number" ? message.created_unix_ms : 0;
    if (!ts) continue;
    items.push({ kind: "message", ts, message });
  }
  for (const run of dbRuns) {
    const ts = typeof run.ts_unix_ms === "number" ? run.ts_unix_ms : 0;
    if (!ts) continue;
    const runId = getDbRunNumericId(run);
    const details = runId !== null ? dbRunDetailsById[runId] : undefined;
    const toolRecords = details?.tool_records || [];
    toolRecords.forEach((toolRecord, idx) => {
      const toolTs = toolRecord.ts_unix_ms || toolRecord.created_unix_ms || toolRecord.updated_unix_ms || ts + idx + 1;
      items.push({
        kind: "tool_record",
        ts: toolTs,
        run,
        runId: runId ?? undefined,
        details,
        toolRecord,
      });
    });
  }
  items.sort((a, b) => a.ts - b.ts);
  return items;
};

export const findLastAssistantMessage = (messages: DbMessageRow[]): DbMessageRow | null => {
  let last: DbMessageRow | null = null;
  for (const message of messages) {
    if (message.role !== "assistant") continue;
    const ts = typeof message.created_unix_ms === "number" ? message.created_unix_ms : 0;
    if (!last || ts >= (last.created_unix_ms || 0)) last = message;
  }
  return last;
};

export const findLastRun = (runs: DbRunSummaryRow[]): DbRunSummaryRow | null => {
  let last: DbRunSummaryRow | null = null;
  for (const run of runs) {
    const ts = typeof run.ts_unix_ms === "number" ? run.ts_unix_ms : 0;
    if (!last || ts >= (last.ts_unix_ms || 0)) last = run;
  }
  return last;
};
