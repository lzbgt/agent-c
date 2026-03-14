import React from "react";

import { formatUnixMs, SCHEDULE_RUN_STATUS_OPTIONS, statusBadge } from "./workflowPanelUtils";
import type { WorkflowSchedulesSectionProps } from "./workflowScheduleSectionTypes";

type WorkflowScheduleRunsSectionProps = Pick<
  WorkflowSchedulesSectionProps,
  | "scheduleRunsLimit"
  | "setScheduleRunsLimit"
  | "scheduleRunsOffset"
  | "setScheduleRunsOffset"
  | "normalizedScheduleRunsStatus"
  | "setScheduleRunsStatus"
  | "scheduleRunsErrorsOnly"
  | "setScheduleRunsErrorsOnly"
  | "scheduleRunsFilter"
  | "setScheduleRunsFilter"
  | "scheduleRunsOffsetValue"
  | "scheduleRunsLimitValue"
  | "scheduleRuns"
  | "scheduleRunsQuery"
  | "scheduleId"
  | "filteredScheduleRuns"
  | "onCopyJson"
  | "onDownloadJson"
  | "onLoadWorkflowFromRun"
>;

export default function WorkflowScheduleRunsSection(props: WorkflowScheduleRunsSectionProps) {
  return (
    <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2" data-testid="workflow-schedule-runs-panel">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="text-[11px] text-white/60">Schedule runs</div>
        <div className="flex items-center gap-2 text-[11px] text-white/60">
          <label className="flex items-center gap-1">
            limit
            <input
              className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={String(props.scheduleRunsLimit || "")}
              onChange={(e) => props.setScheduleRunsLimit(e.target.value)}
            />
          </label>
          <label className="flex items-center gap-1">
            offset
            <input
              className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={String(props.scheduleRunsOffset || "")}
              onChange={(e) => props.setScheduleRunsOffset(e.target.value)}
            />
          </label>
          <label className="flex items-center gap-1">
            status
            <select
              className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={props.normalizedScheduleRunsStatus}
              onChange={(e) => props.setScheduleRunsStatus(e.target.value)}
            >
              {SCHEDULE_RUN_STATUS_OPTIONS.map((status) => (
                <option key={status} value={status}>
                  {status}
                </option>
              ))}
            </select>
          </label>
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={!!props.scheduleRunsErrorsOnly}
              onChange={(e) => props.setScheduleRunsErrorsOnly(e.target.checked)}
            />
            errors only
          </label>
          <label className="flex items-center gap-1">
            filter
            <span className="flex items-center gap-1">
              <input
                className="w-[120px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
                value={String(props.scheduleRunsFilter || "")}
                onChange={(e) => props.setScheduleRunsFilter(e.target.value)}
                placeholder="workflow id"
              />
              {String(props.scheduleRunsFilter || "").trim() ? (
                <button
                  className="rounded border border-white/10 px-1 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => props.setScheduleRunsFilter("")}
                >
                  clear
                </button>
              ) : null}
            </span>
          </label>
          <div className="flex items-center gap-1">
            <button
              className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
              type="button"
              onClick={() =>
                props.setScheduleRunsOffset(String(Math.max(0, props.scheduleRunsOffsetValue - props.scheduleRunsLimitValue)))
              }
              disabled={props.scheduleRunsOffsetValue === 0}
            >
              Prev
            </button>
            <button
              className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
              type="button"
              onClick={() => props.setScheduleRunsOffset(String(props.scheduleRunsOffsetValue + props.scheduleRunsLimitValue))}
              disabled={props.scheduleRuns.length < props.scheduleRunsLimitValue}
            >
              Next
            </button>
          </div>
          <button
            className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
            type="button"
            onClick={() => props.scheduleRunsQuery.refetch()}
            disabled={props.scheduleRunsQuery.isFetching || !String(props.scheduleId || "").trim()}
          >
            {props.scheduleRunsQuery.isFetching ? "Refreshing…" : "Refresh"}
          </button>
          <button
            className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
            type="button"
            onClick={() => void props.onCopyJson("schedule runs", props.scheduleRuns)}
            disabled={props.scheduleRuns.length === 0}
          >
            Copy JSON
          </button>
          <button
            className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
            type="button"
            onClick={() => props.onDownloadJson("workflow-schedule-runs", props.scheduleRuns)}
            disabled={props.scheduleRuns.length === 0}
          >
            Download JSON
          </button>
        </div>
      </div>
      <div className="text-[10px] text-white/50">schedule_id: {props.scheduleId ? props.scheduleId : "—"}</div>
      {props.scheduleRunsQuery.isError ? (
        <div className="text-[11px] text-rose-200">{String(props.scheduleRunsQuery.error)}</div>
      ) : null}
      <div className="grid gap-2">
        {props.filteredScheduleRuns.map((run) => (
          <div
            key={`${run.schedule_id || "schedule"}-${run.tick_unix_ms || 0}-${run.workflow_id || "workflow"}`}
            className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/10 bg-black/50 px-2 py-2 text-[11px] text-white/70"
          >
            <div className="flex flex-wrap items-center gap-2">
              <span className={`rounded border px-2 py-0.5 text-[10px] ${statusBadge(run.status)}`}>
                {String(run.status || "unknown")}
              </span>
              <span className="font-mono">{String(run.workflow_id || "")}</span>
              <span className="text-white/50">tick {formatUnixMs(run.tick_unix_ms)}</span>
            </div>
            <div className="flex items-center gap-2">
              {run.workflow_id ? (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => {
                    const id = String(run.workflow_id || "").trim();
                    if (!id) return;
                    props.onLoadWorkflowFromRun(id);
                  }}
                >
                  load workflow
                </button>
              ) : null}
            </div>
            {run.error ? <div className="text-rose-200">{String(run.error)}</div> : null}
          </div>
        ))}
        {props.scheduleRunsQuery.isSuccess && props.filteredScheduleRuns.length === 0 ? (
          <div className="rounded border border-white/10 bg-black/20 px-2 py-2 text-[11px] text-white/50">
            {props.scheduleRuns.length === 0 ? "No runs yet." : "No runs match the filter."}
          </div>
        ) : null}
      </div>
    </div>
  );
}
