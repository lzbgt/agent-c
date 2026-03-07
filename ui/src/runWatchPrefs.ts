export type RunWatchEntry = {
  job_id?: string;
  cursor?: number;
  started_unix_ms?: number;
  updated_unix_ms?: number;
  [key: string]: unknown;
};

export type RunWatchByScope = Record<string, RunWatchEntry>;

export const extractRunWatchByScope = (prefs: unknown): RunWatchByScope => {
  const root =
    prefs && typeof prefs === "object" && "run_watch" in prefs
      ? (prefs as { run_watch?: unknown }).run_watch
      : null;
  const byScope =
    root && typeof root === "object" && "by_scope" in root
      ? (root as { by_scope?: unknown }).by_scope
      : null;
  if (!byScope || typeof byScope !== "object" || Array.isArray(byScope)) return {};
  return byScope as RunWatchByScope;
};

export const runWatchTs = (value: RunWatchEntry | null | undefined): number => {
  const updated = typeof value?.updated_unix_ms === "number" ? value.updated_unix_ms : 0;
  if (Number.isFinite(updated) && updated > 0) return updated;
  const started = typeof value?.started_unix_ms === "number" ? value.started_unix_ms : 0;
  return Number.isFinite(started) ? started : 0;
};

export const mergeRunWatchByScope = (local: RunWatchByScope, remote: RunWatchByScope): RunWatchByScope => {
  const next: RunWatchByScope = { ...(local || {}) };
  for (const [key, value] of Object.entries(remote || {})) {
    if (!value || typeof value !== "object" || Array.isArray(value)) continue;
    const cur = next[key];
    if (!cur || runWatchTs(value) >= runWatchTs(cur)) {
      next[key] = value;
    }
  }
  return next;
};

export const runWatchMapsEqual = (a: RunWatchByScope, b: RunWatchByScope): boolean => {
  const keysA = Object.keys(a || {});
  const keysB = Object.keys(b || {});
  if (keysA.length !== keysB.length) return false;
  for (const key of keysA) {
    const av = a[key];
    const bv = b[key];
    if (!bv) return false;
    if ((av?.job_id || "") !== (bv?.job_id || "")) return false;
    if ((av?.cursor || 0) !== (bv?.cursor || 0)) return false;
    if (runWatchTs(av) !== runWatchTs(bv)) return false;
  }
  return true;
};
