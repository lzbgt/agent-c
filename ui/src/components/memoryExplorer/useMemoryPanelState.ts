import React from "react";
import {
  apiGetConfig,
  apiMemoryCheckpoints,
  apiMemoryCorrelate,
  apiMemoryCorrelationIndexBuild,
  apiMemoryIndex,
  apiMemoryQuery,
  apiMemoryRecapsCreate,
  apiMemoryRecapsList,
  apiMemoryRetentionEnforce,
  apiMemorySalience,
  apiUpdateDaemonConfig,
  type ApiAuth,
} from "../../api";

export type MemoryPanelStateArgs = {
  baseUrl: string;
  auth: ApiAuth;
};

export const stringifyJson = (value: unknown) => {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value ?? "");
  }
};

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

export type MemoryQueryState = {
  prefix: string;
  setPrefix: React.Dispatch<React.SetStateAction<string>>;
  limit: string;
  setLimit: React.Dispatch<React.SetStateAction<string>>;
  structuredPath: string;
  setStructuredPath: React.Dispatch<React.SetStateAction<string>>;
  result: any | null;
  error: string | null;
  busy: boolean;
  run: () => Promise<void>;
  clear: () => void;
};

export type MemoryCorrelationState = {
  traceId: string;
  setTraceId: React.Dispatch<React.SetStateAction<string>>;
  prefix: string;
  setPrefix: React.Dispatch<React.SetStateAction<string>>;
  structuredPath: string;
  setStructuredPath: React.Dispatch<React.SetStateAction<string>>;
  maxEntries: string;
  setMaxEntries: React.Dispatch<React.SetStateAction<string>>;
  timeline: boolean;
  setTimeline: React.Dispatch<React.SetStateAction<boolean>>;
  result: any | null;
  error: string | null;
  busy: boolean;
  indexResult: any | null;
  indexError: string | null;
  indexBusy: boolean;
  run: () => Promise<void>;
  buildIndex: () => Promise<void>;
  clear: () => void;
};

export type MemoryCheckpointsState = {
  limit: string;
  setLimit: React.Dispatch<React.SetStateAction<string>>;
  structuredPath: string;
  setStructuredPath: React.Dispatch<React.SetStateAction<string>>;
  result: any | null;
  error: string | null;
  busy: boolean;
  run: () => Promise<void>;
  clear: () => void;
};

export type MemoryIndexState = {
  sessionId: string;
  setSessionId: React.Dispatch<React.SetStateAction<string>>;
  includeStructured: boolean;
  setIncludeStructured: React.Dispatch<React.SetStateAction<boolean>>;
  includeCore: boolean;
  setIncludeCore: React.Dispatch<React.SetStateAction<boolean>>;
  includeDaily: boolean;
  setIncludeDaily: React.Dispatch<React.SetStateAction<boolean>>;
  includeSession: boolean;
  setIncludeSession: React.Dispatch<React.SetStateAction<boolean>>;
  dailyDays: string;
  setDailyDays: React.Dispatch<React.SetStateAction<string>>;
  result: any | null;
  error: string | null;
  busy: boolean;
  run: () => Promise<void>;
  clear: () => void;
};

export type MemorySalienceState = {
  includeStructured: boolean;
  setIncludeStructured: React.Dispatch<React.SetStateAction<boolean>>;
  includeDaily: boolean;
  setIncludeDaily: React.Dispatch<React.SetStateAction<boolean>>;
  dailyDays: string;
  setDailyDays: React.Dispatch<React.SetStateAction<string>>;
  maxItems: string;
  setMaxItems: React.Dispatch<React.SetStateAction<string>>;
  structuredMaxItems: string;
  setStructuredMaxItems: React.Dispatch<React.SetStateAction<string>>;
  dailyMaxItems: string;
  setDailyMaxItems: React.Dispatch<React.SetStateAction<string>>;
  halfLifeDays: string;
  setHalfLifeDays: React.Dispatch<React.SetStateAction<string>>;
  importanceWeight: string;
  setImportanceWeight: React.Dispatch<React.SetStateAction<string>>;
  result: any | null;
  error: string | null;
  busy: boolean;
  run: () => Promise<void>;
  clear: () => void;
};

