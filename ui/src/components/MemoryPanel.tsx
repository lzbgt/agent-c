import React from "react";
import {
  apiMemoryCheckpoints,
  apiMemoryCorrelate,
  apiMemoryIndex,
  apiMemoryQuery,
  type ApiAuth,
} from "../api";
import FieldLabel from "./FieldLabel";

export type MemoryPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth: ApiAuth;
};

const stringifyJson = (value: unknown) => {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value ?? "");
  }
};

export default function MemoryPanel(props: MemoryPanelProps) {
  const base = String(props.baseUrl || "").trim().replace(/\/+$/, "");
  const canQuery = base.length > 0;

  const [queryPrefix, setQueryPrefix] = React.useState<string>("");
  const [queryLimit, setQueryLimit] = React.useState<string>("50");
  const [queryStructuredPath, setQueryStructuredPath] = React.useState<string>("");
  const [queryResult, setQueryResult] = React.useState<any | null>(null);
  const [queryError, setQueryError] = React.useState<string | null>(null);
  const [queryBusy, setQueryBusy] = React.useState<boolean>(false);

  const [traceId, setTraceId] = React.useState<string>("");
  const [correlatePrefix, setCorrelatePrefix] = React.useState<string>("");
  const [correlateStructuredPath, setCorrelateStructuredPath] = React.useState<string>("");
  const [correlateMaxEntries, setCorrelateMaxEntries] = React.useState<string>("50");
  const [correlateTimeline, setCorrelateTimeline] = React.useState<boolean>(false);
  const [correlateResult, setCorrelateResult] = React.useState<any | null>(null);
  const [correlateError, setCorrelateError] = React.useState<string | null>(null);
  const [correlateBusy, setCorrelateBusy] = React.useState<boolean>(false);

  const [checkpointLimit, setCheckpointLimit] = React.useState<string>("20");
  const [checkpointStructuredPath, setCheckpointStructuredPath] = React.useState<string>("");
  const [checkpointResult, setCheckpointResult] = React.useState<any | null>(null);
  const [checkpointError, setCheckpointError] = React.useState<string | null>(null);
  const [checkpointBusy, setCheckpointBusy] = React.useState<boolean>(false);

  const [indexSessionId, setIndexSessionId] = React.useState<string>("");
  const [indexIncludeStructured, setIndexIncludeStructured] = React.useState<boolean>(true);
  const [indexIncludeCore, setIndexIncludeCore] = React.useState<boolean>(true);
  const [indexIncludeDaily, setIndexIncludeDaily] = React.useState<boolean>(true);
  const [indexIncludeSession, setIndexIncludeSession] = React.useState<boolean>(false);
  const [indexDailyDays, setIndexDailyDays] = React.useState<string>("2");
  const [indexResult, setIndexResult] = React.useState<any | null>(null);
  const [indexError, setIndexError] = React.useState<string | null>(null);
  const [indexBusy, setIndexBusy] = React.useState<boolean>(false);

  const parsePositiveInt = (raw: string, fallback: number) => {
    const n = Number.parseInt(String(raw || "").trim(), 10);
    if (!Number.isFinite(n) || n <= 0) return fallback;
    return n;
  };

  const runQuery = async () => {
    setQueryError(null);
    setQueryResult(null);
    if (!canQuery) {
      setQueryError("missing base URL");
      return;
    }
    setQueryBusy(true);
    try {
      const res = await apiMemoryQuery(
        base,
        {
          keyPrefix: queryPrefix.trim() || undefined,
          structuredPath: queryStructuredPath.trim() || undefined,
          limit: parsePositiveInt(queryLimit, 50),
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "query failed");
      setQueryResult(res);
    } catch (e) {
      setQueryError(String(e));
    } finally {
      setQueryBusy(false);
    }
  };

  const runCorrelate = async () => {
    setCorrelateError(null);
    setCorrelateResult(null);
    if (!canQuery) {
      setCorrelateError("missing base URL");
      return;
    }
    const tid = traceId.trim();
    if (!tid) {
      setCorrelateError("missing trace_id");
      return;
    }
    setCorrelateBusy(true);
    try {
      const res = await apiMemoryCorrelate(
        base,
        {
          traceId: tid,
          keyPrefix: correlatePrefix.trim() || undefined,
          structuredPath: correlateStructuredPath.trim() || undefined,
          maxEntries: parsePositiveInt(correlateMaxEntries, 50),
          timeline: correlateTimeline,
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "correlate failed");
      setCorrelateResult(res);
    } catch (e) {
      setCorrelateError(String(e));
    } finally {
      setCorrelateBusy(false);
    }
  };

  const runCheckpoints = async () => {
    setCheckpointError(null);
    setCheckpointResult(null);
    if (!canQuery) {
      setCheckpointError("missing base URL");
      return;
    }
    setCheckpointBusy(true);
    try {
      const res = await apiMemoryCheckpoints(
        base,
        {
          structuredPath: checkpointStructuredPath.trim() || undefined,
          limit: parsePositiveInt(checkpointLimit, 20),
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "checkpoint query failed");
      setCheckpointResult(res);
    } catch (e) {
      setCheckpointError(String(e));
    } finally {
      setCheckpointBusy(false);
    }
  };

  const runIndex = async () => {
    setIndexError(null);
    setIndexResult(null);
    if (!canQuery) {
      setIndexError("missing base URL");
      return;
    }
    setIndexBusy(true);
    try {
      const res = await apiMemoryIndex(
        base,
        {
          sessionId: indexSessionId.trim() || undefined,
          includeStructured: indexIncludeStructured,
          includeCore: indexIncludeCore,
          includeDaily: indexIncludeDaily,
          includeSession: indexIncludeSession,
          dailyDays: parsePositiveInt(indexDailyDays, 2),
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "index failed");
      setIndexResult(res);
    } catch (e) {
      setIndexError(String(e));
    } finally {
      setIndexBusy(false);
    }
  };

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Memory explorer</div>
          <div className="text-[11px] text-white/50">Query structured memory + correlate by trace_id + index files</div>
        </div>
      </summary>

      <div className="mt-3 grid gap-4">
        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Structured memory query</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={queryBusy || !canQuery}
                onClick={() => void runQuery()}
              >
                {queryBusy ? "Loading…" : "Query"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={queryBusy}
                onClick={() => {
                  setQueryError(null);
                  setQueryResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Key prefix</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={queryPrefix}
                  onChange={(e) => setQueryPrefix(e.target.value)}
                  placeholder="e.g. ui."
                />
              </div>
              <div>
                <FieldLabel>Limit</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={queryLimit}
                  onChange={(e) => setQueryLimit(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div>
              <FieldLabel>Structured path (optional)</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                value={queryStructuredPath}
                onChange={(e) => setQueryStructuredPath(e.target.value)}
                placeholder="STRUCTURED.md"
              />
            </div>
          </div>
          {queryError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {queryError}
            </div>
          ) : null}
          {queryResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(queryResult)}
            </pre>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Trace correlation</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={correlateBusy || !canQuery}
                onClick={() => void runCorrelate()}
              >
                {correlateBusy ? "Loading…" : "Correlate"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={correlateBusy}
                onClick={() => {
                  setCorrelateError(null);
                  setCorrelateResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Trace ID</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={traceId}
                  onChange={(e) => setTraceId(e.target.value)}
                  placeholder="trace_..."
                />
              </div>
              <div>
                <FieldLabel>Max entries</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={correlateMaxEntries}
                  onChange={(e) => setCorrelateMaxEntries(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Key prefix (optional)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={correlatePrefix}
                  onChange={(e) => setCorrelatePrefix(e.target.value)}
                />
              </div>
              <div>
                <FieldLabel>Structured path (optional)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={correlateStructuredPath}
                  onChange={(e) => setCorrelateStructuredPath(e.target.value)}
                  placeholder="STRUCTURED.md"
                />
              </div>
            </div>
            <label className="flex items-center gap-2 text-[11px] text-white/60">
              <input type="checkbox" checked={correlateTimeline} onChange={(e) => setCorrelateTimeline(e.target.checked)} />
              <span>Include checkpoint timeline</span>
            </label>
          </div>
          {correlateError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {correlateError}
            </div>
          ) : null}
          {correlateResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(correlateResult)}
            </pre>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Checkpoints</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={checkpointBusy || !canQuery}
                onClick={() => void runCheckpoints()}
              >
                {checkpointBusy ? "Loading…" : "List"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={checkpointBusy}
                onClick={() => {
                  setCheckpointError(null);
                  setCheckpointResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Limit</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={checkpointLimit}
                  onChange={(e) => setCheckpointLimit(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Structured path (optional)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={checkpointStructuredPath}
                  onChange={(e) => setCheckpointStructuredPath(e.target.value)}
                  placeholder="STRUCTURED.md"
                />
              </div>
            </div>
          </div>
          {checkpointError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {checkpointError}
            </div>
          ) : null}
          {checkpointResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(checkpointResult)}
            </pre>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Memory index</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={indexBusy || !canQuery}
                onClick={() => void runIndex()}
              >
                {indexBusy ? "Loading…" : "Index"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={indexBusy}
                onClick={() => {
                  setIndexError(null);
                  setIndexResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Session ID (optional)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={indexSessionId}
                  onChange={(e) => setIndexSessionId(e.target.value)}
                  placeholder="session_..."
                />
              </div>
              <div>
                <FieldLabel>Daily days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={indexDailyDays}
                  onChange={(e) => setIndexDailyDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={indexIncludeStructured} onChange={(e) => setIndexIncludeStructured(e.target.checked)} />
                <span>Include structured</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={indexIncludeCore} onChange={(e) => setIndexIncludeCore(e.target.checked)} />
                <span>Include core</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={indexIncludeDaily} onChange={(e) => setIndexIncludeDaily(e.target.checked)} />
                <span>Include daily</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={indexIncludeSession} onChange={(e) => setIndexIncludeSession(e.target.checked)} />
                <span>Include session</span>
              </label>
            </div>
          </div>
          {indexError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {indexError}
            </div>
          ) : null}
          {indexResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(indexResult)}
            </pre>
          ) : null}
        </section>
      </div>
    </details>
  );
}
