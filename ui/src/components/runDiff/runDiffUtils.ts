import { safeJsonParse, safeObject } from "../../jsonUtils";

export type JsonDiffEntry = { path: string; a: unknown; b: unknown };
export const MAX_DIFFS = 200;
export type UsageSummary = Record<string, number>;

type RunRowLike = {
  response_json?: unknown;
  response?: unknown;
};

type EventRowLike = {
  event_id?: unknown;
  id?: unknown;
  ts_unix_ms?: unknown;
  ts?: unknown;
  type?: unknown;
  data_json?: unknown;
  data?: unknown;
};

type ArtifactRowLike = {
  id?: unknown;
  path?: unknown;
  kind?: unknown;
  mime?: unknown;
  title?: unknown;
  tool_call_id?: unknown;
  autoplay?: unknown;
  repeat?: unknown;
  artifact_json?: unknown;
  artifact?: unknown;
};

export type NormalizedEvent = {
  id: string | number | null;
  ts_unix_ms: number | null;
  type: string;
  data: unknown;
};

export type NormalizedArtifact = {
  id: string | number | null;
  path: string;
  kind: string;
  mime: string;
  title: string;
  tool_call_id: string;
  autoplay: unknown;
  repeat: unknown;
  artifact_json: unknown;
};

function asFiniteNumber(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function asStringOrEmpty(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function asId(value: unknown): string | number | null {
  return typeof value === "string" || typeof value === "number" ? value : null;
}

export const stringifyJson = (value: unknown) => {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value ?? "");
  }
};

export const parseJsonLoose = (value: unknown) => {
  if (typeof value === "string") {
    return safeJsonParse(value) ?? value;
  }
  return value;
};

export const formatDiffValue = (value: unknown) => {
  if (value === null) return "null";
  if (value === undefined) return "undefined";
  if (typeof value === "string") return value.length > 120 ? `${value.slice(0, 120)}…` : value;
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  try {
    const raw = JSON.stringify(value);
    if (raw.length > 160) return `${raw.slice(0, 160)}…`;
    return raw;
  } catch {
    return String(value);
  }
};

