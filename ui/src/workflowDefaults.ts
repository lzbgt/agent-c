import type { RunSettings } from "./hooks/useUiSettings";

const parseNumber = (raw: string, min = 0): number | undefined => {
  const s = String(raw ?? "").trim();
  if (!s) return undefined;
  const n = Number(s);
  if (!Number.isFinite(n) || n < min) return undefined;
  return n;
};

const parseToolCallLimits = (raw: string): { tool: string; max_calls: number }[] | undefined => {
  const s = String(raw ?? "").trim();
  if (!s) return undefined;
  if (s.startsWith("[") || s.startsWith("{")) {
    try {
      const v: any = JSON.parse(s);
      const arr = Array.isArray(v) ? v : [v];
      const parsed = arr
        .map((item) => {
          const tool = String(item?.tool ?? item?.name ?? "").trim();
          const max_calls = Number(item?.max_calls ?? item?.maxCalls ?? item?.max ?? item?.limit ?? NaN);
          if (!tool) return null;
          if (!Number.isFinite(max_calls) || max_calls < 0) return null;
          return { tool, max_calls: Math.floor(max_calls) };
        })
        .filter(Boolean) as any;
      return parsed.length ? parsed : undefined;
    } catch {
      return undefined;
    }
  }
  const parts = s
    .split(/[,\n]+/g)
    .map((x) => x.trim())
    .filter((x) => x.length > 0);
  const out: { tool: string; max_calls: number }[] = [];
  for (const p of parts) {
    const eq = p.indexOf("=");
    if (eq <= 0) continue;
    const tool = p.slice(0, eq).trim();
    const n = Number(p.slice(eq + 1).trim());
    if (!tool) continue;
    if (!Number.isFinite(n) || n < 0) continue;
    const max_calls = Math.floor(n);
    const existing = out.find((x) => x.tool === tool);
    if (existing) existing.max_calls = max_calls;
    else out.push({ tool, max_calls });
  }
  return out.length ? out : undefined;
};

export const buildWorkflowDefaults = (run: RunSettings): Record<string, any> => {
  const defaults: Record<string, any> = {};

  if (run.tools) defaults.tools = run.tools;
  if (run.tools === "host" && run.hostPolicy) defaults.host_policy = run.hostPolicy;
  if (typeof run.yolo === "boolean") defaults.yolo = run.yolo;
  if (typeof run.verbose === "boolean") defaults.verbose = run.verbose;

  if (run.model) defaults.model = run.model;
  if (run.summaryModel) defaults.summary_model = run.summaryModel;
  const summaryMax = parseNumber(run.summaryMaxChars, 0);
  if (summaryMax !== undefined) defaults.summary_max_chars = summaryMax;

  if (run.baseUrl) defaults.base_url = run.baseUrl;
  if (run.apiKey) defaults.api_key = run.apiKey;
  if (run.proxyUrl) defaults.proxy = run.proxyUrl;
  const timeout = parseNumber(run.timeoutMs, 1);
  if (timeout !== undefined) defaults.timeout_ms = timeout;
  if (typeof run.streamAssistant === "boolean") defaults.stream_assistant = run.streamAssistant;
  const capture = parseNumber(run.maxCaptureBytes, 0);
  if (capture !== undefined) defaults.max_capture_bytes = capture;

  const maxSteps = parseNumber(run.maxSteps, 0);
  if (maxSteps !== undefined) defaults.max_steps = maxSteps;
  const maxRepeated = parseNumber(run.maxRepeatedToolCalls, 0);
  if (maxRepeated !== undefined) defaults.max_repeated_tool_calls = maxRepeated;
  const maxTotal = parseNumber(run.maxToolCallsTotal, 0);
  if (maxTotal !== undefined) defaults.max_tool_calls_total = maxTotal;
  const maxPerTool = parseNumber(run.maxToolCallsPerTool, 0);
  if (maxPerTool !== undefined) defaults.max_tool_calls_per_tool = maxPerTool;
  const toolLimits = parseToolCallLimits(run.toolCallLimits);
  if (toolLimits) defaults.tool_call_limits = toolLimits;

  const maxChars = parseNumber(run.maxChars, 0);
  if (maxChars !== undefined) defaults.max_chars = maxChars;
  const keepLast = parseNumber(run.keepLast, 0);
  if (keepLast !== undefined) defaults.keep_last = keepLast;

  if (run.tools === "host") {
    if (run.memoryContextMode) defaults.memory_context_mode = run.memoryContextMode;
    if (typeof run.memoryIncludeStructured === "boolean") defaults.memory_include_structured = run.memoryIncludeStructured;
    if (typeof run.memoryIncludeCore === "boolean") defaults.memory_include_core = run.memoryIncludeCore;
    if (typeof run.memoryIncludeDaily === "boolean") defaults.memory_include_daily = run.memoryIncludeDaily;
    if (typeof run.memoryIncludeSession === "boolean") defaults.memory_include_session = run.memoryIncludeSession;
    const dailyDays = parseNumber(run.memoryDailyDays, 0);
    if (dailyDays !== undefined) defaults.memory_daily_days = dailyDays;
    const totalCap = parseNumber(run.memoryTotalCap, 0);
    if (totalCap !== undefined) defaults.memory_total_cap = totalCap;
    if (run.memorySearchQuery) defaults.memory_search_query = run.memorySearchQuery;
    if (typeof run.memorySearchUseIndex === "boolean") defaults.memory_search_use_index = run.memorySearchUseIndex;
    if (typeof run.memorySearchCaseSensitive === "boolean") defaults.memory_search_case_sensitive = run.memorySearchCaseSensitive;
    if (typeof run.memorySearchFallbackToFiles === "boolean")
      defaults.memory_search_fallback_to_files = run.memorySearchFallbackToFiles;
    if (run.memorySearchOrder) defaults.memory_search_order = run.memorySearchOrder;
    const maxResults = parseNumber(run.memorySearchMaxResults, 0);
    if (maxResults !== undefined) defaults.memory_search_max_results = maxResults;
    const maxSnippet = parseNumber(run.memorySearchMaxSnippetChars, 0);
    if (maxSnippet !== undefined) defaults.memory_search_max_snippet_chars = maxSnippet;
    const contextLines = parseNumber(run.memorySearchContextLines, 0);
    if (contextLines !== undefined) defaults.memory_search_context_lines = contextLines;
  }

  if (typeof run.trace === "boolean") defaults.trace = run.trace;

  return defaults;
};
