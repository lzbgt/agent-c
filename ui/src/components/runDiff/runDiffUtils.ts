export type JsonDiffEntry = { path: string; a: unknown; b: unknown };
export const MAX_DIFFS = 200;

export const stringifyJson = (value: unknown) => {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value ?? "");
  }
};

export const parseJsonLoose = (value: unknown) => {
  if (typeof value === "string") {
    try {
      return JSON.parse(value);
    } catch {
      return value;
    }
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

export const extractUsage = (resp: any) => {
  if (!resp || typeof resp !== "object") return null;
  const usage = resp.usage && typeof resp.usage === "object" ? resp.usage : {};
  const totalTokens = usage.total_tokens ?? resp.total_tokens;
  const promptTokens = usage.prompt_tokens ?? resp.prompt_tokens;
  const completionTokens = usage.completion_tokens ?? resp.completion_tokens;
  const totalCost = resp.total_cost ?? resp.cost ?? usage.total_cost ?? usage.cost;
  const out: Record<string, number> = {};
  if (typeof totalTokens === "number") out.total_tokens = totalTokens;
  if (typeof promptTokens === "number") out.prompt_tokens = promptTokens;
  if (typeof completionTokens === "number") out.completion_tokens = completionTokens;
  if (typeof totalCost === "number") out.total_cost = totalCost;
  return Object.keys(out).length > 0 ? out : null;
};

export const extractUsageFromRunRow = (run: any) => {
  if (!run || typeof run !== "object") return null;
  const response = parseJsonLoose(run.response_json ?? run.response);
  return extractUsage(response);
};

export const formatUsage = (usage?: Record<string, number> | null) => {
  if (!usage) return "";
  const parts: string[] = [];
  if (typeof usage.total_tokens === "number") parts.push(`tokens ${usage.total_tokens}`);
  if (typeof usage.prompt_tokens === "number") parts.push(`prompt ${usage.prompt_tokens}`);
  if (typeof usage.completion_tokens === "number") parts.push(`completion ${usage.completion_tokens}`);
  if (typeof usage.total_cost === "number") parts.push(`cost ${usage.total_cost.toFixed(4)}`);
  return parts.join(" · ");
};

export const formatUsageDelta = (a?: Record<string, number> | null, b?: Record<string, number> | null) => {
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

export const normalizeEvent = (row: any) => {
  const data = parseJsonLoose(row?.data_json ?? row?.data);
  return {
    id: row?.event_id ?? row?.id ?? null,
    ts_unix_ms: row?.ts_unix_ms ?? row?.ts ?? null,
    type: row?.type ?? "",
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

export const normalizeArtifact = (row: any) => {
  const artifactJson = parseJsonLoose(row?.artifact_json ?? row?.artifact ?? row);
  return {
    id: row?.id ?? null,
    path: row?.path ?? artifactJson?.path ?? "",
    kind: row?.kind ?? artifactJson?.kind ?? "",
    mime: row?.mime ?? artifactJson?.mime ?? "",
    title: row?.title ?? artifactJson?.title ?? "",
    tool_call_id: row?.tool_call_id ?? artifactJson?.tool_call_id ?? "",
    autoplay: row?.autoplay ?? artifactJson?.autoplay,
    repeat: row?.repeat ?? artifactJson?.repeat,
    artifact_json: artifactJson,
  };
};

export const artifactFingerprint = (artifact: ReturnType<typeof normalizeArtifact>) =>
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

export const formatArtifactSummary = (artifact?: ReturnType<typeof normalizeArtifact> | null) => {
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
  usageA: Record<string, number> | null;
  usageB: Record<string, number> | null;
};

export type DbDiffState = {
  eventsA: Array<ReturnType<typeof normalizeEvent>>;
  eventsB: Array<ReturnType<typeof normalizeEvent>>;
  eventDiffs: JsonDiffEntry[];
  typeDiffs: Array<{ type: string; a: number; b: number }>;
  added: Array<{ key: string; item: ReturnType<typeof normalizeArtifact> }>;
  removed: Array<{ key: string; item: ReturnType<typeof normalizeArtifact> }>;
  changed: Array<{
    key: string;
    a: ReturnType<typeof normalizeArtifact>;
    b: ReturnType<typeof normalizeArtifact>;
    diffs: JsonDiffEntry[];
  }>;
  usageDbA: Record<string, number> | null;
  usageDbB: Record<string, number> | null;
};
