export type ParsedJson = { ok: true; value: unknown } | { ok: false; error: string };
export type OrchestratorRevisionDiff = {
  added?: string[];
  removed?: string[];
  changed?: string[];
};
export type OrchestratorRevisionEntry = {
  version?: number;
  updated_unix_ms?: number;
  updated_by?: string;
  goal?: string;
  goal_changed?: boolean;
  goal_contract_changed?: boolean;
  goal_contract?: Record<string, unknown>;
  goal_contract_diff?: OrchestratorRevisionDiff;
  role_plan_snapshot?: Record<string, unknown>;
  role_plan_diff?: OrchestratorRevisionDiff;
};
export type OrchestratorRevisionEventPayload = OrchestratorRevisionEntry & {
  team_id?: string;
  orchestrator_run_id?: string;
};
export type RevisionFilterInput = {
  version?: unknown;
  updatedBy?: unknown;
  goal?: unknown;
  diffs?: Array<OrchestratorRevisionDiff | null | undefined>;
  changeLabels?: string[];
};

const asRecord = (value: unknown): Record<string, unknown> | null =>
  value && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : null;

const toStringArray = (value: unknown): string[] => {
  if (!Array.isArray(value)) return [];
  return value.map((item) => String(item).trim()).filter(Boolean);
};

export const parseJsonField = (raw: string, label: string): ParsedJson => {
  const trimmed = String(raw || "").trim();
  if (!trimmed) return { ok: true, value: undefined };
  try {
    return { ok: true, value: JSON.parse(trimmed) };
  } catch (err) {
    return { ok: false, error: `${label} JSON invalid: ${String(err)}` };
  }
};

export const fmtAge = (ms?: number | null) => {
  if (typeof ms !== "number" || !Number.isFinite(ms) || ms < 0) return "";
  if (ms < 1000) return `${Math.round(ms)}ms`;
  if (ms < 60000) return `${Math.round(ms / 100) / 10}s`;
  if (ms < 3600000) return `${Math.round(ms / 6000) / 10}m`;
  return `${Math.round(ms / 360000) / 10}h`;
};

