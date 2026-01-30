import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiGetAudit,
  apiGetHealth,
  apiGetJobProgress,
  apiGetOpenRouterModels,
  apiGetTools,
  apiCancelJob,
  apiListSessions,
  apiRun,
  apiRunAsync,
  RunRequest,
  RunResponse,
  type AgentEvent,
} from "./api";
import TraceView from "./components/TraceView";
import EventTimeline from "./components/EventTimeline";
import Markdown from "./components/Markdown";
import ConversationView from "./components/ConversationView";
import useLocalStorageState from "./hooks/useLocalStorageState";

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
  const [maxSteps, setMaxSteps] = useLocalStorageState("agentui.maxSteps", "0");
  const [maxChars, setMaxChars] = useLocalStorageState("agentui.maxChars", "20000");
  const [keepLast, setKeepLast] = useLocalStorageState("agentui.keepLast", "16");
  const [trace, setTrace] = useLocalStorageState("agentui.trace", true);
  const [useAsync, setUseAsync] = useLocalStorageState("agentui.useAsync", true);
  const [showDebugInConversation, setShowDebugInConversation] = useLocalStorageState(
    "agentui.showDebugInConversation",
    false,
  );
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

  const sessions = useQuery({
    queryKey: ["sessions", effectiveBase, daemonAuthToken],
    queryFn: () => apiListSessions(effectiveBase, daemonAuthToken),
    retry: 1,
  });

  const toolsDefs = useQuery({
    queryKey: ["tools", effectiveBase, daemonAuthToken, tools, toolsRoot, yolo, hostPolicy],
    queryFn: () =>
      apiGetTools(effectiveBase, daemonAuthToken, { tools, toolsRoot, yolo, hostPolicy: tools === "host" ? hostPolicy : undefined }),
    retry: 1,
  });

  const audit = useQuery({
    queryKey: ["audit", effectiveBase, daemonAuthToken, sessionId],
    queryFn: () => apiGetAudit(effectiveBase, sessionId, daemonAuthToken),
    enabled: !!sessionId,
    retry: 1,
  });

  const [result, setResult] = React.useState<RunResponse | undefined>(undefined);
  const [openrouterModels, setOpenrouterModels] = React.useState<any | null>(null);

  const run = useMutation({
    mutationFn: async () => {
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
        max_steps: Number.isFinite(Number(maxSteps)) ? Number(maxSteps) : 0,
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
    let opened = false;
    const trySse =
      typeof EventSource !== "undefined" &&
      typeof effectiveBase === "string" &&
      (effectiveBase.startsWith("http://") || effectiveBase.startsWith("https://"));

    let fallbackStarted = false;
    const fallbackToPolling = () => {
      if (fallbackStarted) return;
      fallbackStarted = true;
      startPolling();
    };

    if (trySse) {
      try {
        const url = `${effectiveBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
          String(cursorRef.current),
        )}`;
        es = new EventSource(url);
        setJobStatus("running");
        setJobUpdatedMs(null);
        es.onopen = () => {
          opened = true;
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
                <Label>Max steps (0=∞)</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={maxSteps}
                  onChange={(e) => setMaxSteps(e.target.value)}
                />
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
              prompt={lastRunPrompt || prompt}
              events={liveEvents}
              showDebugEvents={showDebugInConversation}
            />
          </div>
        ) : null}

        {result?.events && result.events.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">{activeJobId ? "Conversation (last completed)" : "Conversation"}</div>
            <ConversationView
              baseUrl={effectiveBase}
              yolo={yolo}
              prompt={lastCompletedPrompt || prompt}
              events={result.events}
              showDebugEvents={showDebugInConversation}
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
                            prompt={String(e.prompt ?? "")}
                            events={e.events}
                            showDebugEvents={showDebugInConversation}
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

        {result?.trace_text ? <TraceView trace={result.trace_text} /> : null}
      </div>
    </div>
  );
}
