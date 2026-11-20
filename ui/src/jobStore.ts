import { parseRunWatchByScope, type RunWatchByScope, type RunWatchEntry } from "./runWatchPrefs";

export const JOB_STORE_MAX = 64;
export const JOB_STORE_TTL_MS = 7 * 24 * 60 * 60 * 1000;
export const UNVERIFIED_JOB_STATUS = "unverified";
export const UNVERIFIED_JOB_RECEIPT = "Run receipt unverified. Waiting for the native receipt.";

// Complete the durable handoff before a submission uncertainty fence is removed.
// The existing React preference uses a JSON string encoded inside localStorage.
function parseLocalWatches(value: unknown): RunWatchByScope {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("Saved job watches are invalid; submission remains held");
  }
  const jobs = value as RunWatchByScope;
  const parsed = parseRunWatchByScope(jobs, true);
  if (Object.keys(parsed).length !== Object.keys(jobs).length || Object.values(jobs).some(entry =>
    entry.local_state !== undefined && entry.local_state !== "accepted" && entry.local_state !== "retired")) {
    throw new Error("Saved job watches are invalid; submission remains held");
  }
  return parsed;
}

export function readJobWatches(): RunWatchByScope {
  const stored = localStorage.getItem("agentui.jobsBySession");
  const encoded = stored === null ? "{}" : JSON.parse(stored);
  if (typeof encoded !== "string") throw new Error("Saved job watches are invalid; submission remains held");
  return pruneJobsBySession(Date.now(), parseLocalWatches(JSON.parse(encoded))).next;
}

export function writeJobWatches(jobs: RunWatchByScope): RunWatchByScope {
  const next = pruneJobsBySession(Date.now(), parseLocalWatches(jobs)).next;
  const value = JSON.stringify(JSON.stringify(next));
  localStorage.setItem("agentui.jobsBySession", value);
  if (localStorage.getItem("agentui.jobsBySession") !== value) throw new Error("Unable to save job watch; submission remains held");
  return next;
}

export function persistAcceptedJobWatch(scope: string, jobId: string, startedMs = Date.now()) {
  return writeJobWatches({ ...readJobWatches(), [scope]: {
    job_id: jobId, cursor: 0, started_unix_ms: startedMs, local_state: "accepted",
  } });
}

export function hasSavedJobWatch(scope: string, sessionId?: string): boolean {
  try {
    const jobs = readJobWatches();
    const entry = jobs[scope] || (sessionId ? jobs[sessionId] : undefined);
    return Boolean(entry?.job_id && entry.local_state !== "retired");
  } catch { return true; } // Unavailable storage never authorizes queue dispatch.
}

export function isTerminalJobStatus(status: string | null | undefined): status is "done" | "error" | "cancelled" | "interrupted" {
  return status === "done" || status === "error" || status === "cancelled" || status === "interrupted";
}

export function pruneJobsBySession(
  nowMs: number,
  jobs: RunWatchByScope,
  maxEntries: number = JOB_STORE_MAX,
  ttlMs: number = JOB_STORE_TTL_MS,
): { next: RunWatchByScope; changed: boolean } {
  const entries: Array<{ key: string; value: RunWatchEntry; ts: number }> = [];
  for (const [key, value] of Object.entries(jobs)) {
    if (!value || typeof value !== "object" || Array.isArray(value)) continue;
    const tsRaw =
      value.local_state === "retired" && typeof value.retired_unix_ms === "number" ? value.retired_unix_ms :
      typeof value.updated_unix_ms === "number"
        ? value.updated_unix_ms
        : typeof value.started_unix_ms === "number"
          ? value.started_unix_ms
          : 0;
    entries.push({ key, value, ts: Number.isFinite(tsRaw) ? tsRaw : 0 });
  }

  const next: RunWatchByScope = Object.create(null);
  let changed = false;

  for (const e of entries) {
    if (e.value.local_state !== "accepted" && ttlMs > 0 && e.ts > 0 && nowMs - e.ts > ttlMs) {
      changed = true;
      continue;
    }
    next[e.key] = e.value;
  }

  const keys = Object.keys(next);
  if (maxEntries > 0 && keys.length > maxEntries) {
    const accepted = keys.filter(key => next[key].local_state === "accepted");
    if (accepted.length > maxEntries) throw new Error("Too many unfinished job watches; preserve existing receipts before continuing");
    const sorted = keys.filter(key => next[key].local_state !== "accepted")
      .map((key) => {
        const v = next[key];
        const tsRaw =
          v.local_state === "retired" && typeof v.retired_unix_ms === "number" ? v.retired_unix_ms :
          typeof v?.updated_unix_ms === "number"
            ? v.updated_unix_ms
            : typeof v?.started_unix_ms === "number"
              ? v.started_unix_ms
              : 0;
        return { key, retired: v.local_state === "retired" ? 1 : 0, ts: Number.isFinite(tsRaw) ? tsRaw : 0 };
      })
      .sort((a, b) => b.retired - a.retired || b.ts - a.ts);
    const keep = new Set([...accepted, ...sorted.slice(0, maxEntries - accepted.length).map((e) => e.key)]);
    for (const key of keys) {
      if (!keep.has(key)) {
        delete next[key];
        changed = true;
      }
    }
  }

  if (Object.keys(next).length !== Object.keys(jobs).length) changed = true;
  return { next, changed };
}
