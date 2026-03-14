import React from "react";
import FieldLabel from "../FieldLabel";
import type { ApprovalQueueState } from "./approvalQueueTypes";

type ApprovalQueueFiltersSectionProps = {
  state: ApprovalQueueState;
};

export default function ApprovalQueueFiltersSection({ state }: ApprovalQueueFiltersSectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="approval-queue-filters">
      <div className="grid gap-3 md:grid-cols-3">
        <div className="grid gap-1">
          <FieldLabel>Status</FieldLabel>
          <select
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90"
            value={state.statusFilter}
            onChange={(event) => state.setStatusFilter(event.target.value)}
          >
            <option value="">all</option>
            <option value="pending">pending</option>
            <option value="approved">approved</option>
            <option value="denied">denied</option>
            <option value="expired">expired</option>
          </select>
        </div>
        <div className="grid gap-1">
          <FieldLabel>Team ID</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="team_..."
            value={state.teamIdFilter}
            onChange={(event) => state.setTeamIdFilter(event.target.value)}
          />
        </div>
        <div className="grid gap-1">
          <FieldLabel>Trace ID</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="trace_..."
            value={state.traceIdFilter}
            onChange={(event) => state.setTraceIdFilter(event.target.value)}
          />
        </div>
        <div className="grid gap-1">
          <FieldLabel>Job ID</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="job_..."
            value={state.jobIdFilter}
            onChange={(event) => state.setJobIdFilter(event.target.value)}
          />
        </div>
        <div className="grid gap-1">
          <FieldLabel>Tool name</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="shell_exec"
            value={state.toolNameFilter}
            onChange={(event) => state.setToolNameFilter(event.target.value)}
          />
        </div>
        <div className="grid gap-1">
          <FieldLabel>Run ID</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="123"
            value={state.runIdFilter}
            onChange={(event) => state.setRunIdFilter(event.target.value)}
          />
        </div>
        <div className="grid gap-1">
          <FieldLabel>Limit</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="100"
            value={state.limitFilter}
            onChange={(event) => state.setLimitFilter(event.target.value)}
          />
        </div>
      </div>
      <div className="mt-3 flex flex-wrap gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={state.listBusy || !state.canQuery}
          onClick={state.loadApprovals}
        >
          {state.listBusy ? "Loading…" : "Load approvals"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={state.listBusy}
          onClick={state.clearApprovals}
        >
          Clear
        </button>
      </div>
      {state.listError ? (
        <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          {state.listError}
        </div>
      ) : null}
    </section>
  );
}