export const toNumber = (val: unknown): number | null => {
  if (typeof val === "number" && Number.isFinite(val)) return val;
  if (typeof val === "string" && val.trim()) {
    const parsed = Number.parseFloat(val);
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
};

export const normalizeRevisionDiff = (raw: unknown): OrchestratorRevisionDiff | null => {
  const obj = asRecord(raw);
  if (!obj) return null;
  const added = toStringArray(obj.added);
  const removed = toStringArray(obj.removed);
  const changed = toStringArray(obj.changed);
  if (added.length === 0 && removed.length === 0 && changed.length === 0) return null;
  return {
    added: added.length > 0 ? added : undefined,
    removed: removed.length > 0 ? removed : undefined,
    changed: changed.length > 0 ? changed : undefined,
  };
};

export const normalizeRevisionEntry = (raw: unknown): OrchestratorRevisionEntry | null => {
  const obj = asRecord(raw);
  if (!obj) return null;
  const goalContract = asRecord(obj.goal_contract) ?? undefined;
  const rolePlanSnapshot = asRecord(obj.role_plan_snapshot) ?? undefined;
  return {
    version: toNumber(obj.version) ?? undefined,
    updated_unix_ms: toNumber(obj.updated_unix_ms) ?? undefined,
    updated_by: typeof obj.updated_by === "string" ? obj.updated_by : undefined,
    goal: typeof obj.goal === "string" ? obj.goal : undefined,
    goal_changed: obj.goal_changed === true ? true : undefined,
    goal_contract_changed: obj.goal_contract_changed === true ? true : undefined,
    goal_contract: goalContract,
    goal_contract_diff: normalizeRevisionDiff(obj.goal_contract_diff) ?? undefined,
    role_plan_snapshot: rolePlanSnapshot,
    role_plan_diff: normalizeRevisionDiff(obj.role_plan_diff) ?? undefined,
  };
};

export const normalizeRevisionEntries = (raw: unknown): OrchestratorRevisionEntry[] => {
  if (!Array.isArray(raw)) return [];
  return raw.map((item) => normalizeRevisionEntry(item)).filter((item): item is OrchestratorRevisionEntry => item !== null);
};

export const normalizeRevisionEventPayload = (raw: unknown): OrchestratorRevisionEventPayload | null => {
  const obj = asRecord(raw);
  if (!obj) return null;
  const entry = normalizeRevisionEntry(obj) ?? {};
  return {
    ...entry,
    team_id: typeof obj.team_id === "string" ? obj.team_id : undefined,
    orchestrator_run_id: typeof obj.orchestrator_run_id === "string" ? obj.orchestrator_run_id : undefined,
  };
};

export const revisionVersion = (entry?: OrchestratorRevisionEntry | null): number => {
  if (!entry) return 0;
  return entry.version ?? 0;
};

export const sortRevisions = (entries: OrchestratorRevisionEntry[]): OrchestratorRevisionEntry[] => {
  if (entries.length <= 1) return entries;
  const copy = [...entries];
  copy.sort((a, b) => revisionVersion(b) - revisionVersion(a));
  return copy;
};

export const diffSummary = (diff: OrchestratorRevisionDiff | null | undefined): string => {
  if (!diff) return "";
  const parts: string[] = [];
  const added = Array.isArray(diff.added) ? diff.added.length : 0;
  const removed = Array.isArray(diff.removed) ? diff.removed.length : 0;
  const changed = Array.isArray(diff.changed) ? diff.changed.length : 0;
  if (added) parts.push(`+${added}`);
  if (removed) parts.push(`-${removed}`);
  if (changed) parts.push(`~${changed}`);
  return parts.join(" ");
};

export const diffKeyLabel = (diff: OrchestratorRevisionDiff | null | undefined): string => {
  if (!diff) return "";
  const labels: string[] = [];
  const added = Array.isArray(diff.added) ? diff.added.map((v) => String(v)) : [];
  const removed = Array.isArray(diff.removed) ? diff.removed.map((v) => String(v)) : [];
  const changed = Array.isArray(diff.changed) ? diff.changed.map((v) => String(v)) : [];
  if (added.length > 0) labels.push(`added: ${added.join(", ")}`);
  if (removed.length > 0) labels.push(`removed: ${removed.join(", ")}`);
  if (changed.length > 0) labels.push(`changed: ${changed.join(", ")}`);
  return labels.join(" · ");
};

export const diffKeys = (diff: OrchestratorRevisionDiff | null | undefined): string[] => {
  if (!diff) return [];
  const out: string[] = [];
  if (Array.isArray(diff.added)) out.push(...diff.added.map((v) => String(v)));
  if (Array.isArray(diff.removed)) out.push(...diff.removed.map((v) => String(v)));
  if (Array.isArray(diff.changed)) out.push(...diff.changed.map((v) => String(v)));
  return out;
};

export const toLowerToken = (value: unknown): string | null => {
  if (value === undefined || value === null) return null;
  const str = String(value).trim();
  if (!str) return null;
  return str.toLowerCase();
};

export const revisionChangeLabels = (goalChanged?: boolean | null, contractChanged?: boolean | null): string[] => {
  const labels: string[] = [];
  if (goalChanged === true) labels.push("goal changed");
  if (contractChanged === true) labels.push("goal contract changed", "contract changed");
  return labels;
};

export const revisionFilterTokens = (input: RevisionFilterInput): string[] => {
  const tokens: string[] = [];
  const pushToken = (value: unknown) => {
    const token = toLowerToken(value);
    if (token) tokens.push(token);
  };
  pushToken(input.version);
  pushToken(input.updatedBy);
  pushToken(input.goal);
  (input.changeLabels || []).forEach((label) => pushToken(label));
  (input.diffs || []).forEach((diff) => {
    diffKeys(diff).forEach((key) => pushToken(key));
  });
  return Array.from(new Set(tokens));
};

export const matchesRevisionFilter = (filterLower: string, input: RevisionFilterInput): boolean => {
  if (!filterLower) return true;
  return revisionFilterTokens(input).some((token) => token.includes(filterLower));
};

export const formatJson = (value: unknown): string => {
  if (value === undefined) return "";
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return "";
  }
};
