export function safeToString(v: any): string {
  try {
    if (typeof v === "string") return v;
    if (v && typeof v === "object" && typeof v.message === "string") return v.message;
    return String(v);
  } catch {
    return "";
  }
}

export function isAutoplayNotAllowedLog(args: any[]): boolean {
  if (!Array.isArray(args) || args.length === 0) return false;
  const first = typeof args[0] === "string" ? args[0] : "";
  if (!/autoplay prevented/i.test(first)) return false;
  const rest = args.slice(1).map(safeToString).join(" ");
  const combined = `${first} ${rest}`.trim();
  return /NotAllowedError|user didn'?t interact|didn'?t interact|play\\(\\) failed/i.test(combined);
}

export function makeSceneConsole(): Console {
  const real: any = (typeof globalThis !== "undefined" ? (globalThis as any).console : undefined) || {};
  const wrap = (fn: any) => {
    return (...args: any[]) => {
      if (isAutoplayNotAllowedLog(args)) return;
      try {
        if (typeof fn === "function") fn.apply(real, args);
      } catch {
        // ignore
      }
    };
  };
  return {
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
    profile: wrap(real.profile),
    profileEnd: wrap(real.profileEnd),
  } as any;
}

export function toTestIdPart(v: string): string {
  return v.replace(/[^a-zA-Z0-9_-]/g, "_").slice(0, 120) || "empty";
}

export function clampInt(n: any, lo: number, hi: number, def: number): number {
  const v = typeof n === "number" ? n : Number(n);
  if (!Number.isFinite(v)) return def;
  return Math.min(Math.max(Math.trunc(v), lo), hi);
}

export function safeString(v: any): string {
  return typeof v === "string" ? v : "";
}

export function safeNumber(v: any, def: number): number {
  return typeof v === "number" && Number.isFinite(v) ? v : def;
}
