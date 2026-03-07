import React from "react";
import { useMutation } from "@tanstack/react-query";
import { apiRun, apiRunAsync, type ApiAuth, type RunRequest, type RunResponse, type AgentEvent } from "../api";
import type { Attachment } from "../components/PromptBar";

export type QueuedRun = { prompt: string; attachments: Attachment[]; queued_unix_ms: number };

type JobStoreWriter = (mutate: (prev: Record<string, any>) => Record<string, any>) => void;

export type RunExecutionArgs = {
  activeJobId: string | null;
  apiKey: string;
  automationProfileValue?: "full" | "guided" | "strict" | "custom";
  baseUrl: string;
  client: { id: string; kind: string; instance_id: string };
  cursorRef: React.MutableRefObject<number>;
  daemonAuth: ApiAuth;
  effectiveBase: string;
  effectiveUseAsync: boolean;
  hostPolicy: "readonly" | "full";
  jobStoreKey: string;
  keepLast: string;
  lastRunPromptRef: React.MutableRefObject<string>;
  maxCaptureBytes: string;
  maxChars: string;
  maxRepeatedToolCalls: string;
  maxSteps: string;
  maxToolCallsPerTool: string;
  maxToolCallsTotal: string;
  memoryContextMode: string;
  memoryDailyDays: string;
  memoryIncludeCore: boolean;
  memoryIncludeDaily: boolean;
  memoryIncludeSession: boolean;
  memoryIncludeStructured: boolean;
  memorySearchCaseSensitive: boolean;
  memorySearchContextLines: string;
  memorySearchFallbackToFiles: boolean;
  memorySearchMaxResults: string;
  memorySearchMaxSnippetChars: string;
  memorySearchOrder: string;
  memorySearchQuery: string;
  memorySearchUseIndex: boolean;
  memoryTotalCap: string;
  model: string;
  proxyUrl: string;
  runQueue: QueuedRun[];
  sessionId: string;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setComposerTaskNonce: React.Dispatch<React.SetStateAction<number>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobNotice: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLastCompletedPrompt: React.Dispatch<React.SetStateAction<string>>;
  setLastRunPrompt: React.Dispatch<React.SetStateAction<string>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
  setPrompt: React.Dispatch<React.SetStateAction<string>>;
  setResult: React.Dispatch<React.SetStateAction<RunResponse | undefined>>;
  setRunQueue: React.Dispatch<React.SetStateAction<QueuedRun[]>>;
  streamAssistant: boolean;
  summaryMaxChars: string;
  summaryModel: string;
  timeoutMs: string;
  toolCallLimits: string;
  tools: "none" | "basic" | "host";
  trace: boolean;
  verbose: boolean;
  writeJobsBySession: JobStoreWriter;
  yolo: boolean;
  auditRefetch: () => unknown;
  sessionsRefetch: () => unknown;
};

