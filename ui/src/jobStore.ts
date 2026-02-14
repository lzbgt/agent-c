export const JOB_STORE_MAX = 64;
export const JOB_STORE_TTL_MS = 7 * 24 * 60 * 60 * 1000;

export function pruneJobsBySession(
  nowMs: number,
  jobs: Record<string, any>,
  maxEntries: number = JOB_STORE_MAX,
  ttlMs: number = JOB_STORE_TTL_MS,
): { next: Record<string, any>; changed: boolean } {
  const entries: Array<{ key: string; value: any; ts: number }> = [];
  for (const [key, value] of Object.entries(jobs)) {
    if (!value || typeof value !== "object") continue;
    const tsRaw =
      typeof value.updated_unix_ms === "number"
        ? value.updated_unix_ms
        : typeof value.started_unix_ms === "number"
          ? value.started_unix_ms
          : 0;
    entries.push({ key, value, ts: Number.isFinite(tsRaw) ? tsRaw : 0 });
  }

  const next: Record<string, any> = {};
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
