import { daemonFetchInit, daemonHeaders, type ApiAuth } from "../../api";

export type UnknownRecord = Record<string, unknown>;
export type SceneCleanup = () => void;
export type SceneDaemonContext = {
  base_url: string;
  yolo: boolean;
  auth_token: string;
  agentd_auth_token: string;
  headers: Record<string, string>;
  session_id?: string;
};

export function isUnknownRecord(value: unknown): value is UnknownRecord {
  return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}

export function safeObject(value: unknown): UnknownRecord {
  return isUnknownRecord(value) ? value : {};
}

export function safeToString(v: unknown): string {
  try {
    if (typeof v === "string") return v;
    if (isUnknownRecord(v) && typeof v.message === "string") return v.message;
    return String(v);
  } catch {
    return "";
  }
}

export function isAutoplayNotAllowedLog(args: unknown[]): boolean {
  if (!Array.isArray(args) || args.length === 0) return false;
  const first = typeof args[0] === "string" ? args[0] : "";
  if (!/autoplay prevented/i.test(first)) return false;
  const rest = args.slice(1).map(safeToString).join(" ");
  const combined = `${first} ${rest}`.trim();
  return /NotAllowedError|user didn'?t interact|didn'?t interact|play\\(\\) failed/i.test(combined);
}

export function makeSceneConsole(): Console {
  const real: Console = typeof globalThis !== "undefined" && globalThis.console ? globalThis.console : console;
  const wrap = <T extends (...args: never[]) => unknown>(fn: T): T =>
    ((...args: Parameters<T>) => {
      if (isAutoplayNotAllowedLog(args)) return;
      try {
        fn.apply(real, args);
      } catch {
        // ignore
      }
    }) as T;
  return {
    ...real,
    log: wrap(real.log),
    info: wrap(real.info),
    warn: wrap(real.warn),
    error: wrap(real.error),
    debug: wrap(real.debug),
    trace: wrap(real.trace),
    assert: wrap(real.assert),
    clear: wrap(real.clear),
    count: wrap(real.count),
    countReset: wrap(real.countReset),
    dir: wrap(real.dir),
    dirxml: wrap(real.dirxml),
    group: wrap(real.group),
    groupCollapsed: wrap(real.groupCollapsed),
    groupEnd: wrap(real.groupEnd),
    table: wrap(real.table),
    time: wrap(real.time),
    timeEnd: wrap(real.timeEnd),
    timeLog: wrap(real.timeLog),
    timeStamp: wrap(real.timeStamp),
  };
}

export function toTestIdPart(v: string): string {
  return v.replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 120) || "empty";
}

export function clampInt(n: unknown, lo: number, hi: number, def: number): number {
  const v = typeof n === "number" ? n : Number(n);
  if (!Number.isFinite(v)) return def;
  return Math.min(Math.max(Math.trunc(v), lo), hi);
}

export function safeString(v: unknown): string {
  return typeof v === "string" ? v : "";
}

export function safeNumber(v: unknown, def: number): number {
  return typeof v === "number" && Number.isFinite(v) ? v : def;
}

export function getSceneDaemonContext(args: {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
}): SceneDaemonContext {
  const { baseUrl, yolo, sessionId, daemonAuth } = args;
  const sessionIdValue = safeString(sessionId).trim();
  const authToken = safeString(daemonAuth?.token).replace(/^bearer\s+/i, "").trim();
  const agentdAuthToken =
    daemonAuth?.mode === "broker" ? safeString(daemonAuth.agentdToken).replace(/^bearer\s+/i, "").trim() : "";
  return {
    base_url: baseUrl,
    yolo,
    auth_token: authToken,
    agentd_auth_token: agentdAuthToken,
    headers: daemonHeaders(daemonAuth),
    session_id: sessionIdValue || undefined,
  };
}

export function createSceneArtifactUrlResolver(args: {
  baseUrl: string;
  yolo: boolean;
  sessionId?: string;
  daemonAuth?: ApiAuth;
  blobUrlsRef: { current: string[] };
}): (path: unknown) => Promise<string> {
  const { baseUrl, yolo, sessionId, daemonAuth, blobUrlsRef } = args;
  const sid = safeString(sessionId).trim();
  return async (path: unknown) => {
    const normalizedPath = safeString(path).trim();
    if (!normalizedPath) throw new Error("artifact.url requires path");
    const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
    const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(normalizedPath)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
    const response = await fetch(src, daemonFetchInit(daemonAuth));
    if (!response.ok) throw new Error(`file fetch failed: ${response.status}`);
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    blobUrlsRef.current.push(url);
    return url;
  };
}

export function getSceneCleanup(value: unknown): SceneCleanup | null {
  if (typeof value === "function") return value as SceneCleanup;
  if (isUnknownRecord(value) && typeof value.cleanup === "function") return value.cleanup as SceneCleanup;
  return null;
}

export function isSceneAutoplayUnlocked(): boolean {
  const globalWithFlag = globalThis as typeof globalThis & { __agentui_autoplay_unlocked?: unknown };
  return globalWithFlag.__agentui_autoplay_unlocked === true;
}
