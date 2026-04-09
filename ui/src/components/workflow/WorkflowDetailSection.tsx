import React from "react";
import type { WorkflowDetailResp } from "../../api";
import type { WorkflowBudgetSnapshot, WorkflowSummaryRow } from "../../workflowTypes";
import {
  canCancelStatus,
  formatUnixMs,
  statusBadge,
  type WorkflowTask,
} from "./workflowPanelUtils";

type WorkflowDetailSectionProps = {
  detail: WorkflowDetailResp | null;
  summary: WorkflowSummaryRow | null;
  tasks: WorkflowTask[];
  taskCounts: Record<string, number>;
  graph: { levels: WorkflowTask[][]; hasCycle: boolean; missingDeps: string[] };
  workflowLimits?: WorkflowBudgetSnapshot;
  workflowUsage?: WorkflowBudgetSnapshot;
  workflowRemaining?: WorkflowBudgetSnapshot;
  cancelBusyId: string | null;
  workflowLookupPending: boolean;
  onReloadWorkflow: (workflowId: string) => void;
  onCancelWorkflow: (workflowId: string) => Promise<void> | void;
  onCopyText: (label: string, value?: string | null) => Promise<void> | void;
  onTraceIdClick?: (traceId: string) => void;
};

export default function WorkflowDetailSection(props: WorkflowDetailSectionProps) {
  const summary = props.summary;
  if (!props.detail || !summary?.workflow_id) return null;

  return (
    <div className="grid gap-3" data-testid="workflow-detail-panel">
      <div className="rounded-md border border-white/10 bg-black/30 p-3">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/70">Workflow summary</div>
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => props.onReloadWorkflow(summary.workflow_id)}
              disabled={props.workflowLookupPending}
            >
              {props.workflowLookupPending ? "Reloading…" : "Reload"}
            </button>
            {canCancelStatus(summary.status) ? (
              <button
                className="rounded-md border border-rose-400/30 bg-rose-400/10 px-2 py-1 text-[11px] text-rose-100 hover:bg-rose-400/20 disabled:opacity-50"
                type="button"
                onClick={() => void props.onCancelWorkflow(summary.workflow_id)}
                disabled={props.cancelBusyId === summary.workflow_id}
              >
                {props.cancelBusyId === summary.workflow_id ? "Canceling…" : "Cancel"}
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
                onClick={() => void props.onCopyText("workflow id", summary.workflow_id)}
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
                  onClick={() => void props.onCopyText("trace id", summary.trace_id)}
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

      <div className="rounded-md border border-white/10 bg-black/30 p-3" data-testid="workflow-dag-panel">
        <div className="text-xs font-semibold text-white/70">Workflow DAG</div>
        <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <span>tasks: {props.tasks.length}</span>
          {Object.keys(props.taskCounts).map((k) => (
            <span key={k} className={`rounded border px-2 py-0.5 ${statusBadge(k)}`}>
              {k}: {props.taskCounts[k]}
            </span>
          ))}
        </div>
        {props.graph.hasCycle ? (
          <div className="mt-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
            Dependency cycle detected; layout may be approximate.
          </div>
        ) : null}
        {props.graph.missingDeps.length > 0 ? (
          <div className="mt-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
            Missing dependency references: {props.graph.missingDeps.join(", ")}
          </div>
        ) : null}
        <div className="mt-3 grid gap-3 overflow-x-auto">
          <div className="grid min-w-[640px] auto-cols-[minmax(200px,1fr)] grid-flow-col gap-3">
            {props.graph.levels.map((levelTasks, idx) => (
              <div key={`level-${idx}`} className="rounded-md border border-white/10 bg-black/20 p-2">
                <div className="text-[10px] uppercase text-white/40">Level {idx}</div>
                <div className="mt-2 grid gap-2">
                  {levelTasks.length === 0 ? <div className="text-[11px] text-white/40">—</div> : null}
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
                      {task.error ? <div className="mt-1 text-[10px] text-rose-200">error: {task.error}</div> : null}
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        </div>
      </div>

      {(props.workflowLimits || props.workflowUsage || props.workflowRemaining) && (
        <div className="rounded-md border border-white/10 bg-black/30 p-3">
          <div className="text-xs font-semibold text-white/70">Budgets</div>
          <div className="mt-2 grid gap-2 text-[11px] text-white/70">
            {props.workflowLimits ? (
              <div>
                <div className="text-white/50">limits</div>
                <pre className="mt-1 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                  {JSON.stringify(props.workflowLimits, null, 2)}
                </pre>
              </div>
            ) : null}
            {props.workflowUsage ? (
              <div>
                <div className="text-white/50">usage</div>
                <pre className="mt-1 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                  {JSON.stringify(props.workflowUsage, null, 2)}
                </pre>
              </div>
            ) : null}
            {props.workflowRemaining ? (
              <div>
                <div className="text-white/50">remaining</div>
                <pre className="mt-1 max-h-40 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
                  {JSON.stringify(props.workflowRemaining, null, 2)}
                </pre>
              </div>
            ) : null}
          </div>
        </div>
      )}

      {props.detail.result ? (
        <div className="rounded-md border border-white/10 bg-black/30 p-3">
          <div className="text-xs font-semibold text-white/70">Workflow result</div>
          <pre className="mt-2 max-h-80 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
            {JSON.stringify(props.detail.result, null, 2)}
          </pre>
        </div>
      ) : null}

      {props.detail.spec || props.detail.spec_json ? (
        <div className="rounded-md border border-white/10 bg-black/30 p-3">
          <div className="text-xs font-semibold text-white/70">Workflow spec</div>
          <pre className="mt-2 max-h-80 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
            {props.detail.spec ? JSON.stringify(props.detail.spec, null, 2) : props.detail.spec_json}
          </pre>
        </div>
      ) : null}
    </div>
  );
}
