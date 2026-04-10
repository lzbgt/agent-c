import {
  normalizeEventData,
  prettyJsonOrRaw,
  safeJsonParse,
  safeObject,
  safeTrunc,
} from "../../jsonUtils";

export type { UnknownRecord } from "../../jsonUtils";

type AutoRunGlobal = typeof globalThis & {
  __agentui_auto_run_once?: Record<string, boolean>;
};

export { normalizeEventData, prettyJsonOrRaw, safeJsonParse, safeObject, safeTrunc };

export function clampInt(n: unknown, lo: number, hi: number, def: number): number {
  const v = typeof n === "number" ? n : Number(n);
  if (!Number.isFinite(v)) return def;
  return Math.min(Math.max(Math.trunc(v), lo), hi);
}

export function isSensitiveKey(k: string): boolean {
  const s = String(k || "").toLowerCase();
  return (
    s.includes("secret") ||
    s.includes("token") ||
    s.includes("auth") ||
    s.includes("apikey") ||
    s.includes("api_key") ||
    s.includes("password") ||
    s.includes("passwd") ||
    s.includes("session") ||
    s.includes("cookie")
  );
}

export function tryParseUrl(s: string): URL | null {
  try {
    return new URL(s);
  } catch {
    return null;
  }
}

export function globalAutoRunOnceMap(): Record<string, boolean> {
  // React StrictMode in dev may mount/unmount/mount components, which resets refs.
  // Use a tiny global cache to avoid auto-running the same client RPC twice per page load.
  const g = globalThis as AutoRunGlobal;
  if (!g.__agentui_auto_run_once || typeof g.__agentui_auto_run_once !== "object") {
    g.__agentui_auto_run_once = {};
  }
  const m = g.__agentui_auto_run_once;
  try {
    const n = Object.keys(m).length;
    if (n > 2000) g.__agentui_auto_run_once = {};
  } catch {
    // ignore
  }
  return g.__agentui_auto_run_once || {};
}

export function createInlineWorker(source: string): Worker {
  const blob = new Blob([source], { type: "text/javascript" });
  const url = URL.createObjectURL(blob);
  const w = new Worker(url);
  // Best-effort: release URL immediately; Worker holds its own reference.
  try {
    URL.revokeObjectURL(url);
  } catch {
    // ignore
  }
  return w;
}