const collectJsonDiffs = (
  a: unknown,
  b: unknown,
  path: string,
  out: JsonDiffEntry[],
  maxDiffs: number,
  depth: number,
  maxDepth: number,
  maxArrayItems: number,
) => {
  if (out.length >= maxDiffs) return;
  if (a === b) return;
  if (depth >= maxDepth) {
    out.push({ path, a, b });
    return;
  }
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) {
      out.push({ path: path ? `${path}.length` : "length", a: a.length, b: b.length });
    }
    const limit = Math.min(a.length, b.length, maxArrayItems);
    for (let i = 0; i < limit; i += 1) {
      collectJsonDiffs(a[i], b[i], `${path}[${i}]`, out, maxDiffs, depth + 1, maxDepth, maxArrayItems);
      if (out.length >= maxDiffs) return;
    }
    if (a.length > maxArrayItems || b.length > maxArrayItems) {
      out.push({ path: path ? `${path}[truncated]` : "[truncated]", a: a.length, b: b.length });
    }
    return;
  }
  if (a && b && typeof a === "object" && typeof b === "object") {
    const keys = new Set<string>([...Object.keys(a as Record<string, unknown>), ...Object.keys(b as Record<string, unknown>)]);
    const sorted = Array.from(keys).sort();
    for (const key of sorted) {
      const nextPath = path ? `${path}.${key}` : key;
      collectJsonDiffs(
        (a as Record<string, unknown>)[key],
        (b as Record<string, unknown>)[key],
        nextPath,
        out,
        maxDiffs,
        depth + 1,
        maxDepth,
        maxArrayItems,
      );
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  out.push({ path, a, b });
};

export const diffJson = (a: unknown, b: unknown, maxDiffs = MAX_DIFFS) => {
  const out: JsonDiffEntry[] = [];
  collectJsonDiffs(a, b, "", out, maxDiffs, 0, 12, 50);
  return out;
};

export const diffJsonWithPath = (a: unknown, b: unknown, path: string, maxDiffs = MAX_DIFFS) => {
  const out: JsonDiffEntry[] = [];
  collectJsonDiffs(a, b, path, out, maxDiffs, 0, 12, 50);
  return out;
};

export const extractUsage = (resp: unknown): UsageSummary | null => {
  const record = safeObject(resp);
  const usage = safeObject(record.usage);
  const totalTokens = usage.total_tokens ?? record.total_tokens;
  const promptTokens = usage.prompt_tokens ?? record.prompt_tokens;
  const completionTokens = usage.completion_tokens ?? record.completion_tokens;
  const totalCost = record.total_cost ?? record.cost ?? usage.total_cost ?? usage.cost;
  const out: UsageSummary = {};
  if (typeof totalTokens === "number") out.total_tokens = totalTokens;
  if (typeof promptTokens === "number") out.prompt_tokens = promptTokens;
  if (typeof completionTokens === "number") out.completion_tokens = completionTokens;
  if (typeof totalCost === "number") out.total_cost = totalCost;
  return Object.keys(out).length > 0 ? out : null;
};

export const extractUsageFromRunRow = (run: RunRowLike | unknown) => {
  const record = safeObject(run);
  const response = parseJsonLoose(record.response_json ?? record.response);
  return extractUsage(response);
};

export const formatUsage = (usage?: UsageSummary | null) => {
  if (!usage) return "";
  const parts: string[] = [];
  if (typeof usage.total_tokens === "number") parts.push(`tokens ${usage.total_tokens}`);
  if (typeof usage.prompt_tokens === "number") parts.push(`prompt ${usage.prompt_tokens}`);
  if (typeof usage.completion_tokens === "number") parts.push(`completion ${usage.completion_tokens}`);
  if (typeof usage.total_cost === "number") parts.push(`cost ${usage.total_cost.toFixed(4)}`);
  return parts.join(" · ");
};

export const formatUsageDelta = (a?: UsageSummary | null, b?: UsageSummary | null) => {
  if (!a || !b) return "";
  const keys = Array.from(new Set([...Object.keys(a), ...Object.keys(b)])).sort();
  const parts: string[] = [];
  for (const key of keys) {
    const av = a[key];
    const bv = b[key];
    if (typeof av !== "number" || typeof bv !== "number") continue;
    const delta = bv - av;
    const sign = delta > 0 ? "+" : delta < 0 ? "-" : "";
    const magnitude = Math.abs(delta);
    parts.push(`${key} ${sign}${magnitude % 1 === 0 ? magnitude : magnitude.toFixed(4)}`);
  }
  return parts.join(" · ");
};

export const normalizeEvent = (row: EventRowLike | unknown): NormalizedEvent => {
  const record = safeObject(row);
  const data = parseJsonLoose(record.data_json ?? record.data);
  return {
    id: asId(record.event_id ?? record.id),
    ts_unix_ms: asFiniteNumber(record.ts_unix_ms ?? record.ts),
    type: asStringOrEmpty(record.type),
    data,
  };
};

export const countByType = (events: Array<{ type: string }>) => {
  const counts = new Map<string, number>();
  for (const ev of events) {
    const key = String(ev.type || "unknown");
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  return counts;
};

export const normalizeArtifact = (row: ArtifactRowLike | unknown): NormalizedArtifact => {
  const record = safeObject(row);
  const artifactJson = parseJsonLoose(record.artifact_json ?? record.artifact ?? row);
  const artifactJsonRecord = safeObject(artifactJson);
  return {
    id: asId(record.id),
    path: asStringOrEmpty(record.path ?? artifactJsonRecord.path),
    kind: asStringOrEmpty(record.kind ?? artifactJsonRecord.kind),
    mime: asStringOrEmpty(record.mime ?? artifactJsonRecord.mime),
    title: asStringOrEmpty(record.title ?? artifactJsonRecord.title),
    tool_call_id: asStringOrEmpty(record.tool_call_id ?? artifactJsonRecord.tool_call_id),
    autoplay: record.autoplay ?? artifactJsonRecord.autoplay,
    repeat: record.repeat ?? artifactJsonRecord.repeat,
    artifact_json: artifactJson,
  };
};

export const artifactFingerprint = (artifact: NormalizedArtifact) =>
  stringifyJson({
    path: artifact.path,
    kind: artifact.kind,
    mime: artifact.mime,
    title: artifact.title,
    tool_call_id: artifact.tool_call_id,
    autoplay: artifact.autoplay,
    repeat: artifact.repeat,
    artifact_json: artifact.artifact_json,
  });

export const formatArtifactSummary = (artifact?: NormalizedArtifact | null) => {
  if (!artifact) return "";
  const parts = [artifact.path || "(no path)"];
  if (artifact.kind) parts.push(artifact.kind);
  if (artifact.mime) parts.push(artifact.mime);
  if (artifact.title) parts.push(`"${artifact.title}"`);
  return parts.join(" · ");
};

export type ReplayDiffState = {
  requestDiffs: JsonDiffEntry[];
  responseDiffs: JsonDiffEntry[];
  toolDiffs: JsonDiffEntry[];
  usageA: UsageSummary | null;
  usageB: UsageSummary | null;
};

export type DbDiffState = {
  eventsA: NormalizedEvent[];
  eventsB: NormalizedEvent[];
  eventDiffs: JsonDiffEntry[];
  typeDiffs: Array<{ type: string; a: number; b: number }>;
  added: Array<{ key: string; item: NormalizedArtifact }>;
  removed: Array<{ key: string; item: NormalizedArtifact }>;
  changed: Array<{
    key: string;
    a: NormalizedArtifact;
    b: NormalizedArtifact;
    diffs: JsonDiffEntry[];
  }>;
  usageDbA: UsageSummary | null;
  usageDbB: UsageSummary | null;
};