export type MemoryRecapsState = {
  limit: string;
  setLimit: React.Dispatch<React.SetStateAction<string>>;
  includeSummary: boolean;
  setIncludeSummary: React.Dispatch<React.SetStateAction<boolean>>;
  dryRun: boolean;
  setDryRun: React.Dispatch<React.SetStateAction<boolean>>;
  writeFile: boolean;
  setWriteFile: React.Dispatch<React.SetStateAction<boolean>>;
  kind: string;
  setKind: React.Dispatch<React.SetStateAction<string>>;
  kindFilter: string;
  setKindFilter: React.Dispatch<React.SetStateAction<string>>;
  model: string;
  setModel: React.Dispatch<React.SetStateAction<string>>;
  summaryMaxChars: string;
  setSummaryMaxChars: React.Dispatch<React.SetStateAction<string>>;
  includeStructured: boolean;
  setIncludeStructured: React.Dispatch<React.SetStateAction<boolean>>;
  includeDaily: boolean;
  setIncludeDaily: React.Dispatch<React.SetStateAction<boolean>>;
  dailyDays: string;
  setDailyDays: React.Dispatch<React.SetStateAction<string>>;
  maxItems: string;
  setMaxItems: React.Dispatch<React.SetStateAction<string>>;
  structuredMaxItems: string;
  setStructuredMaxItems: React.Dispatch<React.SetStateAction<string>>;
  dailyMaxItems: string;
  setDailyMaxItems: React.Dispatch<React.SetStateAction<string>>;
  halfLifeDays: string;
  setHalfLifeDays: React.Dispatch<React.SetStateAction<string>>;
  importanceWeight: string;
  setImportanceWeight: React.Dispatch<React.SetStateAction<string>>;
  result: any | null;
  error: string | null;
  listBusy: boolean;
  generateBusy: boolean;
  list: any[];
  filteredList: any[];
  runList: () => Promise<void>;
  runGenerate: () => Promise<void>;
  clear: () => void;
  schedule: {
    dailyIntervalMs: string;
    setDailyIntervalMs: React.Dispatch<React.SetStateAction<string>>;
    weeklyIntervalMs: string;
    setWeeklyIntervalMs: React.Dispatch<React.SetStateAction<string>>;
    dailyDays: string;
    setDailyDays: React.Dispatch<React.SetStateAction<string>>;
    weeklyDays: string;
    setWeeklyDays: React.Dispatch<React.SetStateAction<string>>;
    summaryModel: string;
    busy: boolean;
    error: string | null;
    result: any | null;
    load: () => Promise<void>;
    apply: () => Promise<void>;
  };
};

export type MemoryRetentionState = {
  dryRun: boolean;
  setDryRun: React.Dispatch<React.SetStateAction<boolean>>;
  dailyMaxDays: string;
  setDailyMaxDays: React.Dispatch<React.SetStateAction<string>>;
  dailyMaxBytes: string;
  setDailyMaxBytes: React.Dispatch<React.SetStateAction<string>>;
  checkpointMaxDays: string;
  setCheckpointMaxDays: React.Dispatch<React.SetStateAction<string>>;
  checkpointMaxCount: string;
  setCheckpointMaxCount: React.Dispatch<React.SetStateAction<string>>;
  structuredDeprecateDays: string;
  setStructuredDeprecateDays: React.Dispatch<React.SetStateAction<string>>;
  structuredDeprecateMaxEntries: string;
  setStructuredDeprecateMaxEntries: React.Dispatch<React.SetStateAction<string>>;
  result: any | null;
  error: string | null;
  busy: boolean;
  run: () => Promise<void>;
  clear: () => void;
};

export type MemoryPanelState = {
  canQuery: boolean;
  stringifyJson: typeof stringifyJson;
  query: MemoryQueryState;
  correlation: MemoryCorrelationState;
  checkpoints: MemoryCheckpointsState;
  index: MemoryIndexState;
  salience: MemorySalienceState;
  recaps: MemoryRecapsState;
  retention: MemoryRetentionState;
};

