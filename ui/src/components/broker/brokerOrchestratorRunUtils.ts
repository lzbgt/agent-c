export type ParsedJson = { ok: true; value: any } | { ok: false; error: string };

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

export const toNumber = (val: any): number | null => {
  if (typeof val === "number" && Number.isFinite(val)) return val;
  if (typeof val === "string" && val.trim()) {
    const parsed = Number.parseFloat(val);
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
};

export const normalizeRevisionEntries = (raw: any): Array<Record<string, any>> => {
  if (!Array.isArray(raw)) return [];
  return raw.filter((item) => item && typeof item === "object") as Array<Record<string, any>>;
};

export const revisionVersion = (entry?: Record<string, any> | null): number => {
  if (!entry) return 0;
  const val = toNumber(entry.version);
  return val ? val : 0;
};

export const sortRevisions = (entries: Array<Record<string, any>>): Array<Record<string, any>> => {
  if (entries.length <= 1) return entries;
  const copy = [...entries];
  copy.sort((a, b) => revisionVersion(b) - revisionVersion(a));
  return copy;
};

export const diffSummary = (diff: any): string => {
  if (!diff || typeof diff !== "object") return "";
  const parts: string[] = [];
  const added = Array.isArray(diff.added) ? diff.added.length : 0;
  const removed = Array.isArray(diff.removed) ? diff.removed.length : 0;
  const changed = Array.isArray(diff.changed) ? diff.changed.length : 0;
  if (added) parts.push(`+${added}`);
  if (removed) parts.push(`-${removed}`);
  if (changed) parts.push(`~${changed}`);
  return parts.join(" ");
};

export const diffKeyLabel = (diff: any): string => {
  if (!diff || typeof diff !== "object") return "";
  const labels: string[] = [];
  const added = Array.isArray(diff.added) ? diff.added.map((v: any) => String(v)) : [];
  const removed = Array.isArray(diff.removed) ? diff.removed.map((v: any) => String(v)) : [];
  const changed = Array.isArray(diff.changed) ? diff.changed.map((v: any) => String(v)) : [];
  if (added.length > 0) labels.push(`added: ${added.join(", ")}`);
  if (removed.length > 0) labels.push(`removed: ${removed.join(", ")}`);
  if (changed.length > 0) labels.push(`changed: ${changed.join(", ")}`);
  return labels.join(" · ");
};

export const diffKeys = (diff: any): string[] => {
  if (!diff || typeof diff !== "object") return [];
  const out: string[] = [];
  if (Array.isArray(diff.added)) out.push(...diff.added.map((v: any) => String(v)));
  if (Array.isArray(diff.removed)) out.push(...diff.removed.map((v: any) => String(v)));
  if (Array.isArray(diff.changed)) out.push(...diff.changed.map((v: any) => String(v)));
  return out;
};

export const toLowerToken = (value: any): string | null => {
  if (value === undefined || value === null) return null;
  const str = String(value).trim();
  if (!str) return null;
  return str.toLowerCase();
};

export const revisionChangeLabels = (goalChanged?: any, contractChanged?: any): string[] => {
  const labels: string[] = [];
  if (goalChanged === true) labels.push("goal changed");
  if (contractChanged === true) labels.push("goal contract changed", "contract changed");
  return labels;
};

export const revisionFilterTokens = (input: {
  version?: any;
  updatedBy?: any;
  goal?: any;
  diffs?: any[];
  changeLabels?: string[];
}): string[] => {
  const tokens: string[] = [];
  const pushToken = (value: any) => {
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

export const matchesRevisionFilter = (
  filterLower: string,
  input: {
    version?: any;
    updatedBy?: any;
    goal?: any;
    diffs?: any[];
    changeLabels?: string[];
  },
): boolean => {
  if (!filterLower) return true;
  return revisionFilterTokens(input).some((token) => token.includes(filterLower));
};

export const formatJson = (value: any): string => {
  if (value === undefined) return "";
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return "";
  }
};
