import type { RunWatchByScope, RunWatchEntry } from "./runWatchPrefs";

export const JOB_STORE_MAX = 64;
export const JOB_STORE_TTL_MS = 7 * 24 * 60 * 60 * 1000;
export const UNVERIFIED_JOB_STATUS = "unverified";
export const UNVERIFIED_JOB_RECEIPT = "Run receipt unverified. Waiting for the native receipt.";

// Complete the durable handoff before a submission uncertainty fence is removed.
// The existing React preference uses a JSON string encoded inside localStorage.
export function persistAcceptedJobWatch(scope: string, jobId: string, startedMs = Date.now()) {
  const stored = localStorage.getItem("agentui.jobsBySession");
  const encoded = stored === null ? "{}" : JSON.parse(stored);
  if (typeof encoded !== "string") throw new Error("Saved job watches are invalid; submission remains held");
  const jobs = JSON.parse(encoded);
  if (!jobs || typeof jobs !== "object" || Array.isArray(jobs)) throw new Error("Saved job watches are invalid; submission remains held");
  // Current acceptance outranks cached timestamps, including a clock rollback.
  // Prune older watches first, then reserve the last slot for this exact job.
  const previous = pruneJobsBySession(Date.now(), jobs, JOB_STORE_MAX - 1).next;
  const next = { ...previous, [scope]: { job_id: jobId, cursor: 0, started_unix_ms: startedMs } };
  const value = JSON.stringify(JSON.stringify(next));
  localStorage.setItem("agentui.jobsBySession", value);
  if (localStorage.getItem("agentui.jobsBySession") !== value) throw new Error("Unable to save accepted job; submission remains held");
  return next;
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
      typeof value.updated_unix_ms === "number"
        ? value.updated_unix_ms
        : typeof value.started_unix_ms === "number"
          ? value.started_unix_ms
          : 0;
    entries.push({ key, value, ts: Number.isFinite(tsRaw) ? tsRaw : 0 });
  }

  const next: RunWatchByScope = {};
  let changed = false;

  for (const e of entries) {
    if (ttlMs > 0 && e.ts > 0 && nowMs - e.ts > ttlMs) {
      changed = true;
      continue;
    }
    next[e.key] = e.value;
  }

  const keys = Object.keys(next);
  if (maxEntries > 0 && keys.length > maxEntries) {
    const sorted = keys
      .map((key) => {
        const v = next[key];
        const tsRaw =
          typeof v?.updated_unix_ms === "number"
            ? v.updated_unix_ms
            : typeof v?.started_unix_ms === "number"
              ? v.started_unix_ms
              : 0;
        return { key, ts: Number.isFinite(tsRaw) ? tsRaw : 0 };
      })
      .sort((a, b) => b.ts - a.ts);
    const keep = new Set(sorted.slice(0, maxEntries).map((e) => e.key));
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
