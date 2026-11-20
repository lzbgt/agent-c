export type RunWatchEntry = {
  job_id?: string;
  cursor?: number;
  started_unix_ms?: number;
  updated_unix_ms?: number;
  local_state?: "accepted" | "retired";
  retired_unix_ms?: number;
  [key: string]: unknown;
};

export type RunWatchByScope = Record<string, RunWatchEntry>;

// Authority tags are meaningful only in this browser's own store. Never trust
// tags supplied by a preference server as native acceptance/completion proof.
export function parseRunWatchByScope(value: unknown, local = false): RunWatchByScope {
  const result: RunWatchByScope = {};
  if (!value || typeof value !== "object" || Array.isArray(value)) return result;
  for (const [scope, raw] of Object.entries(value)) {
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) continue;
    const entry = raw as RunWatchEntry;
    if (typeof entry.job_id !== "string" || !entry.job_id || entry.job_id.length > 256) continue;
    const next: RunWatchEntry = { job_id: entry.job_id };
    for (const key of ["cursor", "started_unix_ms", "updated_unix_ms"] as const) {
      const number = entry[key];
      if (typeof number === "number" && Number.isSafeInteger(number) && number >= 0) next[key] = number;
    }
    if (local && (entry.local_state === "accepted" || entry.local_state === "retired")) {
      next.local_state = entry.local_state;
      if (entry.local_state === "retired" && Number.isSafeInteger(entry.retired_unix_ms) && entry.retired_unix_ms! >= 0) {
        next.retired_unix_ms = entry.retired_unix_ms;
      }
    }
    Object.defineProperty(result, scope, { value: next, enumerable: true, configurable: true, writable: true });
  }
  return result;
}

export function runWatchHints(watches: RunWatchByScope): RunWatchByScope {
  return parseRunWatchByScope(Object.fromEntries(Object.entries(watches).filter(([, value]) => value.local_state !== "retired")));
}

export function retireJobWatch(watches: RunWatchByScope, scope: string, jobId: string): RunWatchByScope {
  const current = watches[scope];
  if (!current || current.job_id !== jobId || current.local_state === "retired") return watches;
  return { ...watches, [scope]: { ...current, local_state: "retired", retired_unix_ms: Date.now() } };
}

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
  const next = parseRunWatchByScope(local, true);
  for (const [key, value] of Object.entries(parseRunWatchByScope(remote))) {
    // Timestamps order cache eviction, not job identity. A local accepted job,
    // retirement receipt or already selected hint always wins this scope.
    if (!Object.hasOwn(next, key)) Object.defineProperty(next, key, {
      value, enumerable: true, configurable: true, writable: true,
    });
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
    if (!Object.hasOwn(b, key) || !bv) return false;
    if ((av?.job_id || "") !== (bv?.job_id || "")) return false;
    if ((av?.cursor || 0) !== (bv?.cursor || 0)) return false;
    if (runWatchTs(av) !== runWatchTs(bv)) return false;
    if (av.local_state !== bv.local_state || av.retired_unix_ms !== bv.retired_unix_ms) return false;
  }
  return true;
};
