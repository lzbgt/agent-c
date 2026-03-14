import React from "react";

import { formatUnixMs, statusBadge } from "./workflowPanelUtils";
import type { WorkflowSchedulesSectionProps } from "./workflowScheduleSectionTypes";

type WorkflowScheduleListSectionProps = Pick<
  WorkflowSchedulesSectionProps,
  | "filteredScheduleList"
  | "scheduleList"
  | "scheduleBusyId"
  | "scheduleListQuery"
  | "onLoadScheduleRuns"
  | "onCopyText"
  | "scheduleCurlSnippet"
  | "onPauseSchedule"
  | "onResumeSchedule"
  | "onDeleteSchedule"
>;

export default function WorkflowScheduleListSection(props: WorkflowScheduleListSectionProps) {
  return (
    <div className="grid gap-2">
      {props.filteredScheduleList.map((schedule) => {
        const id = String(schedule.schedule_id || "").trim();
        const status = String(schedule.status || "").toLowerCase();
        return (
          <div
            data-testid={`workflow-schedule-row-${id}`}
            key={id || `${schedule.cron || "schedule"}-${schedule.updated_unix_ms || 0}`}
            className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/10 bg-black/40 px-2 py-2 text-left text-xs text-white/80"
          >
            <button
              type="button"
              onClick={() => {
                if (!id) return;
                props.onLoadScheduleRuns(id);
              }}
              className="flex flex-1 flex-wrap items-center justify-between gap-2 text-left hover:text-white"
            >
              <div className="flex flex-wrap items-center gap-2">
                <span className={`rounded border px-2 py-0.5 text-[10px] ${statusBadge(status)}`}>{status || "unknown"}</span>
                <span className="font-mono text-[11px] text-white/80">{id}</span>
                {schedule.cron ? <span className="text-[10px] text-white/50">cron {String(schedule.cron)}</span> : null}
                {schedule.next_tick_unix_ms ? (
                  <span className="text-[10px] text-white/50">next {formatUnixMs(schedule.next_tick_unix_ms)}</span>
                ) : null}
                {schedule.last_error ? (
                  <span className="text-[10px] text-rose-200">err {String(schedule.last_error)}</span>
                ) : null}
              </div>
              <div className="text-[10px] text-white/40">updated {formatUnixMs(schedule.updated_unix_ms)}</div>
            </button>
            <div className="flex items-center gap-2">
              {id ? (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => void props.onCopyText("schedule id", id)}
                >
                  copy id
                </button>
              ) : null}
              {id ? (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => void props.onCopyText("schedule pause curl", props.scheduleCurlSnippet(id, "pause"))}
                >
                  copy pause curl
                </button>
              ) : null}
              {id ? (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => void props.onCopyText("schedule resume curl", props.scheduleCurlSnippet(id, "resume"))}
                >
                  copy resume curl
                </button>
              ) : null}
              {id ? (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5"
                  type="button"
                  onClick={() => void props.onCopyText("schedule delete curl", props.scheduleCurlSnippet(id, "delete"))}
                >
                  copy delete curl
                </button>
              ) : null}
              {status === "active" ? (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                  type="button"
                  onClick={() => void props.onPauseSchedule(id)}
                  disabled={props.scheduleBusyId === id}
                >
                  pause
                </button>
              ) : (
                <button
                  className="rounded border border-white/10 px-2 py-1 text-[10px] text-white/60 hover:bg-white/5 disabled:opacity-50"
                  type="button"
                  onClick={() => void props.onResumeSchedule(id)}
                  disabled={props.scheduleBusyId === id}
                >
                  resume
                </button>
              )}
              <button
                className="rounded border border-rose-500/30 px-2 py-1 text-[10px] text-rose-200 hover:bg-rose-500/10 disabled:opacity-50"
                type="button"
                onClick={() => void props.onDeleteSchedule(id)}
                disabled={props.scheduleBusyId === id}
              >
                delete
              </button>
            </div>
          </div>
        );
      })}
      {props.scheduleListQuery.isSuccess && props.filteredScheduleList.length === 0 ? (
        <div className="rounded border border-white/10 bg-black/20 px-2 py-2 text-[11px] text-white/50">
          {props.scheduleList.length === 0 ? "No schedules yet." : "No schedules match the filter."}
        </div>
      ) : null}
    </div>
  );
}
