import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import { apiGetAudit, apiGetHealth, apiGetSession, apiListSessions, apiRun, RunRequest, RunResponse } from "./api";
import TraceView from "./components/TraceView";
import EventTimeline from "./components/EventTimeline";
import Markdown from "./components/Markdown";

function Label({ children }: { children: React.ReactNode }) {
  return <div className="text-xs font-medium text-white/70">{children}</div>;
}

export default function App() {
  const [base, setBase] = React.useState("http://127.0.0.1:8123");
  const [prompt, setPrompt] = React.useState("inspect this project and explain");
  const [sessionId, setSessionId] = React.useState("default");
  const [tools, setTools] = React.useState<"host" | "basic" | "none">("host");
  const [toolsRoot, setToolsRoot] = React.useState(".");
  const [yolo, setYolo] = React.useState(true);
  const [verbose, setVerbose] = React.useState(false);
  const [model, setModel] = React.useState("deepseek-chat");
  const [baseUrl, setBaseUrl] = React.useState("https://api.deepseek.com");
  const [apiKey, setApiKey] = React.useState("");
  const [maxSteps, setMaxSteps] = React.useState("0");
  const [trace, setTrace] = React.useState(true);

  const health = useQuery({
    queryKey: ["health", base],
    queryFn: () => apiGetHealth(base),
    retry: 1,
  });

  const sessions = useQuery({
    queryKey: ["sessions", base],
    queryFn: () => apiListSessions(base),
    retry: 1,
  });

  const audit = useQuery({
    queryKey: ["audit", base, sessionId],
    queryFn: () => apiGetAudit(base, sessionId),
    enabled: !!sessionId,
    retry: 1,
  });

  const run = useMutation({
    mutationFn: async () => {
      const req: RunRequest = {
        prompt,
        session_id: sessionId || undefined,
        no_session: false,
        tools,
        tools_root: toolsRoot,
        yolo,
        verbose,
        model: model || undefined,
        base_url: baseUrl || undefined,
        api_key: apiKey || undefined,
        max_steps: Number.isFinite(Number(maxSteps)) ? Number(maxSteps) : 0,
        trace,
      };
      return apiRun(base, req);
    },
  });

  const result: RunResponse | undefined = run.data;

  return (
    <div className="mx-auto max-w-5xl px-6 py-8">
      <div className="mb-6 flex items-baseline justify-between">
        <div>
          <div className="text-xl font-semibold">agent UI</div>
          <div className="text-sm text-white/60">
            daemon:{" "}
            {health.isSuccess ? (
              <span className="text-emerald-300">
                ok ({health.data.service ?? "agentd"} {health.data.version ?? ""})
              </span>
            ) : health.isFetching ? (
              <span className="text-white/60">checking…</span>
            ) : (
              <span className="text-rose-300">offline</span>
            )}
          </div>
        </div>
      </div>

      <div className="grid gap-4 md:grid-cols-3">
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
          <div className="mt-3 text-xs text-white/50">
            Audit entries: {audit.data?.entries ? audit.data.entries.length : 0}
          </div>
        </div>

        <div className="rounded-xl border border-white/10 bg-white/5 p-4">
          <div className="mb-3 text-sm font-semibold">Connection</div>
          <Label>Daemon base URL</Label>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={base}
            onChange={(e) => setBase(e.target.value)}
          />

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
        </div>

        <div className="rounded-xl border border-white/10 bg-white/5 p-4 md:col-span-1">
          <div className="mb-3 text-sm font-semibold">LLM backend</div>
          <div className="grid grid-cols-2 gap-3">
            <div className="col-span-2">
              <Label>Base URL</Label>
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
              <Label>API key (optional)</Label>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={apiKey}
                placeholder="leave empty to use daemon env"
                onChange={(e) => setApiKey(e.target.value)}
              />
            </div>
          </div>

          <div className="mt-4">
            <Label>Prompt</Label>
            <textarea
              className="mt-1 h-32 w-full resize-none rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={prompt}
              onChange={(e) => setPrompt(e.target.value)}
            />
          </div>

          <button
            className="mt-4 w-full rounded-md bg-indigo-500 px-3 py-2 text-sm font-semibold text-white hover:bg-indigo-400 disabled:opacity-50"
            onClick={() => run.mutate()}
            disabled={run.isPending}
            type="button"
          >
            {run.isPending ? "Running…" : "Run"}
          </button>

          {run.isError ? (
            <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-sm text-rose-200">
              Failed: {String(run.error)}
            </div>
          ) : null}
        </div>
      </div>

      <div className="mt-6 grid gap-4">
        <div className="rounded-xl border border-white/10 bg-white/5 p-4">
          <div className="mb-2 text-sm font-semibold">Assistant</div>
          {result?.assistant_text ? (
            <Markdown text={result.assistant_text} />
          ) : (
            <div className="text-sm text-white/60">{run.isPending ? "" : "(no output yet)"}</div>
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

        {result?.events && result.events.length > 0 ? (
          <div>
            <div className="mb-2 text-sm font-semibold text-white/80">Events</div>
            <EventTimeline baseUrl={base} yolo={yolo} events={result.events} />
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
                        <div className="text-xs font-semibold text-white/70">Events</div>
                        <div className="mt-2">
                          <EventTimeline baseUrl={base} yolo={yolo} events={e.events} />
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