export function useMemoryPanelState(args: MemoryPanelStateArgs): MemoryPanelState {
  const base = String(args.baseUrl || "").trim().replace(/\/+$/, "");
  const canQuery = base.length > 0;

  const [queryPrefix, setQueryPrefix] = React.useState("");
  const [queryLimit, setQueryLimit] = React.useState("50");
  const [queryStructuredPath, setQueryStructuredPath] = React.useState("");
  const [queryResult, setQueryResult] = React.useState<any | null>(null);
  const [queryError, setQueryError] = React.useState<string | null>(null);
  const [queryBusy, setQueryBusy] = React.useState(false);

  const [traceId, setTraceId] = React.useState("");
  const [correlatePrefix, setCorrelatePrefix] = React.useState("");
  const [correlateStructuredPath, setCorrelateStructuredPath] = React.useState("");
  const [correlateMaxEntries, setCorrelateMaxEntries] = React.useState("50");
  const [correlateTimeline, setCorrelateTimeline] = React.useState(false);
  const [correlateResult, setCorrelateResult] = React.useState<any | null>(null);
  const [correlateError, setCorrelateError] = React.useState<string | null>(null);
  const [correlateBusy, setCorrelateBusy] = React.useState(false);
  const [correlationIndexResult, setCorrelationIndexResult] = React.useState<any | null>(null);
  const [correlationIndexError, setCorrelationIndexError] = React.useState<string | null>(null);
  const [correlationIndexBusy, setCorrelationIndexBusy] = React.useState(false);

  const [checkpointLimit, setCheckpointLimit] = React.useState("20");
  const [checkpointStructuredPath, setCheckpointStructuredPath] = React.useState("");
  const [checkpointResult, setCheckpointResult] = React.useState<any | null>(null);
  const [checkpointError, setCheckpointError] = React.useState<string | null>(null);
  const [checkpointBusy, setCheckpointBusy] = React.useState(false);

  const [indexSessionId, setIndexSessionId] = React.useState("");
  const [indexIncludeStructured, setIndexIncludeStructured] = React.useState(true);
  const [indexIncludeCore, setIndexIncludeCore] = React.useState(true);
  const [indexIncludeDaily, setIndexIncludeDaily] = React.useState(true);
  const [indexIncludeSession, setIndexIncludeSession] = React.useState(false);
  const [indexDailyDays, setIndexDailyDays] = React.useState("2");
  const [indexResult, setIndexResult] = React.useState<any | null>(null);
  const [indexError, setIndexError] = React.useState<string | null>(null);
  const [indexBusy, setIndexBusy] = React.useState(false);

  const [salienceIncludeStructured, setSalienceIncludeStructured] = React.useState(true);
  const [salienceIncludeDaily, setSalienceIncludeDaily] = React.useState(true);
  const [salienceDailyDays, setSalienceDailyDays] = React.useState("7");
  const [salienceMaxItems, setSalienceMaxItems] = React.useState("12");
  const [salienceStructuredMaxItems, setSalienceStructuredMaxItems] = React.useState("6");
  const [salienceDailyMaxItems, setSalienceDailyMaxItems] = React.useState("6");
  const [salienceHalfLifeDays, setSalienceHalfLifeDays] = React.useState("14");
  const [salienceImportanceWeight, setSalienceImportanceWeight] = React.useState("0.35");
  const [salienceResult, setSalienceResult] = React.useState<any | null>(null);
  const [salienceError, setSalienceError] = React.useState<string | null>(null);
  const [salienceBusy, setSalienceBusy] = React.useState(false);

  const [recapsLimit, setRecapsLimit] = React.useState("20");
  const [recapsIncludeSummary, setRecapsIncludeSummary] = React.useState(false);
  const [recapsDryRun, setRecapsDryRun] = React.useState(true);
  const [recapsWriteFile, setRecapsWriteFile] = React.useState(true);
  const [recapsKind, setRecapsKind] = React.useState("");
  const [recapsKindFilter, setRecapsKindFilter] = React.useState("");
  const [recapsModel, setRecapsModel] = React.useState("");
  const [recapsSummaryMaxChars, setRecapsSummaryMaxChars] = React.useState("1200");
  const [recapsIncludeStructured, setRecapsIncludeStructured] = React.useState(true);
  const [recapsIncludeDaily, setRecapsIncludeDaily] = React.useState(true);
  const [recapsDailyDays, setRecapsDailyDays] = React.useState("7");
  const [recapsMaxItems, setRecapsMaxItems] = React.useState("12");
  const [recapsStructuredMaxItems, setRecapsStructuredMaxItems] = React.useState("6");
  const [recapsDailyMaxItems, setRecapsDailyMaxItems] = React.useState("6");
  const [recapsHalfLifeDays, setRecapsHalfLifeDays] = React.useState("14");
  const [recapsImportanceWeight, setRecapsImportanceWeight] = React.useState("0.35");
  const [recapsResult, setRecapsResult] = React.useState<any | null>(null);
  const [recapsError, setRecapsError] = React.useState<string | null>(null);
  const [recapsListBusy, setRecapsListBusy] = React.useState(false);
  const [recapsGenerateBusy, setRecapsGenerateBusy] = React.useState(false);

  const [recapScheduleDailyIntervalMs, setRecapScheduleDailyIntervalMs] = React.useState("0");
  const [recapScheduleWeeklyIntervalMs, setRecapScheduleWeeklyIntervalMs] = React.useState("0");
  const [recapScheduleDailyDays, setRecapScheduleDailyDays] = React.useState("1");
  const [recapScheduleWeeklyDays, setRecapScheduleWeeklyDays] = React.useState("7");
  const [recapScheduleBusy, setRecapScheduleBusy] = React.useState(false);
  const [recapScheduleError, setRecapScheduleError] = React.useState<string | null>(null);
  const [recapScheduleResult, setRecapScheduleResult] = React.useState<any | null>(null);
  const [recapSummaryModel, setRecapSummaryModel] = React.useState("");

  const [retentionDryRun, setRetentionDryRun] = React.useState(true);
  const [retentionDailyMaxDays, setRetentionDailyMaxDays] = React.useState("30");
  const [retentionDailyMaxBytes, setRetentionDailyMaxBytes] = React.useState("0");
  const [retentionCheckpointMaxDays, setRetentionCheckpointMaxDays] = React.useState("30");
  const [retentionCheckpointMaxCount, setRetentionCheckpointMaxCount] = React.useState("200");
  const [retentionStructuredDeprecateDays, setRetentionStructuredDeprecateDays] = React.useState("90");
  const [retentionStructuredDeprecateMaxEntries, setRetentionStructuredDeprecateMaxEntries] = React.useState("50");
  const [retentionResult, setRetentionResult] = React.useState<any | null>(null);
  const [retentionError, setRetentionError] = React.useState<string | null>(null);
  const [retentionBusy, setRetentionBusy] = React.useState(false);

  const runQuery = React.useCallback(async () => {
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
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "query failed");
      setQueryResult(res);
    } catch (e) {
      setQueryError(String(e));
    } finally {
      setQueryBusy(false);
    }
  }, [args.auth, base, canQuery, queryLimit, queryPrefix, queryStructuredPath]);

  const runCorrelate = React.useCallback(async () => {
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
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "correlate failed");
      setCorrelateResult(res);
    } catch (e) {
      setCorrelateError(String(e));
    } finally {
      setCorrelateBusy(false);
    }
  }, [
    args.auth,
    base,
    canQuery,
    correlateMaxEntries,
    correlatePrefix,
    correlateStructuredPath,
    correlateTimeline,
    traceId,
  ]);

  const runCorrelationIndexBuild = React.useCallback(async () => {
    setCorrelationIndexError(null);
    setCorrelationIndexResult(null);
    if (!canQuery) {
      setCorrelationIndexError("missing base URL");
      return;
    }
    setCorrelationIndexBusy(true);
    try {
      const res = await apiMemoryCorrelationIndexBuild(base, {}, args.auth);
      if (!res.ok) throw new Error(res.error || "correlation index build failed");
      setCorrelationIndexResult(res);
    } catch (e) {
      setCorrelationIndexError(String(e));
    } finally {
      setCorrelationIndexBusy(false);
    }
  }, [args.auth, base, canQuery]);

  const runCheckpoints = React.useCallback(async () => {
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
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "checkpoint query failed");
      setCheckpointResult(res);
    } catch (e) {
      setCheckpointError(String(e));
    } finally {
      setCheckpointBusy(false);
    }
  }, [args.auth, base, canQuery, checkpointLimit, checkpointStructuredPath]);

  const runIndex = React.useCallback(async () => {
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
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "index failed");
      setIndexResult(res);
    } catch (e) {
      setIndexError(String(e));
    } finally {
      setIndexBusy(false);
    }
  }, [
    args.auth,
    base,
    canQuery,
    indexDailyDays,
    indexIncludeCore,
    indexIncludeDaily,
    indexIncludeSession,
    indexIncludeStructured,
    indexSessionId,
  ]);

  const runSalience = React.useCallback(async () => {
    setSalienceError(null);
    setSalienceResult(null);
    if (!canQuery) {
      setSalienceError("missing base URL");
      return;
    }
    setSalienceBusy(true);
    try {
      const res = await apiMemorySalience(
        base,
        {
          includeStructured: salienceIncludeStructured,
          includeDaily: salienceIncludeDaily,
          dailyDays: parseOptionalInt(salienceDailyDays, 0),
          maxItems: parseOptionalInt(salienceMaxItems, 0),
          maxStructuredItems: parseOptionalInt(salienceStructuredMaxItems, 0),
          maxDailyItems: parseOptionalInt(salienceDailyMaxItems, 0),
          halfLifeDays: parseOptionalFloat(salienceHalfLifeDays, 0),
          importanceWeight: parseOptionalFloat(salienceImportanceWeight, 0),
        },
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "salience failed");
      setSalienceResult(res);
    } catch (e) {
      setSalienceError(String(e));
    } finally {
      setSalienceBusy(false);
    }
  }, [
    args.auth,
    base,
    canQuery,
    salienceDailyDays,
    salienceDailyMaxItems,
    salienceHalfLifeDays,
    salienceImportanceWeight,
    salienceIncludeDaily,
    salienceIncludeStructured,
    salienceMaxItems,
    salienceStructuredMaxItems,
  ]);

  const runRecapsList = React.useCallback(async () => {
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
          kind: recapsKindFilter.trim() || undefined,
        },
        args.auth,
      );
      if (!res.ok) throw new Error(res.error || "recaps list failed");
      setRecapsResult(res);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsListBusy(false);
    }
  }, [args.auth, base, canQuery, recapsIncludeSummary, recapsKindFilter, recapsLimit]);

  const runRecapsGenerate = React.useCallback(async () => {
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
    const kind = recapsKind.trim();
    if (kind) payload.kind = kind;
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
      const res = await apiMemoryRecapsCreate(base, payload, args.auth);
      if (!res.ok) throw new Error(res.error || "recap generation failed");
      setRecapsResult(res);
    } catch (e) {
      setRecapsError(String(e));
    } finally {
      setRecapsGenerateBusy(false);
    }
  }, [
    args.auth,
    base,
    canQuery,
    recapsDailyDays,
    recapsDailyMaxItems,
    recapsDryRun,
    recapsHalfLifeDays,
    recapsImportanceWeight,
    recapsIncludeDaily,
    recapsIncludeStructured,
    recapsKind,
    recapsMaxItems,
    recapsModel,
    recapsStructuredMaxItems,
    recapsSummaryMaxChars,
    recapsWriteFile,
  ]);

  const loadRecapSchedule = React.useCallback(async () => {
    setRecapScheduleError(null);
    setRecapScheduleResult(null);
    if (!canQuery) {
      setRecapScheduleError("missing base URL");
      return;
    }
    setRecapScheduleBusy(true);
    try {
      const res = await apiGetConfig(base, args.auth);
      if (!res.ok) throw new Error(res.error || res.err || "config load failed");
      const mem = (res as any).memory || {};
      const daemon = (res as any).daemon || {};
      setRecapScheduleDailyIntervalMs(String(mem.recap_daily_interval_ms ?? "0"));
      setRecapScheduleWeeklyIntervalMs(String(mem.recap_weekly_interval_ms ?? "0"));
      setRecapScheduleDailyDays(String(mem.recap_daily_days ?? "1"));
      setRecapScheduleWeeklyDays(String(mem.recap_weekly_days ?? "7"));
      setRecapSummaryModel(String(daemon.summary_model || ""));
    } catch (e) {
      setRecapScheduleError(String(e));
    } finally {
      setRecapScheduleBusy(false);
    }
  }, [args.auth, base, canQuery]);

  const applyRecapSchedule = React.useCallback(async () => {
    setRecapScheduleError(null);
    setRecapScheduleResult(null);
    if (!canQuery) {
      setRecapScheduleError("missing base URL");
      return;
    }
    const memory: Record<string, any> = {};
    const dailyInterval = parseOptionalInt(recapScheduleDailyIntervalMs, 0);
    if (dailyInterval !== undefined) memory.recap_daily_interval_ms = dailyInterval;
    const weeklyInterval = parseOptionalInt(recapScheduleWeeklyIntervalMs, 0);
    if (weeklyInterval !== undefined) memory.recap_weekly_interval_ms = weeklyInterval;
    const dailyDays = parseOptionalInt(recapScheduleDailyDays, 0);
    if (dailyDays !== undefined) memory.recap_daily_days = dailyDays;
    const weeklyDays = parseOptionalInt(recapScheduleWeeklyDays, 0);
    if (weeklyDays !== undefined) memory.recap_weekly_days = weeklyDays;
    if (Object.keys(memory).length === 0) {
      setRecapScheduleError("no schedule fields provided");
      return;
    }
    setRecapScheduleBusy(true);
    try {
      const res = await apiUpdateDaemonConfig(base, { memory }, args.auth);
      if (!res.ok) throw new Error(res.error || res.err || "schedule update failed");
      setRecapScheduleResult(res);
    } catch (e) {
      setRecapScheduleError(String(e));
    } finally {
      setRecapScheduleBusy(false);
    }
  }, [
    args.auth,
    base,
    canQuery,
    recapScheduleDailyDays,
    recapScheduleDailyIntervalMs,
    recapScheduleWeeklyDays,
    recapScheduleWeeklyIntervalMs,
  ]);

  const runRetention = React.useCallback(async () => {
    setRetentionError(null);
    setRetentionResult(null);
    if (!canQuery) {
      setRetentionError("missing base URL");
      return;
    }
    const payload: Record<string, any> = { dry_run: retentionDryRun };
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
      const res = await apiMemoryRetentionEnforce(base, payload, args.auth);
      if (!res.ok) throw new Error(res.error || "retention enforce failed");
      setRetentionResult(res);
    } catch (e) {
      setRetentionError(String(e));
    } finally {
      setRetentionBusy(false);
    }
  }, [
    args.auth,
    base,
    canQuery,
    retentionCheckpointMaxCount,
    retentionCheckpointMaxDays,
    retentionDailyMaxBytes,
    retentionDailyMaxDays,
    retentionDryRun,
    retentionStructuredDeprecateDays,
    retentionStructuredDeprecateMaxEntries,
  ]);

  const recapsList = React.useMemo(
    () => (Array.isArray((recapsResult as any)?.recaps) ? (recapsResult as any).recaps : []),
    [recapsResult],
  );
  const recapsKindFilterValue = recapsKindFilter.trim().toLowerCase();
  const recapsHaveKind = React.useMemo(
    () => recapsList.some((item: any) => String(item?.kind || "").trim().length > 0),
    [recapsList],
  );
  const recapsFiltered = React.useMemo(() => {
    if (!recapsKindFilterValue || !recapsHaveKind) return recapsList;
    return recapsList.filter((item: any) => String(item?.kind || "").toLowerCase() === recapsKindFilterValue);
  }, [recapsHaveKind, recapsKindFilterValue, recapsList]);

  return {
    canQuery,
    stringifyJson,
    query: {
      prefix: queryPrefix,
      setPrefix: setQueryPrefix,
      limit: queryLimit,
      setLimit: setQueryLimit,
      structuredPath: queryStructuredPath,
      setStructuredPath: setQueryStructuredPath,
      result: queryResult,
      error: queryError,
      busy: queryBusy,
      run: runQuery,
      clear: () => {
        setQueryError(null);
        setQueryResult(null);
      },
    },
    correlation: {
      traceId,
      setTraceId,
      prefix: correlatePrefix,
      setPrefix: setCorrelatePrefix,
      structuredPath: correlateStructuredPath,
      setStructuredPath: setCorrelateStructuredPath,
      maxEntries: correlateMaxEntries,
      setMaxEntries: setCorrelateMaxEntries,
      timeline: correlateTimeline,
      setTimeline: setCorrelateTimeline,
      result: correlateResult,
      error: correlateError,
      busy: correlateBusy,
      indexResult: correlationIndexResult,
      indexError: correlationIndexError,
      indexBusy: correlationIndexBusy,
      run: runCorrelate,
      buildIndex: runCorrelationIndexBuild,
      clear: () => {
        setCorrelateError(null);
        setCorrelateResult(null);
        setCorrelationIndexError(null);
        setCorrelationIndexResult(null);
      },
    },
    checkpoints: {
      limit: checkpointLimit,
      setLimit: setCheckpointLimit,
      structuredPath: checkpointStructuredPath,
      setStructuredPath: setCheckpointStructuredPath,
      result: checkpointResult,
      error: checkpointError,
      busy: checkpointBusy,
      run: runCheckpoints,
      clear: () => {
        setCheckpointError(null);
        setCheckpointResult(null);
      },
    },
    index: {
      sessionId: indexSessionId,
      setSessionId: setIndexSessionId,
      includeStructured: indexIncludeStructured,
      setIncludeStructured: setIndexIncludeStructured,
      includeCore: indexIncludeCore,
      setIncludeCore: setIndexIncludeCore,
      includeDaily: indexIncludeDaily,
      setIncludeDaily: setIndexIncludeDaily,
      includeSession: indexIncludeSession,
      setIncludeSession: setIndexIncludeSession,
      dailyDays: indexDailyDays,
      setDailyDays: setIndexDailyDays,
      result: indexResult,
      error: indexError,
      busy: indexBusy,
      run: runIndex,
      clear: () => {
        setIndexError(null);
        setIndexResult(null);
      },
    },
    salience: {
      includeStructured: salienceIncludeStructured,
      setIncludeStructured: setSalienceIncludeStructured,
      includeDaily: salienceIncludeDaily,
      setIncludeDaily: setSalienceIncludeDaily,
      dailyDays: salienceDailyDays,
      setDailyDays: setSalienceDailyDays,
      maxItems: salienceMaxItems,
      setMaxItems: setSalienceMaxItems,
      structuredMaxItems: salienceStructuredMaxItems,
      setStructuredMaxItems: setSalienceStructuredMaxItems,
      dailyMaxItems: salienceDailyMaxItems,
      setDailyMaxItems: setSalienceDailyMaxItems,
      halfLifeDays: salienceHalfLifeDays,
      setHalfLifeDays: setSalienceHalfLifeDays,
      importanceWeight: salienceImportanceWeight,
      setImportanceWeight: setSalienceImportanceWeight,
      result: salienceResult,
      error: salienceError,
      busy: salienceBusy,
      run: runSalience,
      clear: () => {
        setSalienceError(null);
        setSalienceResult(null);
      },
    },
    recaps: {
      limit: recapsLimit,
      setLimit: setRecapsLimit,
      includeSummary: recapsIncludeSummary,
      setIncludeSummary: setRecapsIncludeSummary,
      dryRun: recapsDryRun,
      setDryRun: setRecapsDryRun,
      writeFile: recapsWriteFile,
      setWriteFile: setRecapsWriteFile,
      kind: recapsKind,
      setKind: setRecapsKind,
      kindFilter: recapsKindFilter,
      setKindFilter: setRecapsKindFilter,
      model: recapsModel,
      setModel: setRecapsModel,
      summaryMaxChars: recapsSummaryMaxChars,
      setSummaryMaxChars: setRecapsSummaryMaxChars,
      includeStructured: recapsIncludeStructured,
      setIncludeStructured: setRecapsIncludeStructured,
      includeDaily: recapsIncludeDaily,
      setIncludeDaily: setRecapsIncludeDaily,
      dailyDays: recapsDailyDays,
      setDailyDays: setRecapsDailyDays,
      maxItems: recapsMaxItems,
      setMaxItems: setRecapsMaxItems,
      structuredMaxItems: recapsStructuredMaxItems,
      setStructuredMaxItems: setRecapsStructuredMaxItems,
      dailyMaxItems: recapsDailyMaxItems,
      setDailyMaxItems: setRecapsDailyMaxItems,
      halfLifeDays: recapsHalfLifeDays,
      setHalfLifeDays: setRecapsHalfLifeDays,
      importanceWeight: recapsImportanceWeight,
      setImportanceWeight: setRecapsImportanceWeight,
      result: recapsResult,
      error: recapsError,
      listBusy: recapsListBusy,
      generateBusy: recapsGenerateBusy,
      list: recapsList,
      filteredList: recapsFiltered,
      runList: runRecapsList,
      runGenerate: runRecapsGenerate,
      clear: () => {
        setRecapsError(null);
        setRecapsResult(null);
      },
      schedule: {
        dailyIntervalMs: recapScheduleDailyIntervalMs,
        setDailyIntervalMs: setRecapScheduleDailyIntervalMs,
        weeklyIntervalMs: recapScheduleWeeklyIntervalMs,
        setWeeklyIntervalMs: setRecapScheduleWeeklyIntervalMs,
        dailyDays: recapScheduleDailyDays,
        setDailyDays: setRecapScheduleDailyDays,
        weeklyDays: recapScheduleWeeklyDays,
        setWeeklyDays: setRecapScheduleWeeklyDays,
        summaryModel: recapSummaryModel,
        busy: recapScheduleBusy,
        error: recapScheduleError,
        result: recapScheduleResult,
        load: loadRecapSchedule,
        apply: applyRecapSchedule,
      },
    },
    retention: {
      dryRun: retentionDryRun,
      setDryRun: setRetentionDryRun,
      dailyMaxDays: retentionDailyMaxDays,
      setDailyMaxDays: setRetentionDailyMaxDays,
      dailyMaxBytes: retentionDailyMaxBytes,
      setDailyMaxBytes: setRetentionDailyMaxBytes,
      checkpointMaxDays: retentionCheckpointMaxDays,
      setCheckpointMaxDays: setRetentionCheckpointMaxDays,
      checkpointMaxCount: retentionCheckpointMaxCount,
      setCheckpointMaxCount: setRetentionCheckpointMaxCount,
      structuredDeprecateDays: retentionStructuredDeprecateDays,
      setStructuredDeprecateDays: setRetentionStructuredDeprecateDays,
      structuredDeprecateMaxEntries: retentionStructuredDeprecateMaxEntries,
      setStructuredDeprecateMaxEntries: setRetentionStructuredDeprecateMaxEntries,
      result: retentionResult,
      error: retentionError,
      busy: retentionBusy,
      run: runRetention,
      clear: () => {
        setRetentionError(null);
        setRetentionResult(null);
      },
    },
  };
}
