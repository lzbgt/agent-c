import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import { apiCancelWorkflow, apiGetWorkflow, apiListWorkflows, type WorkflowDetailResp, type WorkflowListResp } from "../api";
import type { ApiAuth } from "../api/auth";
import useLocalStorageState from "../hooks/useLocalStorageState";
import WorkflowComposer from "./WorkflowComposer";

type WorkflowPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth?: ApiAuth;
  authKey?: string;
  clientId?: string;
  onTraceIdClick?: (traceId: string) => void;
  workflowDefaults?: Record<string, any>;
  workflowTargets?: string[];
  workflowBearerEnv?: string;
};

type WorkflowTask = {
  task_id: string;
  status?: string;
  depends_on: string[];
  allow_error?: boolean;
  attempt?: number;
  max_attempts?: number;
  error?: string;
  ready_unix_ms?: number;
  started_unix_ms?: number;
  finished_unix_ms?: number;
};

const STATUS_OPTIONS = ["running", "queued", "active", "done", "error", "cancelled", "all"];

function normalizeTask(raw: any): WorkflowTask | null {
  if (!raw || typeof raw !== "object") return null;
  const taskId = typeof raw.task_id === "string" ? raw.task_id : "";
  if (!taskId) return null;
  const deps = Array.isArray(raw.depends_on) ? raw.depends_on.filter((d: any) => typeof d === "string") : [];
  return {
    task_id: taskId,
    status: typeof raw.status === "string" ? raw.status : undefined,
    depends_on: deps,
    allow_error: typeof raw.allow_error === "boolean" ? raw.allow_error : undefined,
    attempt: typeof raw.attempt === "number" ? raw.attempt : undefined,
    max_attempts: typeof raw.max_attempts === "number" ? raw.max_attempts : undefined,
    error: typeof raw.error === "string" ? raw.error : undefined,
    ready_unix_ms: typeof raw.ready_unix_ms === "number" ? raw.ready_unix_ms : undefined,
    started_unix_ms: typeof raw.started_unix_ms === "number" ? raw.started_unix_ms : undefined,
    finished_unix_ms: typeof raw.finished_unix_ms === "number" ? raw.finished_unix_ms : undefined,
  };
}

function formatUnixMs(ms?: number): string {
  if (!ms || !Number.isFinite(ms)) return "—";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
}

function statusBadge(status?: string) {
  const s = String(status || "").toLowerCase();
  if (s === "done") return "bg-emerald-500/15 text-emerald-200 border-emerald-500/30";
  if (s === "running") return "bg-sky-500/15 text-sky-200 border-sky-500/30";
  if (s === "queued") return "bg-amber-500/15 text-amber-200 border-amber-500/30";
  if (s === "error") return "bg-rose-500/15 text-rose-200 border-rose-500/30";
  if (s === "cancelled") return "bg-slate-500/20 text-slate-200 border-slate-500/30";
  return "bg-white/10 text-white/70 border-white/10";
}

function canCancelStatus(status?: string) {
  const s = String(status || "").toLowerCase();
  return s === "running" || s === "queued";
}

function buildLevels(tasks: WorkflowTask[]) {
  const ids = new Set(tasks.map((t) => t.task_id));
  const depsMap = new Map<string, string[]>();
  const missingDeps = new Set<string>();
  for (const task of tasks) {
    const deps = task.depends_on.filter((d) => ids.has(d));
    for (const d of task.depends_on) {
      if (!ids.has(d)) missingDeps.add(d);
    }
    depsMap.set(task.task_id, deps);
  }

  const levels = new Map<string, number>();
  const visiting = new Set<string>();
  let hasCycle = false;

  const computeLevel = (id: string): number => {
    if (levels.has(id)) return levels.get(id) ?? 0;
    if (visiting.has(id)) {
      hasCycle = true;
      return 0;
    }
    visiting.add(id);
    const deps = depsMap.get(id) ?? [];
    let maxDep = -1;
    for (const dep of deps) {
      maxDep = Math.max(maxDep, computeLevel(dep));
    }
    visiting.delete(id);
    const level = maxDep + 1;
    levels.set(id, level);
    return level;
  };

  tasks.forEach((t) => computeLevel(t.task_id));

  const buckets = new Map<number, WorkflowTask[]>();
  for (const task of tasks) {
    const lvl = levels.get(task.task_id) ?? 0;
    const arr = buckets.get(lvl) ?? [];
    arr.push(task);
    buckets.set(lvl, arr);
  }
  const maxLevel = Math.max(0, ...Array.from(buckets.keys()));
  const orderedLevels = [];
  for (let i = 0; i <= maxLevel; i += 1) {
    const arr = buckets.get(i) ?? [];
    arr.sort((a, b) => a.task_id.localeCompare(b.task_id));
    orderedLevels.push(arr);
  }

  return { levels: orderedLevels, hasCycle, missingDeps: Array.from(missingDeps).sort() };
}

