import React from "react";
import type { WorkflowSummaryRow } from "../../workflowTypes";
import { canCancelStatus, formatUnixMs, STATUS_OPTIONS, statusBadge } from "./workflowPanelUtils";
import type { WorkflowQueryState } from "./workflowScheduleSectionTypes";
import { JsonDetails, ToolError } from "../ToolOutput";

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
  return <section data-testid="workflow-list-panel" className="grid gap-4">
    <div className="tool-form-row">
      <label className="tool-field">Status<select className="mt-2 block rounded-md border border-white/10 bg-black/30 p-2" value={props.normalizedListStatus} onChange={(e) => props.setListStatus(e.target.value)}>
        {STATUS_OPTIONS.map((status) => <option key={status} value={status}>{status === "all" ? "All statuses" : status.charAt(0).toUpperCase() + status.slice(1)}</option>)}
      </select></label>
      <label className="tool-field tool-grow">Find a run<input value={props.listFilter} onChange={(e) => props.setListFilter(e.target.value)} placeholder="Search runs" /></label>
      <button className="workspace-button" type="button" onClick={() => props.listQuery.refetch()} disabled={props.listQuery.isFetching}>{props.listQuery.isFetching ? "Refreshing…" : "Refresh"}</button>
    </div>
    <details className="tool-details"><summary>List options</summary>
      <div className="tool-fields"><label className="tool-field">Result limit<input value={props.listLimit} onChange={(e) => props.setListLimit(e.target.value)} inputMode="numeric" /></label>
      <label className="tool-checks"><input type="checkbox" checked={props.listAutoRefresh} onChange={(e) => props.setListAutoRefresh(e.target.checked)} />Auto refresh</label></div>
    </details>
    {!props.baseUrl ? <ToolError error="Connect to view workflows." /> : null}
    <ToolError error={props.listQuery.isError ? props.listQuery.error instanceof Error ? props.listQuery.error.message : String(props.listQuery.error) : null} />
    {props.filteredWorkflows.map((wf) => {
      const id = wf.workflow_id;
      const title = typeof wf.name === "string" && wf.name.trim() ? wf.name : id.length > 20 ? `Workflow · ${id.slice(-8)}` : id;
      return <article key={id} className="tool-card">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <button className="min-w-0 flex-1 text-left" type="button" onClick={() => props.onSelectWorkflow(id)} aria-label={`Open ${title}`}>
            <span className="block text-sm text-white/90">{title}</span>
            <span className={`mt-2 inline-block rounded border px-2 py-1 text-xs ${statusBadge(wf.status)}`}>{wf.cancel_requested ? "Cancel requested" : wf.status || "Unknown"}</span>
          </button>
          {canCancelStatus(wf.status) ? <button className="workspace-button" type="button" onClick={() => void props.onCancelWorkflow(id)} disabled={props.cancelBusyId === id}>{props.cancelBusyId === id ? "Canceling…" : "Cancel"}</button> : null}
        </div>
        {wf.error ? <ToolError error={wf.error} /> : null}
        <details className="tool-details"><summary>Run details</summary>
          <p className="tool-muted">Updated {formatUnixMs(wf.updated_unix_ms)}</p>
          <div className="tool-form-row">
            {[["workflow id", id], ["trace id", wf.trace_id], ["session id", wf.session_id], ["idempotency key", wf.idempotency_key]].map(([label, value]) => value ? <button className="workspace-button" type="button" key={label} onClick={() => void props.onCopyText(label!, value)}>Copy {label}</button> : null)}
          </div>
          <JsonDetails value={wf} />
        </details>
      </article>;
    })}
    {props.listQuery.isSuccess && !props.filteredWorkflows.length ? <p className="tool-empty">No workflows found.</p> : null}
  </section>;
}