export default function useRunExecution(args: RunExecutionArgs) {
  const {
    activeJobId,
    apiKey,
    automationProfileValue,
    baseUrl,
    client,
    cursorRef,
    daemonAuth,
    effectiveBase,
    effectiveUseAsync,
    hostPolicy,
    jobStoreKey,
    keepLast,
    lastRunPromptRef,
    maxCaptureBytes,
    maxChars,
    maxRepeatedToolCalls,
    maxSteps,
    maxToolCallsPerTool,
    maxToolCallsTotal,
    memoryContextMode,
    memoryDailyDays,
    memoryIncludeCore,
    memoryIncludeDaily,
    memoryIncludeSession,
    memoryIncludeStructured,
    memorySearchCaseSensitive,
    memorySearchContextLines,
    memorySearchFallbackToFiles,
    memorySearchMaxResults,
    memorySearchMaxSnippetChars,
    memorySearchOrder,
    memorySearchQuery,
    memorySearchUseIndex,
    memoryTotalCap,
    model,
    proxyUrl,
    runQueue,
    sessionId,
    setActiveJobId,
    setComposerTaskNonce,
    setJobError,
    setJobNotice,
    setJobStatus,
    setJobUpdatedMs,
    setLastCompletedPrompt,
    setLastRunPrompt,
    setLiveEvents,
    setPrompt,
    setResult,
    setRunQueue,
    streamAssistant,
    summaryMaxChars,
    summaryModel,
    timeoutMs,
    toolCallLimits,
    tools,
    trace,
    verbose,
    writeJobsBySession,
    yolo,
    auditRefetch,
    sessionsRefetch,
  } = args;

  const run = useMutation({
    mutationFn: async (vars: { prompt: string; attachments: Attachment[] }) => {
      const maxStepsTrim = String(maxSteps ?? "").trim();
      const parsedMaxSteps =
        maxStepsTrim.length === 0
          ? undefined
          : Number.isFinite(Number(maxStepsTrim)) && Number(maxStepsTrim) >= 0
            ? Number(maxStepsTrim)
            : undefined;
      const maxToolCallsTotalTrim = String(maxToolCallsTotal ?? "").trim();
      const parsedMaxToolCallsTotal =
        maxToolCallsTotalTrim.length === 0
          ? undefined
          : Number.isFinite(Number(maxToolCallsTotalTrim)) && Number(maxToolCallsTotalTrim) >= 0
            ? Number(maxToolCallsTotalTrim)
            : undefined;
      const maxToolCallsPerToolTrim = String(maxToolCallsPerTool ?? "").trim();
      const parsedMaxToolCallsPerTool =
        maxToolCallsPerToolTrim.length === 0
          ? undefined
          : Number.isFinite(Number(maxToolCallsPerToolTrim)) && Number(maxToolCallsPerToolTrim) >= 0
            ? Number(maxToolCallsPerToolTrim)
            : undefined;

      const toolCallLimitsTrim = String(toolCallLimits ?? "").trim();
      let parsedToolCallLimits: { tool: string; max_calls: number }[] | undefined;
      if (toolCallLimitsTrim.length > 0) {
        const value = toolCallLimitsTrim;
        if (value.startsWith("[") || value.startsWith("{")) {
          try {
            const json: any = JSON.parse(value);
            const entries = Array.isArray(json) ? json : [json];
            parsedToolCallLimits = entries
              .map((item) => {
                const tool = String(item?.tool ?? item?.name ?? "").trim();
                const max_calls = Number(item?.max_calls ?? item?.maxCalls ?? item?.max ?? item?.limit ?? NaN);
                if (!tool) return null;
                if (!Number.isFinite(max_calls) || max_calls < 0) return null;
                return { tool, max_calls: Math.floor(max_calls) };
              })
              .filter(Boolean) as { tool: string; max_calls: number }[];
          } catch (error) {
            throw new Error(`Invalid tool call limits JSON: ${String(error)}`);
          }
        } else {
          const parts = value
            .split(/[,\n]+/g)
            .map((item) => item.trim())
            .filter((item) => item.length > 0);
          const out: { tool: string; max_calls: number }[] = [];
          for (const part of parts) {
            const eq = part.indexOf("=");
            if (eq <= 0) throw new Error(`Invalid tool call limit (expected tool=max_calls): ${part}`);
            const tool = part.slice(0, eq).trim();
            const rawLimit = Number(part.slice(eq + 1).trim());
            if (!tool) throw new Error(`Invalid tool call limit tool name: ${part}`);
            if (!Number.isFinite(rawLimit) || rawLimit < 0) throw new Error(`Invalid tool call limit value: ${part}`);
            const max_calls = Math.floor(rawLimit);
            const existing = out.find((entry) => entry.tool === tool);
            if (existing) existing.max_calls = max_calls;
            else out.push({ tool, max_calls });
          }
          parsedToolCallLimits = out.length > 0 ? out : undefined;
        }
      }

      const memMode =
        memoryContextMode === "search" || memoryContextMode === "index" || memoryContextMode === "salience"
          ? memoryContextMode
          : "files";
      const parseNonNegativeNumber = (raw: string) => {
        const trimmed = String(raw ?? "").trim();
        if (trimmed.length === 0) return undefined;
        const value = Number(trimmed);
        return Number.isFinite(value) && value >= 0 ? value : undefined;
      };
      const parsedMemDailyDays = parseNonNegativeNumber(memoryDailyDays);
      const parsedMemTotalCap = parseNonNegativeNumber(memoryTotalCap);
      const parsedMemSearchMaxResults = parseNonNegativeNumber(memorySearchMaxResults);
      const parsedMemSearchMaxSnippetChars = parseNonNegativeNumber(memorySearchMaxSnippetChars);
      const parsedMemSearchContextLines = parseNonNegativeNumber(memorySearchContextLines);
      const memSearchQueryTrim = String(memorySearchQuery ?? "").trim();

      const req: RunRequest = {
        prompt: vars.prompt,
        session_id: sessionId || undefined,
        no_session: false,
        input_files:
          vars.attachments.length > 0
            ? vars.attachments.map((attachment) => ({
                path: attachment.path,
                name: attachment.name,
                mime: attachment.mime,
                kind: attachment.kind,
              }))
            : undefined,
        client: { ...client, kind: "webui" },
        tools,
        host_policy: tools === "host" ? hostPolicy : undefined,
        automation_profile: automationProfileValue,
        yolo,
        verbose,
        model: model || undefined,
        summary_model: summaryModel && summaryModel.trim().length > 0 ? summaryModel.trim() : undefined,
        summary_max_chars:
          Number.isFinite(Number(summaryMaxChars)) && Number(summaryMaxChars) >= 0 ? Number(summaryMaxChars) : undefined,
        base_url: baseUrl || undefined,
        api_key: apiKey || undefined,
        proxy: proxyUrl && proxyUrl.trim().length > 0 ? proxyUrl.trim() : undefined,
        timeout_ms: Number.isFinite(Number(timeoutMs)) && Number(timeoutMs) > 0 ? Number(timeoutMs) : undefined,
        stream_assistant: streamAssistant,
        max_capture_bytes:
          Number.isFinite(Number(maxCaptureBytes)) && Number(maxCaptureBytes) >= 0 ? Number(maxCaptureBytes) : undefined,
        max_steps: parsedMaxSteps,
        max_repeated_tool_calls:
          Number.isFinite(Number(maxRepeatedToolCalls)) && Number(maxRepeatedToolCalls) >= 0
            ? Number(maxRepeatedToolCalls)
            : undefined,
        max_tool_calls_total: parsedMaxToolCallsTotal,
        max_tool_calls_per_tool: parsedMaxToolCallsPerTool,
        tool_call_limits: parsedToolCallLimits,
        max_chars: Number.isFinite(Number(maxChars)) ? Number(maxChars) : 20000,
        keep_last: Number.isFinite(Number(keepLast)) ? Number(keepLast) : 16,
        trace,
      };
      if (tools === "host") {
        req.memory_context_mode = memMode;
        req.memory_include_structured = memoryIncludeStructured;
        req.memory_include_core = memoryIncludeCore;
        req.memory_include_daily = memoryIncludeDaily;
        req.memory_include_session = memoryIncludeSession;
        req.memory_daily_days = parsedMemDailyDays;
        req.memory_total_cap = parsedMemTotalCap;
        if (memSearchQueryTrim.length > 0) req.memory_search_query = memSearchQueryTrim;
        req.memory_search_use_index = memorySearchUseIndex;
        req.memory_search_case_sensitive = memorySearchCaseSensitive;
        req.memory_search_fallback_to_files = memorySearchFallbackToFiles;
        const memOrder =
          memorySearchOrder === "newest" || memorySearchOrder === "oldest" || memorySearchOrder === "ranked"
            ? (memorySearchOrder as "ranked" | "newest" | "oldest")
            : "ranked";
        if (memOrder !== "ranked") req.memory_search_order = memOrder;
        req.memory_search_max_results = parsedMemSearchMaxResults;
        req.memory_search_max_snippet_chars = parsedMemSearchMaxSnippetChars;
        req.memory_search_context_lines = parsedMemSearchContextLines;
      }
      if (effectiveUseAsync) {
        const job = await apiRunAsync(effectiveBase, req, daemonAuth);
        return { mode: "async" as const, job, req };
      }
      const out = await apiRun(effectiveBase, req, daemonAuth);
      return { mode: "sync" as const, out, req };
    },
    onSuccess: (value) => {
      if (value.mode === "sync") {
        setComposerTaskNonce((count) => count + 1);
        lastRunPromptRef.current = value.req.prompt;
        setLastRunPrompt(value.req.prompt);
        setLastCompletedPrompt(value.req.prompt);
        setResult(value.out);
        setLiveEvents([]);
        setJobError(null);
        setJobNotice(null);
        setJobStatus(null);
        setJobUpdatedMs(null);
        void auditRefetch();
        void sessionsRefetch();
        return;
      }
      if (!value.job.ok || !value.job.job_id) {
        setJobError(value.job.error ?? "failed to start async job");
        setJobNotice(null);
        return;
      }
      setComposerTaskNonce((count) => count + 1);
      lastRunPromptRef.current = value.req.prompt;
      setLastRunPrompt(value.req.prompt);
      setJobError(null);
      setJobNotice(null);
      setJobStatus(null);
      setJobUpdatedMs(null);
      setLiveEvents([]);
      cursorRef.current = 0;
      setActiveJobId(value.job.job_id);
      setJobStatus("queued");

      const sid = String(sessionId || "").trim();
      if (sid) {
        writeJobsBySession((prev) => ({
          ...prev,
          [jobStoreKey]: { job_id: value.job.job_id, cursor: 0, started_unix_ms: Date.now() },
        }));
      }
    },
    onError: (error) => {
      setJobError(`run failed: ${String(error)}`);
      setJobNotice(null);
    },
  });

  const dequeueInFlightRef = React.useRef(false);

  const enqueueRun = React.useCallback(
    (vars: { prompt: string; attachments: Attachment[] }) => {
      const trimmed = String(vars.prompt || "").trim();
      if (!trimmed && vars.attachments.length === 0) {
        setJobNotice("prompt or attachment required");
        return;
      }
      setRunQueue((prev) => [
        ...(Array.isArray(prev) ? prev : []),
        { prompt: trimmed, attachments: vars.attachments, queued_unix_ms: Date.now() },
      ]);
      setPrompt("");
      setComposerTaskNonce((count) => count + 1);
      setJobNotice("queued");
    },
    [setComposerTaskNonce, setJobNotice, setPrompt, setRunQueue],
  );

  React.useEffect(() => {
    if (activeJobId || run.isPending) return;
    if (!Array.isArray(runQueue) || runQueue.length === 0) return;
    if (dequeueInFlightRef.current) return;
    const next = runQueue[0];
    if (!next || typeof next !== "object") return;
    dequeueInFlightRef.current = true;
    setRunQueue((prev) => (Array.isArray(prev) ? prev.slice(1) : []));
    run
      .mutateAsync({ prompt: next.prompt, attachments: next.attachments || [] })
      .catch((error) => {
        setJobNotice(`queued run failed: ${String(error)}`);
        setRunQueue((prev) => [next, ...(Array.isArray(prev) ? prev : [])]);
      })
      .finally(() => {
        dequeueInFlightRef.current = false;
      });
  }, [activeJobId, run.isPending, runQueue, run, setJobNotice, setRunQueue]);

  const handleDirectRunRequest = React.useCallback(
    async (vars: { prompt: string; attachments: Attachment[] }) => {
      if (activeJobId || run.isPending) {
        enqueueRun(vars);
        return;
      }
      run.mutate(vars);
    },
    [activeJobId, enqueueRun, run],
  );

  return {
    handleDirectRunRequest,
    run,
  };
}