function extractWorkflows(resp?: WorkflowListResp | null) {
  if (!resp || !resp.ok || !Array.isArray(resp.workflows)) return [];
  return resp.workflows.filter((wf: any) => wf && typeof wf === "object");
}

function extractTasks(resp?: WorkflowDetailResp | null): WorkflowTask[] {
  if (!resp || !Array.isArray(resp.tasks)) return [];
  const out: WorkflowTask[] = [];
  for (const t of resp.tasks) {
    const norm = normalizeTask(t);
    if (norm) out.push(norm);
  }
  return out;
}

function extractWorkflowSummary(resp?: WorkflowDetailResp | null): Record<string, any> {
  if (!resp || !resp.workflow || typeof resp.workflow !== "object") return {};
  return resp.workflow as Record<string, any>;
}

function countByStatus(tasks: WorkflowTask[]) {
  const counts: Record<string, number> = {};
  for (const task of tasks) {
    const s = String(task.status || "unknown").toLowerCase();
    counts[s] = (counts[s] ?? 0) + 1;
  }
  return counts;
}

export default function WorkflowPanel(props: WorkflowPanelProps) {
  const [workflowId, setWorkflowId] = useLocalStorageState("agentui.workflowLookupId", "");
  const [listStatus, setListStatus] = useLocalStorageState("agentui.workflowListStatus", "active");
  const [listLimit, setListLimit] = useLocalStorageState("agentui.workflowListLimit", "50");
  const [listFilter, setListFilter] = useLocalStorageState("agentui.workflowListFilter", "");
  const [listFilterDebounced, setListFilterDebounced] = React.useState(listFilter);
  const [listAutoRefresh, setListAutoRefresh] = useLocalStorageState("agentui.workflowListAutoRefresh", false);
  const [includeResults, setIncludeResults] = useLocalStorageState("agentui.workflowIncludeResults", false);
  const [includeSpec, setIncludeSpec] = useLocalStorageState("agentui.workflowIncludeSpec", false);
  const [detail, setDetail] = React.useState<WorkflowDetailResp | null>(null);
  const [detailError, setDetailError] = React.useState<string | null>(null);
  const [cancelBusyId, setCancelBusyId] = React.useState<string | null>(null);
  const [copyNotice, setCopyNotice] = React.useState<string | null>(null);
  const copyTimerRef = React.useRef<number | null>(null);

  const normalizedListStatus = STATUS_OPTIONS.includes(String(listStatus)) ? String(listStatus) : "running";
  const limitValue = (() => {
    const n = Number(listLimit);
    if (!Number.isFinite(n)) return 50;
    return Math.min(Math.max(Math.trunc(n), 1), 200);
  })();

  const listQuery = useQuery({
    queryKey: ["workflows", props.baseUrl, props.authKey, normalizedListStatus, limitValue, listFilterDebounced],
    queryFn: () =>
      apiListWorkflows(props.baseUrl, { status: normalizedListStatus, limit: limitValue, query: listFilterDebounced }, props.auth),
    enabled: props.open && !!props.baseUrl,
    staleTime: 5_000,
    refetchInterval: listAutoRefresh ? 5_000 : false,
  });
  const filteredWorkflows = React.useMemo(() => {
    const workflows = extractWorkflows(listQuery.data);
    const query = String(listFilter || "").trim().toLowerCase();
    if (!query) return workflows;
    return workflows.filter((wf: any) => {
      const workflowId = String(wf.workflow_id || "").toLowerCase();
      const traceId = String(wf.trace_id || "").toLowerCase();
      const sessionId = String(wf.session_id || "").toLowerCase();
      const idempotencyKey = String(wf.idempotency_key || "").toLowerCase();
      return (
        workflowId.includes(query) ||
        traceId.includes(query) ||
        sessionId.includes(query) ||
        idempotencyKey.includes(query)
      );
    });
  }, [listFilter, listQuery.data]);

  React.useEffect(() => {
    const next = String(listFilter || "").trim();
    const handle = window.setTimeout(() => {
      setListFilterDebounced(next);
    }, 300);
    return () => window.clearTimeout(handle);
  }, [listFilter]);

  const workflowLookup = useMutation({
    mutationFn: async (id: string) =>
      apiGetWorkflow(
        props.baseUrl,
        {
          workflowId: id,
          includeTasks: true,
          includeResults,
          includeSpec,
        },
        props.auth,
      ),
    onSuccess: (resp) => {
      setDetail(resp);
      setDetailError(resp && resp.ok === false ? resp.error || "workflow lookup failed" : null);
    },
    onError: (err) => {
      setDetail(null);
      setDetailError(String(err));
    },
  });

  const canLoad = String(workflowId || "").trim().length > 0;
  const tasks = extractTasks(detail);
  const summary = extractWorkflowSummary(detail);
  const taskCounts = countByStatus(tasks);
  const graph = buildLevels(tasks);

  const loadWorkflow = (id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    if (!props.baseUrl) {
      setDetailError("Base URL is not set.");
      return;
    }
    setDetailError(null);
    workflowLookup.mutate(trimmed);
  };

  const cancelWorkflow = async (id: string) => {
    const trimmed = String(id || "").trim();
    if (!trimmed) return;
    if (!props.baseUrl) {
      setDetailError("Base URL is not set.");
      return;
    }
    setCancelBusyId(trimmed);
    setDetailError(null);
    try {
      const resp = await apiCancelWorkflow(props.baseUrl, trimmed, props.auth);
      if (resp && resp.ok === false) {
        setDetailError(resp.error || "Workflow cancel failed");
      }
    } catch (err) {
      setDetailError(String(err));
    } finally {
      setCancelBusyId(null);
    }
    loadWorkflow(trimmed);
    void listQuery.refetch();
  };

  const copyText = async (label: string, value?: string | null) => {
    const text = String(value || "").trim();
    if (!text) return;
    try {
      if (navigator?.clipboard?.writeText) {
        await navigator.clipboard.writeText(text);
      } else {
        const textarea = document.createElement("textarea");
        textarea.value = text;
        textarea.style.position = "fixed";
        textarea.style.opacity = "0";
        document.body.appendChild(textarea);
        textarea.focus();
        textarea.select();
        document.execCommand("copy");
        document.body.removeChild(textarea);
      }
      setCopyNotice(`${label} copied`);
    } catch {
      setCopyNotice(`Failed to copy ${label}`);
    }
    if (copyTimerRef.current) {
      window.clearTimeout(copyTimerRef.current);
    }
    copyTimerRef.current = window.setTimeout(() => setCopyNotice(null), 2000);
  };

  React.useEffect(
    () => () => {
      if (copyTimerRef.current) window.clearTimeout(copyTimerRef.current);
    },
    [],
  );

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Workflow editor</div>
          <div className="text-[11px] text-white/50">Durable workflow DAGs + composer</div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <div className="flex flex-wrap items-center gap-2">
          <input
            className="min-w-[260px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="workflow_id (e.g. wf_...)"
            value={workflowId}
            onChange={(e) => setWorkflowId(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={workflowLookup.isPending || !canLoad}
            onClick={() => loadWorkflow(workflowId)}
          >
            {workflowLookup.isPending ? "Loading…" : "Load"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={workflowLookup.isPending}
            onClick={() => {
              setDetail(null);
              setDetailError(null);
            }}
          >
            Clear
          </button>
        </div>
        {copyNotice ? <div className="text-[10px] text-white/50">{copyNotice}</div> : null}
        {copyNotice ? (
          <div className="text-[10px] text-white/50">Tip: use the copy buttons in the workflow list rows for quick sharing.</div>
        ) : null}

        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/70">
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={includeResults}
              onChange={(e) => setIncludeResults(e.target.checked)}
            />
            include results
          </label>
          <label className="flex items-center gap-1">
            <input type="checkbox" className="h-3 w-3" checked={includeSpec} onChange={(e) => setIncludeSpec(e.target.checked)} />
            include spec
          </label>
        </div>

        <div className="rounded-md border border-white/10 bg-black/30 p-3">
          <div className="mb-2 flex flex-wrap items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/70">Recent workflows</div>
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              <label className="flex items-center gap-1">
                status
                <select
                  className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                  value={normalizedListStatus}
                  onChange={(e) => setListStatus(e.target.value)}
                >
                  {STATUS_OPTIONS.map((s) => (
                    <option key={s} value={s}>
                      {s}
                    </option>
                  ))}
                </select>
              </label>
              <label className="flex items-center gap-1">
                limit
                <input
                  className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                  value={String(listLimit || "")}
                  onChange={(e) => setListLimit(e.target.value)}
                />
              </label>
              <label className="flex items-center gap-1">
                filter
                <span className="flex items-center gap-1">
                  <input
                    className="w-[140px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                    value={String(listFilter || "")}
                    onChange={(e) => setListFilter(e.target.value)}
                    placeholder="id/trace/session"
                  />
                  {String(listFilter || "").trim() ? (
                    <button
                      className="rounded border border-white/10 px-1 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                      type="button"
                      onClick={() => setListFilter("")}
                    >
                      clear
                    </button>
                  ) : null}
                </span>
              </label>
              <label className="flex items-center gap-1">
                <input
                  type="checkbox"
                  className="h-3 w-3"
                  checked={!!listAutoRefresh}
                  onChange={(e) => setListAutoRefresh(e.target.checked)}
                />
                auto
              </label>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => listQuery.refetch()}
                disabled={listQuery.isFetching}
              >
                {listQuery.isFetching ? "Refreshing…" : "Refresh"}
              </button>
            </div>
          </div>
          {!props.baseUrl ? (
            <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
              Set a daemon base URL to list workflows.
            </div>
          ) : null}
          {listQuery.isError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-xs text-rose-200">
              {String(listQuery.error)}
            </div>
          ) : null}
          <div className="grid gap-2">
            {filteredWorkflows.map((wf: any) => {
              const id = String(wf.workflow_id || "").trim();
              const canCancel = canCancelStatus(wf.status);
              return (
                <div
                  key={String(wf.workflow_id || Math.random())}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/10 bg-black/40 px-2 py-2 text-left text-xs text-white/80"
                >
                  <button
                    type="button"
                    onClick={() => {
                      if (!id) return;
                      setWorkflowId(id);
                      loadWorkflow(id);
                    }}
                    className="flex flex-1 flex-wrap items-center justify-between gap-2 text-left hover:text-white"
                  >
                    <div className="flex flex-wrap items-center gap-2">
                      <span className={`rounded border px-2 py-0.5 text-[10px] ${statusBadge(wf.status)}`}>
                        {wf.status ?? "unknown"}
                      </span>
                      {wf.cancel_requested ? (
                        <span className="rounded border border-amber-500/40 bg-amber-500/10 px-2 py-0.5 text-[9px] text-amber-100">
                          cancel requested
                        </span>
                      ) : null}
                      <span className="font-mono text-[11px] text-white/80">{wf.workflow_id}</span>
                      {wf.trace_id ? <span className="text-[10px] text-white/50">trace {String(wf.trace_id)}</span> : null}
                      {wf.session_id ? (
                        <span className="text-[10px] text-white/50">session {String(wf.session_id)}</span>
                      ) : null}
                      {wf.idempotency_key ? (
                        <span className="text-[10px] text-white/50">idk {String(wf.idempotency_key)}</span>
                      ) : null}
                    </div>
                    <div className="text-[10px] text-white/40">updated {formatUnixMs(wf.updated_unix_ms)}</div>
                  </button>
                  <div className="flex items-center gap-2">
                    {id ? (
                      <button
                        className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                        type="button"
                        onClick={() => void copyText("workflow id", id)}
                      >
                        copy id
                      </button>
                    ) : null}
                    {wf.trace_id ? (
                      <button
                        className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                        type="button"
                        onClick={() => void copyText("trace id", wf.trace_id)}
                      >
                        copy trace
                      </button>
                    ) : null}
                    {wf.session_id ? (
                      <button
                        className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                        type="button"
                        onClick={() => void copyText("session id", wf.session_id)}
                      >
                        copy session
                      </button>
                    ) : null}
                    {wf.idempotency_key ? (
                      <button
                        className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                        type="button"
                        onClick={() => void copyText("idempotency key", wf.idempotency_key)}
                      >
                        copy idk
                      </button>
                    ) : null}
                    {canCancel ? (
                      <button
                        className="rounded-md border border-rose-400/30 bg-rose-400/10 px-2 py-1 text-[10px] text-rose-100 hover:bg-rose-400/20 disabled:opacity-50"
                        type="button"
                        onClick={() => void cancelWorkflow(id)}
                        disabled={!id || cancelBusyId === id}
                      >
                        {cancelBusyId === id ? "Canceling…" : "Cancel"}
                      </button>
                    ) : null}
                  </div>
                </div>
              );
            })}
            {listQuery.isSuccess && filteredWorkflows.length === 0 ? (
              <div className="text-xs text-white/50">
                No workflows found for status "{normalizedListStatus}".
                {String(listFilter || "").trim() ? " Clear the filter to see more." : ""}
              </div>
            ) : null}
          </div>
        </div>

        {detailError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
            {detailError}
          </div>
        ) : null}

        {detail && summary.workflow_id ? (
          <div className="grid gap-3">
            <div className="rounded-md border border-white/10 bg-black/30 p-3">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div className="text-xs font-semibold text-white/70">Workflow summary</div>
                <div className="flex flex-wrap items-center gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => loadWorkflow(summary.workflow_id)}
                    disabled={workflowLookup.isPending}
                  >
                    {workflowLookup.isPending ? "Reloading…" : "Reload"}
                  </button>
                  {canCancelStatus(summary.status) ? (
                    <button
                      className="rounded-md border border-rose-400/30 bg-rose-400/10 px-2 py-1 text-[11px] text-rose-100 hover:bg-rose-400/20 disabled:opacity-50"
                      type="button"
                      onClick={() => void cancelWorkflow(summary.workflow_id)}
                      disabled={cancelBusyId === summary.workflow_id}
                    >
                      {cancelBusyId === summary.workflow_id ? "Canceling…" : "Cancel"}
                    </button>
                  ) : null}
                </div>
              </div>
              <div className="mt-2 grid gap-2 text-[11px] text-white/70">
                <div className="flex flex-wrap items-center gap-2">
                  <span className={`rounded border px-2 py-0.5 text-[10px] ${statusBadge(summary.status)}`}>
                    {summary.status ?? "unknown"}
                  </span>
                  <span className="font-mono text-[11px] text-white/80">{summary.workflow_id}</span>
                  {summary.workflow_id ? (
                    <button
                      type="button"
                      className="rounded border border-white/10 px-2 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                      onClick={() => void copyText("workflow id", summary.workflow_id)}
                    >
                      copy id
                    </button>
                  ) : null}
                  {summary.trace_id ? (
                    <span className="flex flex-wrap items-center gap-2">
                      <button
                        type="button"
                        className="rounded border border-white/10 px-2 py-0.5 text-[10px] text-white/70 hover:bg-white/5"
                        onClick={() => props.onTraceIdClick?.(String(summary.trace_id))}
                      >
                        trace {String(summary.trace_id)}
                      </button>
                      <button
                        type="button"
                        className="rounded border border-white/10 px-2 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                        onClick={() => void copyText("trace id", summary.trace_id)}
                      >
                        copy trace
                      </button>
                    </span>
                  ) : null}
                </div>
                <div className="grid gap-1 sm:grid-cols-2">
                  <div>priority: {summary.priority ?? "—"}</div>
                  <div>session: {summary.session_id || "—"}</div>
                  <div>idempotency: {summary.idempotency_key || "—"}</div>
                  <div>created: {formatUnixMs(summary.created_unix_ms)}</div>
                  <div>updated: {formatUnixMs(summary.updated_unix_ms)}</div>
                  <div>deadline: {formatUnixMs(summary.deadline_unix_ms)}</div>
                  <div>cancel requested: {String(summary.cancel_requested ?? false)}</div>
                </div>
                {summary.error ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                    {String(summary.error)}
                  </div>
                ) : null}
              </div>
            </div>

            <div className="rounded-md border border-white/10 bg-black/30 p-3">
              <div className="text-xs font-semibold text-white/70">Workflow DAG</div>
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                <span>tasks: {tasks.length}</span>
                {Object.keys(taskCounts).map((k) => (
                  <span key={k} className={`rounded border px-2 py-0.5 ${statusBadge(k)}`}>
                    {k}: {taskCounts[k]}
                  </span>
                ))}
              </div>
              {graph.hasCycle ? (
                <div className="mt-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
                  Dependency cycle detected; layout may be approximate.
                </div>
              ) : null}
              {graph.missingDeps.length > 0 ? (
                <div className="mt-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
                  Missing dependency references: {graph.missingDeps.join(", ")}
                </div>
              ) : null}
              <div className="mt-3 grid gap-3 overflow-x-auto">
                <div className="grid min-w-[640px] auto-cols-[minmax(200px,1fr)] grid-flow-col gap-3">
                  {graph.levels.map((levelTasks, idx) => (
                    <div key={`level-${idx}`} className="rounded-md border border-white/10 bg-black/20 p-2">
                      <div className="text-[10px] uppercase text-white/40">Level {idx}</div>
                      <div className="mt-2 grid gap-2">
                        {levelTasks.length === 0 ? (
                          <div className="text-[11px] text-white/40">—</div>
                        ) : null}
                        {levelTasks.map((task) => (
                          <div key={task.task_id} className="rounded-md border border-white/10 bg-black/40 px-2 py-1">
                            <div className="flex flex-wrap items-center justify-between gap-2">
                              <span className="font-mono text-[11px] text-white/80">{task.task_id}</span>
                              <span className={`rounded border px-1.5 py-0.5 text-[9px] ${statusBadge(task.status)}`}>
                                {task.status ?? "unknown"}
                              </span>
                            </div>
                            <div className="mt-1 text-[10px] text-white/50">
                              deps: {task.depends_on.length ? task.depends_on.join(", ") : "none"}
                            </div>
                            <div className="mt-1 text-[10px] text-white/40">
                              attempt {task.attempt ?? 0}/{task.max_attempts ?? 1}
                              {task.allow_error ? " · allow_error" : ""}
                            </div>
                            {task.error ? (
                              <div className="mt-1 text-[10px] text-rose-200">error: {task.error}</div>
                            ) : null}
                          </div>
                        ))}
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            </div>

            {(detail.workflow_limits || detail.workflow_usage || detail.workflow_remaining) && (
              <div className="rounded-md border border-white/10 bg-black/30 p-3">
                <div className="text-xs font-semibold text-white/70">Budgets</div>
                <div className="mt-2 grid gap-2 text-[11px] text-white/70">
                  {detail.workflow_limits ? (
                    <div>
                      <div className="text-white/50">limits</div>
                      <pre className="mt-1 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                        {JSON.stringify(detail.workflow_limits, null, 2)}
                      </pre>
                    </div>
                  ) : null}
                  {detail.workflow_usage ? (
                    <div>
                      <div className="text-white/50">usage</div>
                      <pre className="mt-1 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                        {JSON.stringify(detail.workflow_usage, null, 2)}
                      </pre>
                    </div>
                  ) : null}
                  {detail.workflow_remaining ? (
                    <div>
                      <div className="text-white/50">remaining</div>
                      <pre className="mt-1 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                        {JSON.stringify(detail.workflow_remaining, null, 2)}
                      </pre>
                    </div>
                  ) : null}
                </div>
              </div>
            )}

            {detail.result ? (
              <div className="rounded-md border border-white/10 bg-black/30 p-3">
                <div className="text-xs font-semibold text-white/70">Workflow result</div>
                <pre className="mt-2 max-h-80 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                  {JSON.stringify(detail.result, null, 2)}
                </pre>
              </div>
            ) : null}

            {detail.spec || detail.spec_json ? (
              <div className="rounded-md border border-white/10 bg-black/30 p-3">
                <div className="text-xs font-semibold text-white/70">Workflow spec</div>
                <pre className="mt-2 max-h-80 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                  {detail.spec ? JSON.stringify(detail.spec, null, 2) : detail.spec_json}
                </pre>
              </div>
            ) : null}
          </div>
        ) : null}

        <WorkflowComposer
          baseUrl={props.baseUrl}
          auth={props.auth}
          authKey={props.authKey}
          clientId={props.clientId}
          workflowDefaults={props.workflowDefaults}
          workflowTargets={props.workflowTargets}
          workflowBearerEnv={props.workflowBearerEnv}
          onSubmitted={(workflowId) => {
            setWorkflowId(workflowId);
            loadWorkflow(workflowId);
          }}
        />
      </div>
    </details>
  );
}
