import React from "react";
import {
  apiMemoryCheckpoints,
  apiMemoryCorrelate,
  apiMemoryIndex,
  apiMemoryQuery,
  apiMemorySalience,
  apiMemoryRecapsCreate,
  apiMemoryRecapsList,
  apiMemoryRetentionEnforce,
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

  const [salienceIncludeStructured, setSalienceIncludeStructured] = React.useState<boolean>(true);
  const [salienceIncludeDaily, setSalienceIncludeDaily] = React.useState<boolean>(true);
  const [salienceDailyDays, setSalienceDailyDays] = React.useState<string>("7");
  const [salienceMaxItems, setSalienceMaxItems] = React.useState<string>("12");
  const [salienceStructuredMaxItems, setSalienceStructuredMaxItems] = React.useState<string>("6");
  const [salienceDailyMaxItems, setSalienceDailyMaxItems] = React.useState<string>("6");
  const [salienceHalfLifeDays, setSalienceHalfLifeDays] = React.useState<string>("14");
  const [salienceImportanceWeight, setSalienceImportanceWeight] = React.useState<string>("0.35");
  const [salienceResult, setSalienceResult] = React.useState<any | null>(null);
  const [salienceError, setSalienceError] = React.useState<string | null>(null);
  const [salienceBusy, setSalienceBusy] = React.useState<boolean>(false);

  const [recapsLimit, setRecapsLimit] = React.useState<string>("20");
  const [recapsIncludeSummary, setRecapsIncludeSummary] = React.useState<boolean>(false);
  const [recapsDryRun, setRecapsDryRun] = React.useState<boolean>(true);
  const [recapsWriteFile, setRecapsWriteFile] = React.useState<boolean>(true);
  const [recapsModel, setRecapsModel] = React.useState<string>("");
  const [recapsSummaryMaxChars, setRecapsSummaryMaxChars] = React.useState<string>("1200");
  const [recapsIncludeStructured, setRecapsIncludeStructured] = React.useState<boolean>(true);
  const [recapsIncludeDaily, setRecapsIncludeDaily] = React.useState<boolean>(true);
  const [recapsDailyDays, setRecapsDailyDays] = React.useState<string>("7");
  const [recapsMaxItems, setRecapsMaxItems] = React.useState<string>("12");
  const [recapsStructuredMaxItems, setRecapsStructuredMaxItems] = React.useState<string>("6");
  const [recapsDailyMaxItems, setRecapsDailyMaxItems] = React.useState<string>("6");
  const [recapsHalfLifeDays, setRecapsHalfLifeDays] = React.useState<string>("14");
  const [recapsImportanceWeight, setRecapsImportanceWeight] = React.useState<string>("0.35");
  const [recapsResult, setRecapsResult] = React.useState<any | null>(null);
  const [recapsError, setRecapsError] = React.useState<string | null>(null);
  const [recapsListBusy, setRecapsListBusy] = React.useState<boolean>(false);
  const [recapsGenerateBusy, setRecapsGenerateBusy] = React.useState<boolean>(false);

  const [retentionDryRun, setRetentionDryRun] = React.useState<boolean>(true);
  const [retentionDailyMaxDays, setRetentionDailyMaxDays] = React.useState<string>("30");
  const [retentionDailyMaxBytes, setRetentionDailyMaxBytes] = React.useState<string>("0");
  const [retentionCheckpointMaxDays, setRetentionCheckpointMaxDays] = React.useState<string>("30");
  const [retentionCheckpointMaxCount, setRetentionCheckpointMaxCount] = React.useState<string>("200");
  const [retentionStructuredDeprecateDays, setRetentionStructuredDeprecateDays] = React.useState<string>("90");
  const [retentionStructuredDeprecateMaxEntries, setRetentionStructuredDeprecateMaxEntries] = React.useState<string>("50");
  const [retentionResult, setRetentionResult] = React.useState<any | null>(null);
  const [retentionError, setRetentionError] = React.useState<string | null>(null);
  const [retentionBusy, setRetentionBusy] = React.useState<boolean>(false);

  const parsePositiveInt = (raw: string, fallback: number) => {
    const n = Number.parseInt(String(raw || "").trim(), 10);
    if (!Number.isFinite(n) || n <= 0) return fallback;
    return n;
  };

  const parseOptionalInt = (raw: string, min = 0) => {
    const s = String(raw || "").trim();
    if (!s) return undefined;
    const n = Number.parseInt(s, 10);
    if (!Number.isFinite(n) || n < min) return undefined;
    return n;
  };

  const parseOptionalFloat = (raw: string, min = 0) => {
    const s = String(raw || "").trim();
    if (!s) return undefined;
    const n = Number.parseFloat(s);
    if (!Number.isFinite(n) || n < min) return undefined;
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

  const runSalience = async () => {
    setSalienceError(null);
    setSalienceResult(null);
    if (!canQuery) {
      setSalienceError("missing base URL");
      return;
    }
    const dailyDays = parseOptionalInt(salienceDailyDays, 0);
    const maxItems = parseOptionalInt(salienceMaxItems, 0);
    const maxStructured = parseOptionalInt(salienceStructuredMaxItems, 0);
    const maxDaily = parseOptionalInt(salienceDailyMaxItems, 0);
    const halfLife = parseOptionalFloat(salienceHalfLifeDays, 0);
    const importance = parseOptionalFloat(salienceImportanceWeight, 0);

    setSalienceBusy(true);
    try {
      const res = await apiMemorySalience(
        base,
        {
          includeStructured: salienceIncludeStructured,
          includeDaily: salienceIncludeDaily,
          dailyDays,
          maxItems,
          maxStructuredItems: maxStructured,
          maxDailyItems: maxDaily,
          halfLifeDays: halfLife,
          importanceWeight: importance,
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "salience failed");
      setSalienceResult(res);
    } catch (e) {
      setSalienceError(String(e));
    } finally {
      setSalienceBusy(false);
    }
  };

  const runRecapsList = async () => {
    setRecapsError(null);
    setRecapsResult(null);
    if (!canQuery) {
      setRecapsError("missing base URL");
      return;
    }
    setRecapsListBusy(true);
    try {
      const res = await apiMemoryRecapsList(
        base,
        {
          limit: parsePositiveInt(recapsLimit, 20),
          includeSummary: recapsIncludeSummary,
        },
        props.auth,
      );
      if (!res.ok) throw new Error(res.error || "recaps list failed");
      setRecapsResult(res);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsListBusy(false);
    }
  };

  const runRecapsGenerate = async () => {
    setRecapsError(null);
    setRecapsResult(null);
    if (!canQuery) {
      setRecapsError("missing base URL");
      return;
    }
    const payload: Record<string, any> = {
      dry_run: recapsDryRun,
      write_file: recapsWriteFile,
      include_structured: recapsIncludeStructured,
      include_daily: recapsIncludeDaily,
    };
    const model = recapsModel.trim();
    if (model) payload.model = model;
    const summaryMax = parseOptionalInt(recapsSummaryMaxChars, 0);
    if (summaryMax !== undefined) payload.summary_max_chars = summaryMax;
    const dailyDays = parseOptionalInt(recapsDailyDays, 0);
    if (dailyDays !== undefined) payload.daily_days = dailyDays;
    const maxItems = parseOptionalInt(recapsMaxItems, 0);
    if (maxItems !== undefined) payload.max_items = maxItems;
    const maxStructured = parseOptionalInt(recapsStructuredMaxItems, 0);
    if (maxStructured !== undefined) payload.max_structured_items = maxStructured;
    const maxDaily = parseOptionalInt(recapsDailyMaxItems, 0);
    if (maxDaily !== undefined) payload.max_daily_items = maxDaily;
    const halfLife = parseOptionalFloat(recapsHalfLifeDays, 0);
    if (halfLife !== undefined) payload.half_life_days = halfLife;
    const importance = parseOptionalFloat(recapsImportanceWeight, 0);
    if (importance !== undefined) payload.importance_weight = importance;

    setRecapsGenerateBusy(true);
    try {
      const res = await apiMemoryRecapsCreate(base, payload, props.auth);
      if (!res.ok) throw new Error(res.error || "recap generation failed");
      setRecapsResult(res);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsGenerateBusy(false);
    }
  };

  const runRetention = async () => {
    setRetentionError(null);
    setRetentionResult(null);
    if (!canQuery) {
      setRetentionError("missing base URL");
      return;
    }
    const payload: Record<string, any> = {
      dry_run: retentionDryRun,
    };
    const dailyDays = parseOptionalInt(retentionDailyMaxDays, 0);
    if (dailyDays !== undefined) payload.daily_max_days = dailyDays;
    const dailyBytes = parseOptionalInt(retentionDailyMaxBytes, 0);
    if (dailyBytes !== undefined) payload.daily_max_bytes = dailyBytes;
    const checkpointDays = parseOptionalInt(retentionCheckpointMaxDays, 0);
    if (checkpointDays !== undefined) payload.checkpoint_max_days = checkpointDays;
    const checkpointCount = parseOptionalInt(retentionCheckpointMaxCount, 0);
    if (checkpointCount !== undefined) payload.checkpoint_max_count = checkpointCount;
    const deprecateDays = parseOptionalInt(retentionStructuredDeprecateDays, 0);
    if (deprecateDays !== undefined) payload.structured_deprecate_days = deprecateDays;
    const deprecateMax = parseOptionalInt(retentionStructuredDeprecateMaxEntries, 0);
    if (deprecateMax !== undefined) payload.structured_deprecate_max_entries = deprecateMax;

    setRetentionBusy(true);
    try {
      const res = await apiMemoryRetentionEnforce(base, payload, props.auth);
      if (!res.ok) throw new Error(res.error || "retention enforce failed");
      setRetentionResult(res);
    } catch (e) {
      setRetentionError(String(e));
    } finally {
      setRetentionBusy(false);
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

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Memory salience</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={salienceBusy || !canQuery}
                onClick={() => void runSalience()}
              >
                {salienceBusy ? "Loading…" : "Fetch"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={salienceBusy}
                onClick={() => {
                  setSalienceError(null);
                  setSalienceResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Daily days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={salienceDailyDays}
                  onChange={(e) => setSalienceDailyDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Max items</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={salienceMaxItems}
                  onChange={(e) => setSalienceMaxItems(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Structured max items</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={salienceStructuredMaxItems}
                  onChange={(e) => setSalienceStructuredMaxItems(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Daily max items</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={salienceDailyMaxItems}
                  onChange={(e) => setSalienceDailyMaxItems(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Half-life days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={salienceHalfLifeDays}
                  onChange={(e) => setSalienceHalfLifeDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Importance weight</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={salienceImportanceWeight}
                  onChange={(e) => setSalienceImportanceWeight(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={salienceIncludeStructured} onChange={(e) => setSalienceIncludeStructured(e.target.checked)} />
                <span>Include structured</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={salienceIncludeDaily} onChange={(e) => setSalienceIncludeDaily(e.target.checked)} />
                <span>Include daily</span>
              </label>
            </div>
          </div>
          {salienceError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {salienceError}
            </div>
          ) : null}
          {salienceResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(salienceResult)}
            </pre>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Memory recaps</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={recapsListBusy || !canQuery}
                onClick={() => void runRecapsList()}
              >
                {recapsListBusy ? "Loading…" : "List"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={recapsGenerateBusy || !canQuery}
                onClick={() => void runRecapsGenerate()}
              >
                {recapsGenerateBusy ? "Generating…" : "Generate"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={recapsListBusy || recapsGenerateBusy}
                onClick={() => {
                  setRecapsError(null);
                  setRecapsResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>List limit</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsLimit}
                  onChange={(e) => setRecapsLimit(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Summary max chars</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsSummaryMaxChars}
                  onChange={(e) => setRecapsSummaryMaxChars(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Model override (optional)</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsModel}
                  onChange={(e) => setRecapsModel(e.target.value)}
                  placeholder="summary model"
                />
              </div>
              <div>
                <FieldLabel>Daily days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsDailyDays}
                  onChange={(e) => setRecapsDailyDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Max items</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsMaxItems}
                  onChange={(e) => setRecapsMaxItems(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Structured max items</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsStructuredMaxItems}
                  onChange={(e) => setRecapsStructuredMaxItems(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Daily max items</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsDailyMaxItems}
                  onChange={(e) => setRecapsDailyMaxItems(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Half-life days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsHalfLifeDays}
                  onChange={(e) => setRecapsHalfLifeDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Importance weight</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={recapsImportanceWeight}
                  onChange={(e) => setRecapsImportanceWeight(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <label className="flex items-center gap-2 text-[11px] text-white/60">
                <input type="checkbox" checked={recapsIncludeSummary} onChange={(e) => setRecapsIncludeSummary(e.target.checked)} />
                <span>Include summary in list</span>
              </label>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={recapsDryRun} onChange={(e) => setRecapsDryRun(e.target.checked)} />
                <span>Dry run</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={recapsWriteFile} onChange={(e) => setRecapsWriteFile(e.target.checked)} />
                <span>Write recap file</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={recapsIncludeStructured} onChange={(e) => setRecapsIncludeStructured(e.target.checked)} />
                <span>Include structured</span>
              </label>
              <label className="flex items-center gap-2">
                <input type="checkbox" checked={recapsIncludeDaily} onChange={(e) => setRecapsIncludeDaily(e.target.checked)} />
                <span>Include daily</span>
              </label>
            </div>
          </div>
          {recapsError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {recapsError}
            </div>
          ) : null}
          {recapsResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(recapsResult)}
            </pre>
          ) : null}
        </section>

        <section className="rounded-md border border-white/10 bg-black/20 p-3">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/80">Memory retention</div>
            <div className="flex items-center gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={retentionBusy || !canQuery}
                onClick={() => void runRetention()}
              >
                {retentionBusy ? "Running…" : "Enforce"}
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={retentionBusy}
                onClick={() => {
                  setRetentionError(null);
                  setRetentionResult(null);
                }}
              >
                Clear
              </button>
            </div>
          </div>
          <div className="grid gap-2 text-[11px] text-white/70">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Daily max days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={retentionDailyMaxDays}
                  onChange={(e) => setRetentionDailyMaxDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Daily max bytes</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={retentionDailyMaxBytes}
                  onChange={(e) => setRetentionDailyMaxBytes(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Checkpoint max days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={retentionCheckpointMaxDays}
                  onChange={(e) => setRetentionCheckpointMaxDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Checkpoint max count</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={retentionCheckpointMaxCount}
                  onChange={(e) => setRetentionCheckpointMaxCount(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <FieldLabel>Structured deprecate days</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={retentionStructuredDeprecateDays}
                  onChange={(e) => setRetentionStructuredDeprecateDays(e.target.value)}
                  inputMode="numeric"
                />
              </div>
              <div>
                <FieldLabel>Structured deprecate max entries</FieldLabel>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
                  value={retentionStructuredDeprecateMaxEntries}
                  onChange={(e) => setRetentionStructuredDeprecateMaxEntries(e.target.value)}
                  inputMode="numeric"
                />
              </div>
            </div>
            <label className="flex items-center gap-2">
              <input type="checkbox" checked={retentionDryRun} onChange={(e) => setRetentionDryRun(e.target.checked)} />
              <span>Dry run (preview only)</span>
            </label>
          </div>
          {retentionError ? (
            <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {retentionError}
            </div>
          ) : null}
          {retentionResult ? (
            <pre className="mt-2 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
              {stringifyJson(retentionResult)}
            </pre>
          ) : null}
        </section>
      </div>
    </details>
  );
}
