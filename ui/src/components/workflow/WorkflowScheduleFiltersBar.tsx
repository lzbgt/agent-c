import React from "react";

import {
  SCHEDULE_STATUS_OPTIONS,
} from "./workflowPanelUtils";
import type { WorkflowSchedulesSectionProps } from "./workflowScheduleSectionTypes";

type WorkflowScheduleFiltersBarProps = Pick<
  WorkflowSchedulesSectionProps,
  | "normalizedScheduleStatus"
  | "setScheduleStatus"
  | "scheduleLimit"
  | "setScheduleLimit"
  | "scheduleOffset"
  | "setScheduleOffset"
  | "scheduleFilter"
  | "setScheduleFilter"
  | "scheduleAutoRefresh"
  | "setScheduleAutoRefresh"
  | "scheduleOffsetValue"
  | "scheduleLimitValue"
  | "scheduleList"
  | "filteredScheduleList"
  | "scheduleListQuery"
  | "onCopyJson"
  | "onDownloadJson"
>;

export default function WorkflowScheduleFiltersBar(props: WorkflowScheduleFiltersBarProps) {
  return (
    <div className="mb-2 flex flex-wrap items-center justify-between gap-2">
      <div className="text-xs font-semibold text-white/70">Workflow schedules (UTC)</div>
      <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <label className="flex items-center gap-1">
          status
          <select
            className="rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
            value={props.normalizedScheduleStatus}
            onChange={(e) => props.setScheduleStatus(e.target.value)}
          >
            {SCHEDULE_STATUS_OPTIONS.map((status) => (
              <option key={status} value={status}>
                {status}
              </option>
            ))}
          </select>
        </label>
        <label className="flex items-center gap-1">
          limit
          <input
            className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
            value={String(props.scheduleLimit || "")}
            onChange={(e) => props.setScheduleLimit(e.target.value)}
          />
        </label>
        <label className="flex items-center gap-1">
          offset
          <input
            className="w-[56px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
            value={String(props.scheduleOffset || "")}
            onChange={(e) => props.setScheduleOffset(e.target.value)}
          />
        </label>
        <label className="flex items-center gap-1">
          filter
          <span className="flex items-center gap-1">
            <input
              className="w-[140px] rounded border border-white/10 bg-black/40 px-1 py-0.5 text-[11px] text-white/80"
              value={String(props.scheduleFilter || "")}
              onChange={(e) => props.setScheduleFilter(e.target.value)}
              placeholder="id/cron/error"
            />
            {String(props.scheduleFilter || "").trim() ? (
              <button
                className="rounded border border-white/10 px-1 py-0.5 text-[10px] text-white/60 hover:bg-white/5"
                type="button"
                onClick={() => props.setScheduleFilter("")}
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
            checked={!!props.scheduleAutoRefresh}
            onChange={(e) => props.setScheduleAutoRefresh(e.target.checked)}
          />
          auto
        </label>
        <div className="flex items-center gap-1">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => props.setScheduleOffset(String(Math.max(0, props.scheduleOffsetValue - props.scheduleLimitValue)))}
            disabled={props.scheduleOffsetValue === 0}
          >
            Prev
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            onClick={() => props.setScheduleOffset(String(props.scheduleOffsetValue + props.scheduleLimitValue))}
            disabled={props.scheduleList.length < props.scheduleLimitValue}
          >
            Next
          </button>
        </div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
          type="button"
          onClick={() => props.scheduleListQuery.refetch()}
          disabled={props.scheduleListQuery.isFetching}
        >
          {props.scheduleListQuery.isFetching ? "Refreshing…" : "Refresh"}
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => void props.onCopyJson("schedules", props.filteredScheduleList)}
          disabled={props.filteredScheduleList.length === 0}
        >
          Copy JSON
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          onClick={() => props.onDownloadJson("workflow-schedules", props.filteredScheduleList)}
          disabled={props.filteredScheduleList.length === 0}
        >
          Download JSON
        </button>
      </div>
    </div>
  );
}
