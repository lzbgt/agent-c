import React from "react";
import { ToolError } from "../ToolOutput";
import type { ApprovalQueueState } from "./approvalQueueTypes";

export default function ApprovalQueueFiltersSection({ state }: { state: ApprovalQueueState }) {
  const filters: [string, string, (value: string) => void][] = [
    ["Team ID", state.teamIdFilter, state.setTeamIdFilter], ["Trace ID", state.traceIdFilter, state.setTraceIdFilter],
    ["Job ID", state.jobIdFilter, state.setJobIdFilter], ["Tool name", state.toolNameFilter, state.setToolNameFilter],
    ["Run ID", state.runIdFilter, state.setRunIdFilter], ["Result limit", state.limitFilter, state.setLimitFilter],
  ];
  return <section data-testid="approval-queue-filters">
    <div className="tool-form-row">
      <label className="tool-field tool-grow">Status
        <select className="mt-2 w-full rounded-md border border-white/10 bg-black/30 p-3" value={state.statusFilter} onChange={(event) => state.setStatusFilter(event.target.value)}>
          <option value="">All statuses</option><option value="pending">Pending</option><option value="approved">Approved</option><option value="denied">Denied</option><option value="expired">Expired</option>
        </select>
      </label>
      <button className="workspace-button" type="button" disabled={state.listBusy || !state.canQuery} onClick={state.loadApprovals}>{state.listBusy ? "Loading…" : "Load approvals"}</button>
      <button className="workspace-button" type="button" disabled={state.listBusy} onClick={state.clearApprovals}>Clear</button>
    </div>
    <details className="tool-details" data-testid="approval-more-filters">
      <summary>More filters</summary>
      <div className="tool-fields">{filters.map(([label, value, set]) => <label className="tool-field" key={label}>{label}<input value={value} onChange={(event) => set(event.target.value)} /></label>)}</div>
    </details>
    <ToolError error={state.listError} />
  </section>;
}
