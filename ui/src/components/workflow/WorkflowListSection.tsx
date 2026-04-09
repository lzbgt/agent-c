import React from "react";
import type { WorkflowSummaryRow } from "../../workflowTypes";
import {
  canCancelStatus,
  formatUnixMs,
  STATUS_OPTIONS,
  statusBadge,
} from "./workflowPanelUtils";
import type { WorkflowQueryState } from "./workflowScheduleSectionTypes";

type WorkflowListSectionProps = {
  baseUrl: string;
  normalizedListStatus: string;
  listStatus: string;
  setListStatus: (value: string) => void;
  listLimit: string;
  setListLimit: (value: string) => void;
  listFilter: string;
  setListFilter: (value: string) => void;
  listAutoRefresh: boolean;
  setListAutoRefresh: (value: boolean) => void;
  listQuery: WorkflowQueryState;
  filteredWorkflows: WorkflowSummaryRow[];
  cancelBusyId: string | null;
  onSelectWorkflow: (id: string) => void;
  onCopyText: (label: string, value?: string | null) => Promise<void> | void;
  onCancelWorkflow: (id: string) => Promise<void> | void;
};

export default function WorkflowListSection(props: WorkflowListSectionProps) {
  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-3" data-testid="workflow-list-panel">
      <div className="mb-2 flex flex-wrap items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/70">Recent workflows</div>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <label className="flex items-center gap-1">
            status
            <select
              className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={props.normalizedListStatus}
              onChange={(e) => props.setListStatus(e.target.value)}
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
              value={String(props.listLimit || "")}
              onChange={(e) => props.setListLimit(e.target.value)}
            />
          </label>
          <label className="flex items-center gap-1">
            filter
            <span className="flex items-center gap-1">
              <input
                className="w-[140px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                value={String(props.listFilter || "")}
                onChange={(e) => props.setListFilter(e.target.value)}
                placeholder="id/trace/session"
              />
              {String(props.listFilter || "").trim() ? (
                <button
                  className="rounded border border-white/10 px-1 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => props.setListFilter("")}
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
              checked={!!props.listAutoRefresh}
              onChange={(e) => props.setListAutoRefresh(e.target.checked)}
            />
            auto
          </label>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => props.listQuery.refetch()}
            disabled={props.listQuery.isFetching}
          >
            {props.listQuery.isFetching ? "Refreshing…" : "Refresh"}
          </button>
        </div>
      </div>
      {!props.baseUrl ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
          Set a daemon base URL to list workflows.
        </div>
      ) : null}
      {props.listQuery.isError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-xs text-rose-200">
          {String(props.listQuery.error)}
        </div>
      ) : null}
      <div className="grid gap-2">
        {props.filteredWorkflows.map((wf) => {
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
                  props.onSelectWorkflow(id);
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
                  {wf.session_id ? <span className="text-[10px] text-white/50">session {String(wf.session_id)}</span> : null}
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
                    onClick={() => void props.onCopyText("workflow id", id)}
                  >
                    copy id
                  </button>
                ) : null}
                {wf.trace_id ? (
                  <button
                    className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                    type="button"
                    onClick={() => void props.onCopyText("trace id", wf.trace_id)}
                  >
                    copy trace
                  </button>
                ) : null}
                {wf.session_id ? (
                  <button
                    className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                    type="button"
                    onClick={() => void props.onCopyText("session id", wf.session_id)}
                  >
                    copy session
                  </button>
                ) : null}
                {wf.idempotency_key ? (
                  <button
                    className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                    type="button"
                    onClick={() => void props.onCopyText("idempotency key", wf.idempotency_key)}
                  >
                    copy idk
                  </button>
                ) : null}
                {canCancel ? (
                  <button
                    className="rounded-md border border-rose-400/30 bg-rose-400/10 px-2 py-1 text-[10px] text-rose-100 hover:bg-rose-400/20 disabled:opacity-50"
                    type="button"
                    onClick={() => void props.onCancelWorkflow(id)}
                    disabled={!id || props.cancelBusyId === id}
                  >
                    {props.cancelBusyId === id ? "Canceling…" : "Cancel"}
                  </button>
                ) : null}
              </div>
            </div>
          );
        })}
        {props.listQuery.isSuccess && props.filteredWorkflows.length === 0 ? (
          <div className="text-xs text-white/50">
            No workflows found for status "{props.normalizedListStatus}".
            {String(props.listFilter || "").trim() ? " Clear the filter to see more." : ""}
          </div>
        ) : null}
      </div>
    </div>
  );
}
