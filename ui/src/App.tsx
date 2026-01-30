import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiGetAudit,
  apiGetConfig,
  apiGetDbRun,
  apiGetDbRuns,
  apiGetDbUiActions,
  apiGetDbMessages,
  apiGetDbClientEvents,
  apiGetSessionClientEvents,
  apiGetHealth,
  apiGetJobProgress,
  apiGetOpenRouterModels,
  apiGetSessionArtifacts,
  apiGetTools,
  apiCancelJob,
  apiListSessions,
  apiNewSession,
  apiRun,
  apiRunAsync,
  daemonHeaders,
  RunRequest,
  RunResponse,
  type AgentEvent,
} from "./api";
import TraceView from "./components/TraceView";
import EventTimeline from "./components/EventTimeline";
import Markdown from "./components/Markdown";
import ConversationView from "./components/ConversationView";
import ArtifactView from "./components/ArtifactView";
import DbRunsView from "./components/DbRunsView";
import DbUiActionsView from "./components/DbUiActionsView";
import DbMessagesView from "./components/DbMessagesView";
import DbClientEventsView from "./components/DbClientEventsView";
import useLocalStorageState from "./hooks/useLocalStorageState";
import { readSseStream } from "./sse";

function Label({ children }: { children: React.ReactNode }) {
  return <div className="text-xs font-medium text-white/70">{children}</div>;
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export default function App() {
  const [showSettings, setShowSettings] = useLocalStorageState("agentui.showSettings", true);

  const [base, setBase] = useLocalStorageState("agentui.base", "http://127.0.0.1:8123");
  const [daemonAuthToken, setDaemonAuthToken] = useLocalStorageState("agentui.daemonAuthToken", "");
  const [prompt, setPrompt] = useLocalStorageState("agentui.prompt", "inspect this project and explain");
  const [sessionId, setSessionId] = useLocalStorageState("agentui.sessionId", "default");
  const [clientId] = useLocalStorageState(
    "agentui.clientId",
    (() => {
      try {
        // Stable per-browser-profile id (shared across tabs); used as client.id.
        // A per-tab instance id is generated separately.
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const g: any = typeof globalThis !== "undefined" ? globalThis : {};
        if (g.crypto && typeof g.crypto.randomUUID === "function") {
          return `webui-${g.crypto.randomUUID()}`;
        }
      } catch {
        // ignore
      }
      return `webui-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    })(),
  );
  const clientInstanceIdRef = React.useRef<string>(
    (() => {
      try {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const g: any = typeof globalThis !== "undefined" ? globalThis : {};
        if (g.crypto && typeof g.crypto.randomUUID === "function") {
          return `tab-${g.crypto.randomUUID()}`;
        }
      } catch {
        // ignore
      }
      return `tab-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    })(),
  );
  const client = React.useMemo(
    () => ({ id: String(clientId || "webui"), kind: "webui", instance_id: clientInstanceIdRef.current }),
    [clientId],
  );
  const [tools, setTools] = useLocalStorageState<"host" | "basic" | "none">("agentui.tools", "host");
  const [toolsRoot, setToolsRoot] = useLocalStorageState("agentui.toolsRoot", ".");
  const [yolo, setYolo] = useLocalStorageState("agentui.yolo", true);
  const [hostPolicy, setHostPolicy] = useLocalStorageState<"full" | "readonly">("agentui.hostPolicy", "full");
  const [verbose, setVerbose] = useLocalStorageState("agentui.verbose", false);
  const [model, setModel] = useLocalStorageState("agentui.model", "deepseek-chat");
  const [summaryModel, setSummaryModel] = useLocalStorageState("agentui.summaryModel", "");
  const [summaryMaxChars, setSummaryMaxChars] = useLocalStorageState("agentui.summaryMaxChars", "1200");
  const [baseUrl, setBaseUrl] = useLocalStorageState("agentui.baseUrl", "https://api.deepseek.com");
  const [apiKey, setApiKey] = useLocalStorageState("agentui.apiKey", "");
  // Many dev environments require a local HTTP proxy for outbound HTTPS (and this repo's test scripts assume it).
  // Users can clear this if their environment does not need a proxy.
  const [proxyUrl, setProxyUrl] = useLocalStorageState("agentui.proxyUrl", "http://localhost:8120");
  const [timeoutMs, setTimeoutMs] = useLocalStorageState("agentui.timeoutMs", "60000");
  const [maxCaptureBytes, setMaxCaptureBytes] = useLocalStorageState("agentui.maxCaptureBytes", "65536");
  const [streamAssistant, setStreamAssistant] = useLocalStorageState("agentui.streamAssistant", false);
  const [orMinTotal, setOrMinTotal] = useLocalStorageState("agentui.orMinTotal", "0.01");
  const [orMaxTotal, setOrMaxTotal] = useLocalStorageState("agentui.orMaxTotal", "0.50");
  const [orRequireMultimodal, setOrRequireMultimodal] = useLocalStorageState("agentui.orRequireMultimodal", true);
  const [orRequireTools, setOrRequireTools] = useLocalStorageState("agentui.orRequireTools", true);
  const [orLimit, setOrLimit] = useLocalStorageState("agentui.orLimit", "50");
  // Blank means "use daemon default". This avoids accidental infinite loops.
  // Explicit `0` is still supported and means unlimited.
  const [maxSteps, setMaxSteps] = useLocalStorageState("agentui.maxSteps", "");
  const [maxStepsUserSet, setMaxStepsUserSet] = useLocalStorageState("agentui.maxStepsUserSet", false);
  const [maxRepeatedToolCalls, setMaxRepeatedToolCalls] = useLocalStorageState("agentui.maxRepeatedToolCalls", "12");
  const [maxToolCallsTotal, setMaxToolCallsTotal] = useLocalStorageState("agentui.maxToolCallsTotal", "");
  const [maxToolCallsPerTool, setMaxToolCallsPerTool] = useLocalStorageState("agentui.maxToolCallsPerTool", "");
  const [toolCallLimits, setToolCallLimits] = useLocalStorageState("agentui.toolCallLimits", "");
  const [maxChars, setMaxChars] = useLocalStorageState("agentui.maxChars", "20000");
  const [keepLast, setKeepLast] = useLocalStorageState("agentui.keepLast", "16");
  const [trace, setTrace] = useLocalStorageState("agentui.trace", true);
  const [useAsync, setUseAsync] = useLocalStorageState("agentui.useAsync", true);
  const [showDebugInConversation, setShowDebugInConversation] = useLocalStorageState(
    "agentui.showDebugInConversation",
    false,
  );
  const [allowAutoplay, setAllowAutoplay] = useLocalStorageState("agentui.allowAutoplay", false);
  const [dbRunsOnlyErrors, setDbRunsOnlyErrors] = useLocalStorageState("agentui.dbRunsOnlyErrors", true);
  const [dbRunsStopReason, setDbRunsStopReason] = useLocalStorageState("agentui.dbRunsStopReason", "");
  // Keep prompts separate so an active async run does not overwrite the "last completed" view.
  const [lastRunPrompt, setLastRunPrompt] = React.useState("");
  const [lastCompletedPrompt, setLastCompletedPrompt] = React.useState("");
  const lastRunPromptRef = React.useRef<string>("");

  const [activeJobId, setActiveJobId] = React.useState<string | null>(null);
  const [jobStatus, setJobStatus] = React.useState<string | null>(null);
  const [jobError, setJobError] = React.useState<string | null>(null);
  const [jobUpdatedMs, setJobUpdatedMs] = React.useState<number | null>(null);
  const [liveEvents, setLiveEvents] = React.useState<AgentEvent[]>([]);
  const cursorRef = React.useRef<number>(0);

  const effectiveBase = React.useMemo(() => {
    const b = String(base || "").trim();
    if (b.length === 0) return "http://127.0.0.1:8123";
    const withScheme = /^https?:\/\//i.test(b) ? b : `http://${b}`;
    return withScheme.replace(/\/+$/, "");
  }, [base]);

  const health = useQuery({
    queryKey: ["health", effectiveBase, daemonAuthToken],
    queryFn: () => apiGetHealth(effectiveBase, daemonAuthToken),
    retry: 1,
  });

  // Migration: older UI versions defaulted `maxSteps` to "0" (unlimited).
  // If the user never explicitly set a value, move to blank so daemon defaults apply.
  React.useEffect(() => {
    if (maxStepsUserSet) return;
    if (String(maxSteps) === "0") {
      setMaxSteps("");
    }
  }, [maxSteps, maxStepsUserSet, setMaxSteps]);

  const daemonConfig = useQuery({
    queryKey: ["config", effectiveBase, daemonAuthToken],
    queryFn: () => apiGetConfig(effectiveBase, daemonAuthToken),
    retry: 1,
  });

  const sessions = useQuery({
    queryKey: ["sessions", effectiveBase, daemonAuthToken],
    queryFn: () => apiListSessions(effectiveBase, daemonAuthToken),
    retry: 1,
  });

  const audit = useQuery({
    queryKey: ["audit", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetAudit(effectiveBase, sessionId, daemonAuthToken),
    enabled: !!sessionId,
    retry: 1,
  });

  const sessionClientEvents = useQuery({
    queryKey: ["session_client_events", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetSessionClientEvents(effectiveBase, sessionId, daemonAuthToken, { maxBytes: 1024 * 1024 }),
    enabled: !!sessionId,
    retry: 1,
  });

  const sessionArtifacts = useQuery({
    queryKey: ["session_artifacts", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetSessionArtifacts(effectiveBase, sessionId, daemonAuthToken, { maxBytes: 2 * 1024 * 1024, maxArtifacts: 64 }),
    enabled: !!sessionId,
    retry: 1,
  });

  const dbRuns = useQuery({
    queryKey: ["db_runs", effectiveBase, daemonAuthToken, sessionId, dbRunsOnlyErrors, dbRunsStopReason],
    queryFn: () =>
      apiGetDbRuns(effectiveBase, sessionId, daemonAuthToken, {
        limit: 50,
        offset: 0,
        onlyErrors: dbRunsOnlyErrors,
        stopReason: dbRunsStopReason,
      }),
    enabled: false,
    retry: 1,
  });

  const dbUiActions = useQuery({
    queryKey: ["db_ui_actions", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetDbUiActions(effectiveBase, sessionId, daemonAuthToken, { limit: 100, offset: 0 }),
    enabled: false,
    retry: 1,
  });

  const dbMessages = useQuery({
    queryKey: ["db_messages", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetDbMessages(effectiveBase, sessionId, daemonAuthToken, { limit: 80, offset: 0, maxContentBytes: 8192 }),
    enabled: false,
    retry: 1,
  });

  const dbClientEvents = useQuery({
    queryKey: ["db_client_events", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetDbClientEvents(effectiveBase, sessionId, daemonAuthToken, { limit: 100, offset: 0 }),
    enabled: false,
    retry: 1,
  });

  const [selectedDbRunId, setSelectedDbRunId] = React.useState<number | null>(null);
  const dbRunDetail = useQuery({
    queryKey: ["db_run", effectiveBase, daemonAuthToken, selectedDbRunId],
    queryFn: () =>
      selectedDbRunId
        ? apiGetDbRun(effectiveBase, selectedDbRunId, daemonAuthToken, {
            includeEvents: true,
            includeTools: true,
            includeArtifacts: true,
            includeUiActions: true,
          })
        : Promise.resolve({ ok: false, error: "no run selected" }),
    enabled: selectedDbRunId !== null,
    retry: 1,
  });

  const newSession = useMutation({
    mutationFn: async () => {
      const r = await apiNewSession(effectiveBase, daemonAuthToken);
      return r;
    },
    onSuccess: (v) => {
      if (v.ok && v.session_id) {
        setSessionId(v.session_id);
        void sessions.refetch();
        void audit.refetch();
        void sessionClientEvents.refetch();
        void sessionArtifacts.refetch();
        void dbUiActions.refetch();
        void dbMessages.refetch();
        void dbClientEvents.refetch();
        setSelectedDbRunId(null);
      }
    },
  });

  const autoSessionInitRef = React.useRef(false);
  React.useEffect(() => {
    if (autoSessionInitRef.current) return;
    if (!sessions.isSuccess) return;
    const ids = sessions.data?.sessions ?? [];
    // If the UI is still on the historical "default" placeholder but no such session exists,
    // create a new unique session id to avoid collisions across tabs/clients.
    if (sessionId === "default" && !ids.includes("default")) {
      autoSessionInitRef.current = true;
      void newSession.mutateAsync().catch(() => {
        // keep default; user can retry via button
      });
    }
  }, [sessionId, sessions.isSuccess, sessions.data?.sessions, newSession]);

  const toolsDefs = useQuery({
    queryKey: ["tools", effectiveBase, daemonAuthToken, tools, toolsRoot, yolo, hostPolicy, sessionId],
    queryFn: () =>
      apiGetTools(effectiveBase, daemonAuthToken, {
        tools,
        toolsRoot,
        yolo,
        hostPolicy: tools === "host" ? hostPolicy : undefined,
        sessionId,
      }),
    retry: 1,
  });

  const [result, setResult] = React.useState<RunResponse | undefined>(undefined);
  const [openrouterModels, setOpenrouterModels] = React.useState<any | null>(null);

  const run = useMutation({
    mutationFn: async () => {
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
      let parsedToolCallLimits: { tool: string; max_calls: number }[] | undefined = undefined;
      if (toolCallLimitsTrim.length > 0) {
        const s = toolCallLimitsTrim;
        if (s.startsWith("[") || s.startsWith("{")) {
          try {
            const v: any = JSON.parse(s);
            const arr = Array.isArray(v) ? v : [v];
            parsedToolCallLimits = arr
              .map((item) => {
                const tool = String(item?.tool ?? item?.name ?? "").trim();
                const max_calls = Number(item?.max_calls ?? item?.maxCalls ?? item?.max ?? item?.limit ?? NaN);
                if (!tool) return null;
                if (!Number.isFinite(max_calls) || max_calls < 0) return null;
                return { tool, max_calls: Math.floor(max_calls) };
              })
              .filter(Boolean) as any;
          } catch (e) {
            throw new Error(`Invalid tool call limits JSON: ${String(e)}`);
          }
        } else {
          const parts = s
            .split(/[,\n]+/g)
            .map((x) => x.trim())
            .filter((x) => x.length > 0);
          const out: { tool: string; max_calls: number }[] = [];
          for (const p of parts) {
            const eq = p.indexOf("=");
            if (eq <= 0) throw new Error(`Invalid tool call limit (expected tool=max_calls): ${p}`);
            const tool = p.slice(0, eq).trim();
            const n = Number(p.slice(eq + 1).trim());
            if (!tool) throw new Error(`Invalid tool call limit tool name: ${p}`);
            if (!Number.isFinite(n) || n < 0) throw new Error(`Invalid tool call limit value: ${p}`);
            const max_calls = Math.floor(n);
            const existing = out.find((x) => x.tool === tool);
            if (existing) existing.max_calls = max_calls;
            else out.push({ tool, max_calls });
          }
          parsedToolCallLimits = out;
        }
        if (parsedToolCallLimits && parsedToolCallLimits.length === 0) {
          parsedToolCallLimits = undefined;
        }
      }
      const req: RunRequest = {
        prompt,
        session_id: sessionId || undefined,
        no_session: false,
        tools,
        tools_root: toolsRoot,
        host_policy: tools === "host" ? hostPolicy : undefined,
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
          Number.isFinite(Number(maxRepeatedToolCalls)) && Number(maxRepeatedToolCalls) >= 0 ? Number(maxRepeatedToolCalls) : undefined,
        max_tool_calls_total: parsedMaxToolCallsTotal,
        max_tool_calls_per_tool: parsedMaxToolCallsPerTool,
        tool_call_limits: parsedToolCallLimits,
        max_chars: Number.isFinite(Number(maxChars)) ? Number(maxChars) : 20000,
        keep_last: Number.isFinite(Number(keepLast)) ? Number(keepLast) : 16,
        trace,
      };
      if (useAsync) {
        const job = await apiRunAsync(effectiveBase, req, daemonAuthToken);
        return { mode: "async" as const, job, req };
      }
      const out = await apiRun(effectiveBase, req, daemonAuthToken);
      return { mode: "sync" as const, out, req };
    },
    onSuccess: (v) => {
      if (v.mode === "sync") {
        // Only replace history once we have a new result (prevents "fetch failed" from wiping the UI).
        lastRunPromptRef.current = v.req.prompt;
        setLastRunPrompt(v.req.prompt);
        setLastCompletedPrompt(v.req.prompt);
        setResult(v.out);
        setLiveEvents([]);
        setJobError(null);
        setJobStatus(null);
        setJobUpdatedMs(null);
        void audit.refetch();
        void sessions.refetch();
        return;
      }
      if (!v.job.ok || !v.job.job_id) {
        setJobError(v.job.error ?? "failed to start async job");
        return;
      }
      // Only reset/replace history after the job has been successfully created.
      lastRunPromptRef.current = v.req.prompt;
      setLastRunPrompt(v.req.prompt);
      setJobError(null);
      setJobStatus(null);
      setJobUpdatedMs(null);
      setLiveEvents([]);
      cursorRef.current = 0;
      setActiveJobId(v.job.job_id);
      setJobStatus("queued");
    },
    onError: (e) => {
      // Keep the last conversation visible when a run cannot be started.
      setJobError(`run failed: ${String(e)}`);
    },
  });

  const fetchOpenRouterModels = useMutation({
    mutationFn: async () => {
      const minTotal = Number(orMinTotal);
      const maxTotal = Number(orMaxTotal);
      const limit = Number(orLimit);
      return apiGetOpenRouterModels(effectiveBase, {
        daemonAuthToken: daemonAuthToken || undefined,
        apiKey: apiKey || undefined,
        openrouterBaseUrl: "https://openrouter.ai/api/v1",
        minTotal: Number.isFinite(minTotal) ? minTotal : 0.01,
        maxTotal: Number.isFinite(maxTotal) ? maxTotal : 0.5,
        requireMultimodalInput: orRequireMultimodal,
        requireTools: orRequireTools,
        includeFree: false,
        limit: Number.isFinite(limit) ? limit : 50,
        refresh: true,
      });
    },
    onSuccess: (v) => {
      setOpenrouterModels(v);
      if (v.ok && v.recommended_model && typeof v.recommended_model === "string" && v.recommended_model.length > 0) {
        // Convenience: prime the model field with the recommended choice.
        setModel(v.recommended_model);
        setBaseUrl("https://openrouter.ai/api/v1");
      }
    },
  });

  React.useEffect(() => {
    if (!activeJobId) return;
    let cancelled = false;
    const jobId = activeJobId;

    const startPolling = () => {
      (async () => {
        // Poll job progress + stream events via cursor.
        for (;;) {
          if (cancelled) return;
          let job: any;
          try {
            job = await apiGetJobProgress(effectiveBase, jobId, daemonAuthToken, { cursor: cursorRef.current, maxEvents: 256 });
          } catch (e) {
            // Transient fetch failures should not invalidate the visible conversation.
            // Keep the current liveEvents and keep the job active; retry with backoff.
            setJobError(`job fetch failed: ${String(e)}`);
            await sleep(1000);
            continue;
          }

          if (cancelled) return;
          setJobStatus(job.status ?? null);
          setJobError(job.error ?? null);
          setJobUpdatedMs(typeof job.updated_unix_ms === "number" ? job.updated_unix_ms : null);

          const ev = Array.isArray(job.events) ? job.events : [];
          const next = typeof job.events_cursor_next === "number" ? job.events_cursor_next : cursorRef.current + ev.length;
          if (ev.length > 0) {
            if (job.events_reset) {
              setLiveEvents(ev);
            } else {
              setLiveEvents((prev) => prev.concat(ev));
            }
            cursorRef.current = next;
          }

          if (job.status === "done" || job.status === "error") {
            if (job.result) {
              setResult(job.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
            } else {
              setJobError("job completed but missing result");
            }
            setActiveJobId(null);
            void audit.refetch();
            void sessions.refetch();
            return;
          }

          await sleep(500);
        }
      })().catch((e) => {
        if (cancelled) return;
        // Keep job state visible; do not invalidate the conversation on unexpected polling loop errors.
        setJobError(`polling loop failed: ${String(e)}`);
      });
    };

    // Prefer SSE streaming when available. Fall back to polling.
    let es: EventSource | null = null;
    let fetchAbort: AbortController | null = null;
    let fetchFinished = false;
    const canUseEventSource =
      typeof EventSource !== "undefined" &&
      (!daemonAuthToken || daemonAuthToken.trim().length === 0) &&
      typeof effectiveBase === "string" &&
      (effectiveBase.startsWith("http://") || effectiveBase.startsWith("https://"));
    const canUseFetchSse =
      typeof fetch !== "undefined" &&
      typeof effectiveBase === "string" &&
      (effectiveBase.startsWith("http://") || effectiveBase.startsWith("https://"));

    let fallbackStarted = false;
    const fallbackToPolling = () => {
      if (fallbackStarted) return;
      fallbackStarted = true;
      startPolling();
    };

    const startFetchSse = () => {
      const url = `${effectiveBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
        String(cursorRef.current),
      )}`;
      const controller = new AbortController();
      fetchAbort = controller;
      setJobStatus("running");
      setJobUpdatedMs(null);

      (async () => {
        const resp = await fetch(url, {
          headers: daemonHeaders(daemonAuthToken || undefined),
          signal: controller.signal,
        });
        if (!resp.ok) {
          throw new Error(`SSE fetch failed: HTTP ${resp.status}`);
        }
        await readSseStream(resp, (evt) => {
          if (cancelled) return;
          if (evt.id && evt.id.length > 0) {
            const n = Number(evt.id);
            if (Number.isFinite(n)) cursorRef.current = n + 1;
          }
          if (evt.event === "reset") {
            try {
              const data = JSON.parse(String(evt.data || "{}"));
              if (typeof data?.cursor_base === "number") {
                cursorRef.current = data.cursor_base;
              }
            } catch {
              // ignore
            }
            setLiveEvents([]);
          } else if (evt.event === "agent_event") {
            try {
              const ev = JSON.parse(String(evt.data || "{}"));
              setLiveEvents((prev) => prev.concat(ev));
            } catch {
              // ignore malformed
            }
          } else if (evt.event === "job_done") {
            if (cancelled) return;
            try {
              const data = JSON.parse(String(evt.data || "{}"));
              setJobStatus(typeof data?.status === "string" ? data.status : "done");
              setJobError(typeof data?.error === "string" ? data.error : null);
              if (data?.result) {
                setResult(data.result);
                setLastCompletedPrompt(lastRunPromptRef.current);
              }
            } catch {
              setJobError("failed to parse job_done event");
            }
            fetchFinished = true;
            setActiveJobId(null);
            void audit.refetch();
            void sessions.refetch();
          }
        });
        if (!cancelled && !fetchFinished) {
          // Stream ended without a terminal job_done event; fall back to polling so the UI still progresses.
          fallbackToPolling();
        }
      })().catch((e) => {
        if (cancelled || fetchFinished) return;
        setJobError(`SSE fetch failed: ${String(e)}`);
        fallbackToPolling();
      });
    };

    if (canUseEventSource) {
      try {
        const url = `${effectiveBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
          String(cursorRef.current),
        )}`;
        es = new EventSource(url);
        setJobStatus("running");
        setJobUpdatedMs(null);
        es.onopen = () => {
          // ok
        };
        es.addEventListener("reset", (evt: any) => {
          if (cancelled) return;
          try {
            const data = JSON.parse(String(evt.data || "{}"));
            if (typeof data?.cursor_base === "number") {
              cursorRef.current = data.cursor_base;
            }
          } catch {
            // ignore
          }
          setLiveEvents([]);
        });
        es.addEventListener("agent_event", (evt: any) => {
          if (cancelled) return;
          try {
            const ev = JSON.parse(String(evt.data || "{}"));
            if (evt && typeof evt.lastEventId === "string" && evt.lastEventId.length > 0) {
              const n = Number(evt.lastEventId);
              if (Number.isFinite(n)) cursorRef.current = n + 1;
            }
            setLiveEvents((prev) => prev.concat(ev));
          } catch {
            // ignore malformed
          }
        });
        es.addEventListener("job_done", (evt: any) => {
          if (cancelled) return;
          try {
            const data = JSON.parse(String(evt.data || "{}"));
            setJobStatus(typeof data?.status === "string" ? data.status : "done");
            setJobError(typeof data?.error === "string" ? data.error : null);
            if (data?.result) {
              setResult(data.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
            }
          } catch {
            setJobError("failed to parse job_done event");
          }
          setActiveJobId(null);
          try {
            es?.close();
          } catch {
            // ignore
          }
          void audit.refetch();
          void sessions.refetch();
        });
        es.onerror = () => {
          if (cancelled) return;
          try {
            es?.close();
          } catch {
            // ignore
          }
          // Any SSE failure should fall back to polling (cursor-based) so the UI keeps progressing.
          // Do not clear liveEvents; keep the conversation visible.
          fallbackToPolling();
        };
      } catch {
        fallbackToPolling();
      }
    } else if (canUseFetchSse) {
      startFetchSse();
    } else {
      fallbackToPolling();
    }

    return () => {
      cancelled = true;
      try {
        es?.close();
      } catch {
        // ignore
      }
      try {
        fetchAbort?.abort();
      } catch {
        // ignore
      }
    };
  }, [activeJobId, effectiveBase, audit, sessions]);

  return (
    <div className="mx-auto max-w-5xl px-6 py-8">
      <div className="mb-6 flex items-baseline justify-between">
        <div>
          <div className="text-xl font-semibold">agent UI</div>
          <div className="text-sm text-white/60">
            daemon: <span className="font-mono text-[12px] text-white/70">{effectiveBase}</span>{" "}
            {health.isSuccess ? (
              <span className="text-emerald-300">
                ok ({health.data.service ?? "agentd"} {health.data.version ?? ""})
              </span>
            ) : health.isFetching ? (
              <span className="text-white/60">checking…</span>
            ) : (
              <span className="text-rose-300">offline</span>
            )}
            {health.isError ? (
              <div className="mt-1 text-[11px] text-rose-200/80">health check failed: {String(health.error)}</div>
            ) : null}
          </div>
        </div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={() => health.refetch()}
            type="button"
          >
            Recheck
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={() => setShowSettings((v) => !v)}
            type="button"
          >
            {showSettings ? "Hide settings" : "Show settings"}
          </button>
        </div>
      </div>

      <div className="grid gap-4 md:grid-cols-3">
        {showSettings ? (
          <div className="rounded-xl border border-white/10 bg-white/5 p-4">
            <div className="mb-3 text-sm font-semibold">Sessions</div>
            <div className="text-xs text-white/60">
              {sessions.isFetching ? "Loading…" : sessions.isError ? "Failed to load" : null}
            </div>
            <div className="mt-3 flex gap-2">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                onClick={() => sessions.refetch()}
                type="button"
              >
                Refresh
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => newSession.mutate()}
                type="button"
                disabled={newSession.isPending}
                title="Create a new unique session id"
              >
                New session
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                onClick={() => toolsDefs.refetch()}
                type="button"
              >
                Refresh tools
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                onClick={() => audit.refetch()}
                type="button"
                disabled={!sessionId}
              >
                Load audit
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                onClick={() => sessionClientEvents.refetch()}
                type="button"
                disabled={!sessionId}
                title="Reads <session>.client_events.jsonl (works even if DB is disabled)"
              >
                Load client events
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                onClick={() => sessionArtifacts.refetch()}
                type="button"
                disabled={!sessionId}
              >
                Load artifacts
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => {
                  setSelectedDbRunId(null);
                  void dbRuns.refetch();
                }}
                type="button"
                disabled={!sessionId || dbRuns.isFetching}
                title="Query the optional SQLite troubleshooting DB (requires agentd --db-path)"
              >
                Load DB runs
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => {
                  void dbUiActions.refetch();
                }}
                type="button"
                disabled={!sessionId || dbUiActions.isFetching}
                title="Query the optional SQLite troubleshooting DB (requires agentd --db-path)"
              >
                Load DB ui_actions
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => {
                  void dbMessages.refetch();
                }}
                type="button"
                disabled={!sessionId || dbMessages.isFetching}
                title="Query the optional SQLite troubleshooting DB (requires agentd --db-path)"
              >
                Load DB messages
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => {
                  void dbClientEvents.refetch();
                }}
                type="button"
                disabled={!sessionId || dbClientEvents.isFetching}
                title="Query the optional SQLite troubleshooting DB (requires agentd --db-path)"
              >
                Load DB client_events
              </button>
            </div>
            <div className="mt-3 max-h-80 overflow-auto rounded-md border border-white/10 bg-black/20">
              {(sessions.data?.sessions ?? []).map((sid) => (
                <button
                  key={sid}
                  className={`block w-full px-3 py-2 text-left text-sm hover:bg-white/5 ${
                    sid === sessionId ? "bg-white/10" : ""
                  }`}
                  onClick={() => setSessionId(sid)}
                  type="button"
                >
                  {sid}
                </button>
              ))}
            </div>
            <div className="mt-3 text-xs text-white/50">Audit entries: {audit.data?.entries ? audit.data.entries.length : 0}</div>
            <div className="mt-1 text-xs text-white/50">
              Client events:{" "}
              {sessionClientEvents.isFetching
                ? "loading…"
                : sessionClientEvents.isError
                  ? "failed"
                  : sessionClientEvents.data?.ok
                    ? `${sessionClientEvents.data?.count ?? 0}`
                    : "error"}
            </div>
            <div className="mt-1 text-xs text-white/50">
              Artifacts:{" "}
              {sessionArtifacts.isFetching
                ? "loading…"
                : sessionArtifacts.isError
                  ? "failed"
                  : sessionArtifacts.data?.ok
                    ? `${sessionArtifacts.data?.count ?? 0}`
                    : "error"}
            </div>
            <div className="mt-1 text-xs text-white/50">
              DB runs:{" "}
              {dbRuns.isFetching ? "loading…" : dbRuns.isError ? "failed" : dbRuns.data?.ok ? `${dbRuns.data?.count ?? 0}` : "(disabled)"}
            </div>
            <div className="mt-1 text-xs text-white/50">
              DB ui_actions:{" "}
              {dbUiActions.isFetching
                ? "loading…"
                : dbUiActions.isError
                  ? "failed"
                  : dbUiActions.data?.ok
                    ? `${dbUiActions.data?.count ?? 0}`
                    : "(disabled)"}
            </div>
            <div className="mt-1 text-xs text-white/50">
              DB messages:{" "}
              {dbMessages.isFetching
                ? "loading…"
                : dbMessages.isError
                  ? "failed"
                  : dbMessages.data?.ok
                    ? `${dbMessages.data?.count ?? 0}`
                    : "(disabled)"}
            </div>
            <div className="mt-1 text-xs text-white/50">
              DB client_events:{" "}
              {dbClientEvents.isFetching
                ? "loading…"
                : dbClientEvents.isError
                  ? "failed"
                  : dbClientEvents.data?.ok
                    ? `${dbClientEvents.data?.count ?? 0}`
                    : "(disabled)"}
            </div>
            <div className="mt-2 rounded-md border border-white/10 bg-black/10 p-2">
              <div className="flex items-center justify-between gap-2">
                <div className="text-xs font-semibold text-white/70">DB filters</div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  onClick={() => {
                    setSelectedDbRunId(null);
                    void dbRuns.refetch();
                  }}
                  type="button"
                  disabled={!sessionId || dbRuns.isFetching}
                  title="Re-run the DB query with the filters below"
                >
                  Apply
                </button>
              </div>
              <div className="mt-2 grid grid-cols-2 gap-2">
                <label className="flex items-center gap-2 text-[11px] text-white/70">
                  <input
                    type="checkbox"
                    checked={!!dbRunsOnlyErrors}
                    onChange={(e) => setDbRunsOnlyErrors(e.target.checked)}
                  />
                  Errors only
                </label>
                <div />
                <div className="col-span-2">
                  <Label>Stop reason (exact match)</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={dbRunsStopReason}
                    placeholder="e.g. max_steps_exceeded, repeated_tool_call_guard, max_tool_calls_for_tool_exceeded"
                    onChange={(e) => setDbRunsStopReason(e.target.value)}
                  />
                  <div className="mt-1 text-[11px] text-white/40">
                    Uses <code>/api/v1/db/runs?only_errors=...</code> and <code>stop_reason=...</code>.
                  </div>
                </div>
              </div>
            </div>
            <div className="mt-2 text-xs text-white/50">
              Tools:{" "}
              {toolsDefs.isFetching
                ? "loading…"
                : toolsDefs.isError
                  ? "failed"
                  : toolsDefs.data?.ok
                    ? `${toolsDefs.data?.count ?? 0}`
                    : "error"}
            </div>

            {sessionId &&
            sessionArtifacts.data?.ok &&
            Array.isArray(sessionArtifacts.data.artifacts) &&
            sessionArtifacts.data.artifacts.length > 0 ? (
              <div className="mt-3">
                <div className="text-xs font-semibold text-white/70">Artifacts (latest)</div>
                <div className="mt-2 grid gap-2">
                  {sessionArtifacts.data.artifacts
                    .slice(-6)
                    .reverse()
                    .map((a: any, idx: number) => {
                      const ts = typeof a?.ts_unix_ms === "number" ? new Date(a.ts_unix_ms).toISOString() : "";
                      const artifact = a?.data?.artifact ?? {};
                      const title = String(artifact?.title ?? artifact?.path ?? "artifact");
                      return (
                        <div key={idx} className="rounded-md border border-white/10 bg-black/10 p-2">
                          <div className="flex items-baseline justify-between gap-2">
                            <div className="text-[11px] text-white/50">{ts}</div>
                            <div className="text-[11px] text-white/40">{title}</div>
                          </div>
                          <div className="mt-2">
                            <ArtifactView
                              baseUrl={effectiveBase}
                              yolo={yolo}
                              artifact={artifact}
                              allowAutoplay={allowAutoplay}
                              sessionId={sessionId}
                              daemonAuthToken={daemonAuthToken}
                            />
                          </div>
                        </div>
                      );
                    })}
                </div>
              </div>
            ) : null}

            {dbRuns.data?.ok && Array.isArray(dbRuns.data.runs) ? (
              <div className="mt-3">
                <DbRunsView
                  runs={dbRuns.data.runs as any[]}
                  onSelectRunId={(runId) => {
                    setSelectedDbRunId(runId);
                  }}
                />
                {selectedDbRunId && dbRunDetail.isFetching ? (
                  <div className="mt-2 text-[11px] text-white/50">loading run {selectedDbRunId}…</div>
                ) : selectedDbRunId && dbRunDetail.data?.ok ? (
                  <div className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
                    <div className="flex items-center justify-between gap-2">
                      <div className="text-xs font-semibold text-white/70">Run {selectedDbRunId}</div>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                        type="button"
                        onClick={() => setSelectedDbRunId(null)}
                      >
                        Close
                      </button>
                    </div>
                    <div className="mt-2 text-[11px] text-white/50">
                      events={Array.isArray(dbRunDetail.data.events) ? dbRunDetail.data.events.length : 0},{" "}
                      tools={Array.isArray(dbRunDetail.data.tool_records) ? dbRunDetail.data.tool_records.length : 0},{" "}
                      artifacts={Array.isArray(dbRunDetail.data.artifacts) ? dbRunDetail.data.artifacts.length : 0},{" "}
                      ui_actions={Array.isArray(dbRunDetail.data.ui_actions) ? dbRunDetail.data.ui_actions.length : 0}
                    </div>
                    {Array.isArray(dbRunDetail.data.events) ? (
                      (() => {
                        const evs = dbRunDetail.data.events as any[];
                        const lastErr = [...evs].reverse().find((e) => e?.type === "error");
                        const reason = lastErr?.data?.reason ?? "";
                        const msg = lastErr?.data?.error ?? "";
                        if (!reason && !msg) return null;
                        return (
                          <div className="mt-2 text-[11px] text-amber-200/80">
                            last error: <code>{String(reason || msg)}</code>
                          </div>
                        );
                      })()
                    ) : null}
                    {(() => {
                      const run = (dbRunDetail.data.run ?? {}) as any;
                      const stopReason = String(run?.stop_reason ?? "");
                      const steps = typeof run?.steps_executed === "number" ? run.steps_executed : null;
                      const toolCalls = typeof run?.tool_calls_total === "number" ? run.tool_calls_total : null;
                      const byTool = run?.tool_calls_by_tool && typeof run.tool_calls_by_tool === "object" ? run.tool_calls_by_tool : null;
                      if (!stopReason && steps === null && toolCalls === null && !byTool) return null;
                      return (
                        <div className="mt-2 rounded-md border border-white/10 bg-black/10 p-2 text-[11px] text-white/70">
                          <div className="flex flex-wrap items-center gap-x-3 gap-y-1">
                            <div>
                              stop_reason: <code className="text-white/80">{stopReason || "(unknown)"}</code>
                            </div>
                            {typeof steps === "number" ? (
                              <div>
                                steps: <code className="text-white/80">{steps}</code>
                              </div>
                            ) : null}
                            {typeof toolCalls === "number" ? (
                              <div>
                                tool_calls: <code className="text-white/80">{toolCalls}</code>
                              </div>
                            ) : null}
                            {stopReason ? (
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                                type="button"
                                title="Set DB filter stop_reason and refetch"
                                onClick={() => {
                                  setDbRunsStopReason(stopReason);
                                  setDbRunsOnlyErrors(true);
                                  setSelectedDbRunId(null);
                                  void dbRuns.refetch();
                                }}
                              >
                                Filter by reason
                              </button>
                            ) : null}
                          </div>
                          {byTool ? (
                            <div className="mt-2">
                              <div className="text-[11px] text-white/50">tool_calls_by_tool</div>
                              <pre className="mt-1 max-h-[140px] overflow-auto whitespace-pre-wrap break-words text-[11px] text-white/70">
                                {JSON.stringify(byTool, null, 2)}
                              </pre>
                            </div>
                          ) : null}
                        </div>
                      );
                    })()}
                    <pre className="mt-2 max-h-[220px] overflow-auto whitespace-pre-wrap break-words text-[11px] text-white/70">
                      {JSON.stringify(dbRunDetail.data.run ?? {}, null, 2)}
                    </pre>
                    {Array.isArray(dbRunDetail.data.artifacts) && dbRunDetail.data.artifacts.length > 0 ? (
                      <div className="mt-2">
                        <div className="text-xs font-semibold text-white/70">Artifacts</div>
                        <div className="mt-2 grid gap-2">
                          {(dbRunDetail.data.artifacts as any[]).slice(0, 6).map((a, idx) => {
                            const artifact = a?.artifact ?? {
                              path: a?.path,
                              kind: a?.kind,
                              mime: a?.mime,
                              title: a?.title,
                              autoplay: a?.autoplay,
                              repeat: a?.repeat,
                            };
                            const title = String(artifact?.title ?? artifact?.path ?? "artifact");
                            return (
                              <div key={idx} className="rounded-md border border-white/10 bg-black/10 p-2">
                                <div className="text-[11px] text-white/50">{title}</div>
                                <div className="mt-2">
                                  <ArtifactView
                                    baseUrl={effectiveBase}
                                    yolo={yolo}
                                    artifact={artifact}
                                    allowAutoplay={allowAutoplay}
                                    sessionId={sessionId}
                                    daemonAuthToken={daemonAuthToken}
                                  />
                                </div>
                              </div>
                            );
                          })}
                        </div>
                      </div>
                    ) : null}
                    {Array.isArray(dbRunDetail.data.ui_actions) && dbRunDetail.data.ui_actions.length > 0 ? (
                      <div className="mt-2">
                        <DbUiActionsView uiActions={dbRunDetail.data.ui_actions as any[]} />
                      </div>
                    ) : null}
                  </div>
                ) : selectedDbRunId && dbRunDetail.isError ? (
                  <div className="mt-2 text-[11px] text-amber-200/80">failed to load run {selectedDbRunId}</div>
                ) : null}
              </div>
            ) : dbRuns.isFetching ? (
              <div className="mt-2 text-[11px] text-white/50">loading db runs…</div>
            ) : dbRuns.data && !dbRuns.data.ok ? (
              <div className="mt-2 text-[11px] text-white/40">db: {dbRuns.data.error ?? "(disabled)"}</div>
            ) : null}

            {dbUiActions.data?.ok && Array.isArray(dbUiActions.data.ui_actions) ? (
              <div className="mt-3">
                <DbUiActionsView
                  uiActions={dbUiActions.data.ui_actions as any[]}
                  onSelectRunId={(runId) => {
                    setSelectedDbRunId(runId);
                  }}
                />
              </div>
            ) : dbUiActions.isFetching ? (
              <div className="mt-2 text-[11px] text-white/50">loading db ui_actions…</div>
            ) : dbUiActions.data && !dbUiActions.data.ok ? (
              <div className="mt-2 text-[11px] text-white/40">db: {dbUiActions.data.error ?? "(disabled)"}</div>
            ) : null}

            {dbMessages.data?.ok && Array.isArray(dbMessages.data.messages) ? (
              <div className="mt-3">
                <DbMessagesView messages={dbMessages.data.messages as any[]} />
              </div>
            ) : dbMessages.isFetching ? (
              <div className="mt-2 text-[11px] text-white/50">loading db messages…</div>
            ) : dbMessages.data && !dbMessages.data.ok ? (
              <div className="mt-2 text-[11px] text-white/40">db: {dbMessages.data.error ?? "(disabled)"}</div>
            ) : null}

            {dbClientEvents.data?.ok && Array.isArray(dbClientEvents.data.client_events) ? (
              <div className="mt-3">
                <DbClientEventsView events={dbClientEvents.data.client_events as any[]} />
              </div>
            ) : dbClientEvents.isFetching ? (
              <div className="mt-2 text-[11px] text-white/50">loading db client_events…</div>
            ) : dbClientEvents.data && !dbClientEvents.data.ok ? (
              <div className="mt-2 text-[11px] text-white/40">db: {dbClientEvents.data.error ?? "(disabled)"}</div>
            ) : null}
          </div>
        ) : null}

        {showSettings ? (
            <div className="rounded-xl border border-white/10 bg-white/5 p-4">
              <div className="mb-3 text-sm font-semibold">Settings</div>

            <Label>Daemon base URL</Label>
            <input
              className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={base}
              onChange={(e) => setBase(e.target.value)}
            />
            {!/^https?:\/\//i.test(String(base || "").trim()) ? (
              <div className="mt-1 text-[11px] text-amber-200/80">
                Tip: include scheme (e.g. <code>http://127.0.0.1:8123</code>). Missing scheme will default to <code>http://</code>.
              </div>
            ) : null}

            <div className="mt-4">
              <Label>Daemon auth token (optional)</Label>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={daemonAuthToken}
                placeholder="sent as Authorization: Bearer <token>"
                onChange={(e) => setDaemonAuthToken(e.target.value)}
              />
              <div className="mt-1 text-[11px] text-white/40">
                Use when `agentd` is started with <code>--auth-token</code> (or env <code>AGENTD_AUTH_TOKEN</code>).
              </div>
            </div>

            <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
              <div className="flex items-center justify-between gap-3">
                <div className="text-xs font-semibold text-white/70">
                  Daemon config <span className="text-white/40">(GET /api/v1/config)</span>
                </div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  onClick={() => daemonConfig.refetch()}
                  type="button"
                  disabled={daemonConfig.isFetching}
                  title="Refetch daemon config"
                >
                  Refresh
                </button>
              </div>

              {daemonConfig.isFetching ? (
                <div className="mt-2 text-[11px] text-white/50">loading…</div>
              ) : daemonConfig.isError ? (
                <div className="mt-2 text-[11px] text-amber-200/80">failed to load config</div>
              ) : daemonConfig.data?.ok ? (
                <div className="mt-2 grid grid-cols-2 gap-2 text-[11px] text-white/60">
                  <div>
                    auth:{" "}
                    <code className="text-white/70">{daemonConfig.data.daemon?.auth_enabled ? "enabled" : "disabled"}</code>
                  </div>
                  <div>
                    cors:{" "}
                    <code className="text-white/70">{daemonConfig.data.cors?.enabled ? "enabled" : "disabled"}</code>
                  </div>
                  <div className="col-span-2">
                    cors origins:{" "}
                    <code className="text-white/70">
                      {daemonConfig.data.cors?.origins && daemonConfig.data.cors.origins.length > 0
                        ? daemonConfig.data.cors.origins.join(", ")
                        : "(none)"}
                    </code>
                  </div>
                  <div>
                    yolo default: <code className="text-white/70">{daemonConfig.data.sandbox?.yolo_default ? "true" : "false"}</code>
                  </div>
                  <div>
                    host policy:{" "}
                    <code className="text-white/70">{daemonConfig.data.sandbox?.host_policy ?? "(unknown)"}</code>
                  </div>
                  <div className="col-span-2">
                    tools root:{" "}
                    <code className="text-white/70">
                      {daemonConfig.data.sandbox?.tools_root ?? "(default / cwd)"}
                    </code>
                  </div>
                  {daemonConfig.data.daemon?.state_dir ? (
                    <div className="col-span-2">
                      state dir: <code className="text-white/70">{daemonConfig.data.daemon.state_dir}</code>
                    </div>
                  ) : null}
                  {daemonConfig.data.daemon?.sessions_root_dir ? (
                    <div className="col-span-2">
                      sessions root: <code className="text-white/70">{daemonConfig.data.daemon.sessions_root_dir}</code>
                    </div>
                  ) : null}
                  <div className="col-span-2">
                    db: <code className="text-white/70">{daemonConfig.data.daemon?.db_path ?? "(disabled)"}</code>
                  </div>
                  <div className="col-span-2">
                    run limits:{" "}
                    <code className="text-white/70">
                      max_steps_default={daemonConfig.data.daemon?.max_steps_default ?? "?"}, max_tool_calls_total_default=
                      {daemonConfig.data.daemon?.max_tool_calls_total_default ?? "?"}, tool_call_limits_default=
                      {(daemonConfig.data.daemon?.tool_call_limits_default ?? [])
                        .map((x) => `${x.tool}=${x.max_calls}`)
                        .join(", ") || "(none)"}
                    </code>
                  </div>
                  <div className="col-span-2">
                    job gc:{" "}
                    <code className="text-white/70">
                      ttl={daemonConfig.data.jobs?.job_ttl_ms ?? "?"}ms, max_jobs={daemonConfig.data.jobs?.max_jobs ?? "?"}
                    </code>
                  </div>
                </div>
              ) : (
                <div className="mt-2 text-[11px] text-amber-200/80">
                  {daemonConfig.data?.error ? `error: ${daemonConfig.data.error}` : "error"}
                </div>
              )}
            </div>

            <div className="mt-4 grid grid-cols-2 gap-3">
              <div>
                <Label>Session</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={sessionId}
                  onChange={(e) => setSessionId(e.target.value)}
                />
              </div>
              <div>
                <Label>Tools</Label>
                <select
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={tools}
                  onChange={(e) => setTools(e.target.value as any)}
                >
                  <option value="host">host</option>
                  <option value="basic">basic</option>
                  <option value="none">none</option>
                </select>
              </div>
              <div>
                <Label>Host policy</Label>
                <select
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={hostPolicy}
                  onChange={(e) => setHostPolicy(e.target.value as any)}
                  disabled={tools !== "host"}
                  title={tools !== "host" ? "Only applies when tools=host" : undefined}
                >
                  <option value="full">full</option>
                  <option value="readonly">readonly</option>
                </select>
                {toolsDefs.data?.effective_host_policy ? (
                  <div className="mt-1 text-[11px] text-white/40">
                    effective: <code>{toolsDefs.data.effective_host_policy}</code>
                  </div>
                ) : null}
              </div>
              <div className="col-span-2 rounded-md border border-white/10 bg-black/20 p-3">
                <div className="flex items-center justify-between gap-3">
                  <div className="text-xs font-semibold text-white/70">Agent media</div>
                  <label className="flex items-center gap-2 text-[11px] text-white/70">
                    <input
                      type="checkbox"
                      checked={allowAutoplay}
                      onChange={(e) => setAllowAutoplay(e.target.checked)}
                    />
                    Allow agent-requested audio autoplay
                  </label>
                </div>
                <div className="mt-1 text-[11px] text-white/40">
                  When enabled and the agent emits an <code>artifact</code> event with <code>autoplay=true</code>, the UI will try to play the audio.
                  Browsers may still block autoplay; you can always click “Play”.
                </div>
              </div>
              <div>
                <Label>Tools root</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={toolsRoot}
                  onChange={(e) => setToolsRoot(e.target.value)}
                  disabled={yolo}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Supports special values: <code>@host</code> (daemon host scope), <code>@cwd</code> (unrestricted).
                </div>
              </div>
              <div>
                <Label>Max steps (blank=daemon default; 0=∞)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxSteps}
                  placeholder={String(daemonConfig.data?.daemon?.max_steps_default ?? "32")}
                  onChange={(e) => {
                    setMaxStepsUserSet(true);
                    setMaxSteps(e.target.value);
                  }}
                />
                {String(maxSteps).trim() === "0" ? (
                  <div className="mt-1 text-[11px] text-amber-200/80">
                    Unlimited steps can loop forever. Prefer blank (daemon default) or a small cap (e.g. 16–64).
                  </div>
                ) : null}
              </div>
              <div>
                <Label>Max repeated tool calls</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxRepeatedToolCalls}
                  onChange={(e) => setMaxRepeatedToolCalls(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Stops runaway loops when the model repeats the exact same tool call. Set <code>0</code> to disable.
                </div>
              </div>
              <div>
                <Label>Max tool calls total (blank=daemon default; 0=∞)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxToolCallsTotal}
                  placeholder={String(daemonConfig.data?.daemon?.max_tool_calls_total_default ?? "128")}
                  onChange={(e) => setMaxToolCallsTotal(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Caps total tool calls even if a single model step requests many tools.
                </div>
              </div>
              <div>
                <Label>Max tool calls per tool (blank=daemon default; 0=∞)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxToolCallsPerTool}
                  placeholder={String(daemonConfig.data?.daemon?.max_tool_calls_per_tool_default ?? "0")}
                  onChange={(e) => setMaxToolCallsPerTool(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Caps tool calls per tool name, catching loops with varying args that bypass the exact-repeat guard.
                </div>
              </div>
              <div className="col-span-2">
                <Label>Tool call limits (blank=daemon default)</Label>
                <textarea
                  className="mt-1 h-20 w-full resize-none rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={toolCallLimits}
                  placeholder={(daemonConfig.data?.daemon?.tool_call_limits_default ?? [])
                    .map((x) => `${x.tool}=${x.max_calls}`)
                    .join("\n")}
                  onChange={(e) => setToolCallLimits(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Repeatable per-tool caps (one per line, or comma-separated): <code>proc_exec=4</code>. Set <code>=0</code> to disable for a tool.
                </div>
              </div>
              <div>
                <Label>Max chars</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxChars}
                  onChange={(e) => setMaxChars(e.target.value)}
                />
              </div>
              <div>
                <Label>Keep last</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={keepLast}
                  onChange={(e) => setKeepLast(e.target.value)}
                />
              </div>
              <div className="col-span-2">
                <Label>LLM Base URL</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={baseUrl}
                  onChange={(e) => setBaseUrl(e.target.value)}
                />
              </div>
              <div>
                <Label>Model</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={model}
                  onChange={(e) => setModel(e.target.value)}
                />
              </div>
              <div>
                <Label>Summary model (tools=none)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={summaryModel}
                  placeholder="optional (e.g. gpt-4o-mini or deepseek-chat)"
                  onChange={(e) => setSummaryModel(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Used to summarize dropped messages when compaction triggers (host inserts a system summary).
                </div>
              </div>
              <div>
                <Label>API key (optional)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={apiKey}
                  placeholder="leave empty to use daemon env"
                  onChange={(e) => setApiKey(e.target.value)}
                />
              </div>
              <div>
                <Label>Summary max chars</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={summaryMaxChars}
                  onChange={(e) => setSummaryMaxChars(e.target.value)}
                />
              </div>
              <div className="col-span-2">
                <Label>HTTPS proxy (optional)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={proxyUrl}
                  placeholder="e.g. http://localhost:8120 (leave empty to use daemon env)"
                  onChange={(e) => setProxyUrl(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  If outbound networking requires a proxy, set it here to avoid “hangs”.
                </div>
              </div>
              <div>
                <Label>Timeout (ms)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={timeoutMs}
                  onChange={(e) => setTimeoutMs(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Provider HTTP timeout (daemon passes through to libcurl).
                </div>
              </div>
              <div>
                <Label>Max capture bytes</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxCaptureBytes}
                  onChange={(e) => setMaxCaptureBytes(e.target.value)}
                />
                <div className="mt-1 text-[11px] text-white/40">
                  Caps verbose event payloads (`llm_request`/`llm_response`/tool output) for UI stability. Full transcript is in `trace_text`.
                </div>
              </div>
            </div>

            <div className="mt-4 flex items-center gap-2">
              <input id="trace" type="checkbox" checked={trace} onChange={(e) => setTrace(e.target.checked)} />
              <label htmlFor="trace" className="text-sm text-white/70">
                Include transcript
              </label>
            </div>

            <div className="mt-3 flex items-center gap-2">
              <input id="yolo" type="checkbox" checked={yolo} onChange={(e) => setYolo(e.target.checked)} />
              <label htmlFor="yolo" className="text-sm text-white/70">
                YOLO (no tool restrictions)
              </label>
            </div>

            <div className="mt-3 flex items-center gap-2">
              <input id="verbose" type="checkbox" checked={verbose} onChange={(e) => setVerbose(e.target.checked)} />
              <label htmlFor="verbose" className="text-sm text-white/70">
                Verbose (capture tool output + LLM request/response)
              </label>
            </div>

            <div className="mt-3 flex items-center gap-2">
              <input
                id="showDebugInConversation"
                type="checkbox"
                checked={showDebugInConversation}
                onChange={(e) => setShowDebugInConversation(e.target.checked)}
              />
              <label htmlFor="showDebugInConversation" className="text-sm text-white/70">
                Show debug events in conversation
              </label>
            </div>

            <div className="mt-3 flex items-center gap-2">
              <input id="async" type="checkbox" checked={useAsync} onChange={(e) => setUseAsync(e.target.checked)} />
              <label htmlFor="async" className="text-sm text-white/70">
                Async run (poll jobs)
              </label>
            </div>

            <div className="mt-3 flex items-center gap-2">
              <input
                id="streamAssistant"
                type="checkbox"
                checked={streamAssistant}
                onChange={(e) => setStreamAssistant(e.target.checked)}
              />
              <label htmlFor="streamAssistant" className="text-sm text-white/70">
                Stream assistant tokens (tools=none)
              </label>
            </div>

            <div className="mt-3 text-[11px] text-white/40">
              Settings are persisted in this browser tab via <code>localStorage</code>.
            </div>

            {toolsDefs.data?.ok && Array.isArray(toolsDefs.data.defs) && toolsDefs.data.defs.length > 0 ? (
              <div className="mt-4">
                <div className="mb-2 text-xs font-semibold text-white/70">Active tool schemas</div>
                <div className="max-h-56 overflow-auto rounded-md border border-white/10 bg-black/20 p-2 text-xs text-white/80">
                  {toolsDefs.data.defs.map((d) => (
                    <div key={d.name} className="border-b border-white/5 py-2 last:border-b-0">
                      <div className="font-semibold">{d.name}</div>
                      {d.description ? <div className="mt-1 text-white/60">{d.description}</div> : null}
                      {d.parameters_json ? (
                        <pre className="mt-2 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-2 text-[11px] leading-relaxed text-white/80">
                          {d.parameters_json}
                        </pre>
                      ) : null}
                    </div>
                  ))}
                </div>
              </div>
            ) : null}

            <div className="mt-4">
              <div className="mb-2 text-xs font-semibold text-white/70">OpenRouter model picker</div>
              <div className="grid grid-cols-2 gap-3">
                <div>
                  <Label>Min total $/1M</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={orMinTotal}
                    onChange={(e) => setOrMinTotal(e.target.value)}
                  />
                </div>
                <div>
                  <Label>Max total $/1M</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={orMaxTotal}
                    onChange={(e) => setOrMaxTotal(e.target.value)}
                  />
                </div>
                <div>
                  <Label>Limit</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={orLimit}
                    onChange={(e) => setOrLimit(e.target.value)}
                  />
                </div>
                <div className="mt-6 flex items-center gap-2">
                  <input
                    id="orRequireMultimodal"
                    type="checkbox"
                    checked={orRequireMultimodal}
                    onChange={(e) => setOrRequireMultimodal(e.target.checked)}
                  />
                  <label htmlFor="orRequireMultimodal" className="text-sm text-white/70">
                    Require multimodal input
                  </label>
                </div>
                <div className="col-span-2 flex items-center gap-2">
                  <input
                    id="orRequireTools"
                    type="checkbox"
                    checked={orRequireTools}
                    onChange={(e) => setOrRequireTools(e.target.checked)}
                  />
                  <label htmlFor="orRequireTools" className="text-sm text-white/70">
                    Require tools
                  </label>
                </div>
              </div>

              <button
                className="mt-3 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                onClick={() => fetchOpenRouterModels.mutate()}
                disabled={fetchOpenRouterModels.isPending}
                type="button"
              >
                {fetchOpenRouterModels.isPending ? "Fetching…" : "Fetch OpenRouter models"}
              </button>

              {fetchOpenRouterModels.isError ? (
                <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                  Failed: {String(fetchOpenRouterModels.error)}
                </div>
              ) : null}

              {openrouterModels?.ok && Array.isArray(openrouterModels.models) ? (
                <div className="mt-3 max-h-72 overflow-auto rounded-md border border-white/10 bg-black/20">
                  <div className="sticky top-0 grid grid-cols-5 gap-2 border-b border-white/10 bg-black/40 px-3 py-2 text-[11px] font-semibold text-white/70">
                    <div>Total</div>
                    <div>Prompt</div>
                    <div>Compl</div>
                    <div>Ctx</div>
                    <div>Model</div>
                  </div>
                  {openrouterModels.models.map((m: any) => (
                    <div key={m.id} className="grid grid-cols-5 gap-2 px-3 py-2 text-[11px] text-white/80 hover:bg-white/5">
                      <div>{typeof m.total_usd_per_million === "number" ? m.total_usd_per_million.toFixed(3) : ""}</div>
                      <div>{typeof m.prompt_usd_per_million === "number" ? m.prompt_usd_per_million.toFixed(3) : ""}</div>
                      <div>
                        {typeof m.completion_usd_per_million === "number" ? m.completion_usd_per_million.toFixed(3) : ""}
                      </div>
                      <div>{m.context_length ?? ""}</div>
                      <div className="flex items-center justify-between gap-2">
                        <div className="truncate font-mono">{m.id}</div>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/80 hover:bg-black/40"
                          onClick={() => {
                            setModel(String(m.id || ""));
                            setBaseUrl("https://openrouter.ai/api/v1");
                          }}
                          type="button"
                        >
                          Use
                        </button>
                      </div>
                    </div>
                  ))}
                </div>
              ) : openrouterModels && !openrouterModels.ok ? (
                <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                  {openrouterModels.error || "OpenRouter models fetch failed"}
                </div>
              ) : null}

              <div className="mt-2 text-[11px] text-white/40">
                Uses the daemon endpoint <code>/api/v1/openrouter/models</code>. If you set an API key above, the UI will send it as an
                <code>X-OpenRouter-Key</code> header. Daemon auth (if enabled) is sent separately as <code>Authorization: Bearer ...</code>.
              </div>
            </div>
          </div>
        ) : null}

        <div className={`rounded-xl border border-white/10 bg-white/5 p-4 ${showSettings ? "md:col-span-1" : "md:col-span-3"}`}>
          <div className="mb-3 text-sm font-semibold">Prompt</div>
          <textarea
            className="mt-1 h-32 w-full resize-none rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={prompt}
            onChange={(e) => setPrompt(e.target.value)}
          />

          <button
            className="mt-4 w-full rounded-md bg-indigo-500 px-3 py-2 text-sm font-semibold text-white hover:bg-indigo-400 disabled:opacity-50"
            onClick={() => run.mutate()}
            disabled={run.isPending || !!activeJobId}
            type="button"
          >
            {run.isPending || activeJobId ? "Running…" : "Run"}
          </button>

          {run.isError ? (
            <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-sm text-rose-200">
              Failed: {String(run.error)}
            </div>
          ) : null}

          {activeJobId ? (
            <div className="mt-3 rounded-md border border-white/10 bg-black/20 px-3 py-2 text-xs text-white/70">
              <div>
                Job: <span className="text-white/90">{activeJobId}</span>
              </div>
              <div>
                Status: <span className="text-white/90">{jobStatus ?? "running"}</span>
              </div>
              {jobUpdatedMs ? <div>Updated: {new Date(jobUpdatedMs).toISOString()}</div> : null}
              <div className="mt-2 flex gap-2">
                <button
                  className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200 hover:bg-rose-500/15"
                  onClick={async () => {
                    try {
                      await apiCancelJob(effectiveBase, activeJobId, daemonAuthToken);
                      setJobError("cancel requested");
                    } catch (e) {
                      setJobError(`cancel failed: ${String(e)}`);
                    }
                  }}
                  type="button"
                >
                  Cancel job
                </button>
              </div>
            </div>
          ) : null}
          {jobError ? (
            <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-sm text-rose-200">
              {jobError}
            </div>
          ) : null}
        </div>
      </div>

      <div className="mt-6 grid gap-4">
        <div className="rounded-xl border border-white/10 bg-white/5 p-4">
          <div className="mb-2 flex items-center justify-between gap-2">
            <div className="text-sm font-semibold">Assistant</div>
            {activeJobId ? (
              <div className="text-xs text-white/60">
                Active job: <span className="text-white/80">{activeJobId}</span> ({jobStatus ?? "running"})
              </div>
            ) : null}
          </div>

          {result?.assistant_text ? (
            <div>
              <div className="mb-2 text-xs font-semibold text-white/70">
                {activeJobId ? "Last completed" : "Latest"}
              </div>
              <Markdown text={result.assistant_text} />
            </div>
          ) : (
            <div className="text-sm text-white/60">
              {activeJobId ? `Running… (${jobStatus ?? "running"})` : run.isPending ? "Starting…" : "(no output yet)"}
            </div>
          )}
          {!result?.ok && result?.error ? (
            <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-sm text-rose-200">
              {result.error}
            </div>
          ) : null}
          {result?.http_status ? (
            <div className="mt-3 text-xs text-white/50">HTTP status: {result.http_status}</div>
          ) : null}
          {typeof result?.effective_yolo === "boolean" ? (
            <div className="mt-1 text-xs text-white/50">
              Effective: yolo={String(result.effective_yolo)} tools_root={result.effective_tools_root ?? ""}
            </div>
          ) : null}
        </div>

        {activeJobId ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">Conversation (live)</div>
            {liveEvents.length === 0 ? (
              <div className="mb-2 rounded-md border border-white/10 bg-black/20 px-3 py-2 text-xs text-white/70">
                Waiting for live events from the daemon (SSE/polling). If this stays empty, check the daemon base URL and network proxy.
              </div>
            ) : null}
            <ConversationView
              baseUrl={effectiveBase}
              yolo={yolo}
              sessionId={sessionId}
              client={client}
              daemonAuthToken={daemonAuthToken}
              prompt={lastRunPrompt || prompt}
              events={liveEvents}
              showDebugEvents={showDebugInConversation}
              allowAutoplay={allowAutoplay}
            />
          </div>
        ) : null}

        {result?.events && result.events.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">{activeJobId ? "Conversation (last completed)" : "Conversation"}</div>
            <ConversationView
              baseUrl={effectiveBase}
              yolo={yolo}
              sessionId={sessionId}
              client={client}
              daemonAuthToken={daemonAuthToken}
              prompt={lastCompletedPrompt || prompt}
              events={result.events}
              showDebugEvents={showDebugInConversation}
              allowAutoplay={allowAutoplay}
            />
          </div>
        ) : null}

        {activeJobId && liveEvents.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">Live Events</div>
            <EventTimeline baseUrl={effectiveBase} yolo={yolo} events={liveEvents} />
          </div>
        ) : null}

        {result?.events && result.events.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">{activeJobId ? "Events (last completed)" : "Events"}</div>
            <EventTimeline baseUrl={effectiveBase} yolo={yolo} events={result.events} />
          </div>
        ) : null}

        {audit.data?.entries && audit.data.entries.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">Session Audit (latest)</div>
            <div className="grid gap-3">
              {audit.data.entries
                .slice(-10)
                .reverse()
                .map((e: any, idx: number) => (
                  <div key={idx} className="rounded-lg border border-white/10 bg-white/5 p-3">
                    <div className="text-xs text-white/50">
                      {typeof e.ts_unix_ms === "number" ? new Date(e.ts_unix_ms).toISOString() : ""}
                    </div>
                    {typeof e.prompt === "string" ? (
                      <div className="mt-2">
                        <div className="text-xs font-semibold text-white/70">Prompt</div>
                        <pre className="mt-1 whitespace-pre-wrap text-xs text-white/80">{e.prompt}</pre>
                      </div>
                    ) : null}
                    {typeof e.assistant_text === "string" ? (
                      <div className="mt-2">
                        <div className="text-xs font-semibold text-white/70">Assistant</div>
                        <div className="mt-1">
                          <Markdown text={e.assistant_text} />
                        </div>
                      </div>
                    ) : null}
                    {Array.isArray(e.events) ? (
                      <div className="mt-3">
                        <div className="text-xs font-semibold text-white/70">Conversation</div>
                        <div className="mt-2">
                          <ConversationView
                            baseUrl={effectiveBase}
                            yolo={yolo}
                            sessionId={sessionId}
                            client={client}
                            daemonAuthToken={daemonAuthToken}
                            prompt={String(e.prompt ?? "")}
                            events={e.events}
                            showDebugEvents={showDebugInConversation}
                            allowAutoplay={allowAutoplay}
                          />
                        </div>
                        <div className="mt-3 text-xs font-semibold text-white/70">Events (raw)</div>
                        <div className="mt-2">
                          <EventTimeline baseUrl={effectiveBase} yolo={yolo} events={e.events} />
                        </div>
                      </div>
                    ) : null}
                  </div>
                ))}
            </div>
          </div>
        ) : null}

        {sessionClientEvents.data?.ok &&
        Array.isArray(sessionClientEvents.data.events) &&
        sessionClientEvents.data.events.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">Session Client Events (latest)</div>
            <div className="grid gap-3">
              {sessionClientEvents.data.events
                .slice(-20)
                .reverse()
                .map((e: any, idx: number) => (
                  <div key={idx} className="rounded-lg border border-white/10 bg-white/5 p-3">
                    <div className="text-xs text-white/50">
                      {typeof e.ts_unix_ms === "number" ? new Date(e.ts_unix_ms).toISOString() : ""}
                    </div>
                    <div className="mt-2">
                      <pre className="max-h-[220px] overflow-auto whitespace-pre-wrap break-words text-xs text-white/80">
                        {JSON.stringify(e, null, 2)}
                      </pre>
                    </div>
                  </div>
                ))}
            </div>
          </div>
        ) : null}

        {result?.trace_text ? <TraceView trace={result.trace_text} /> : null}
      </div>
    </div>
  );
}
