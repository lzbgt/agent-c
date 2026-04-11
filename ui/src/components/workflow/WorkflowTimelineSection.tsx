import React from "react";
import type { UseQueryResult } from "@tanstack/react-query";
import type { WorkflowEvent, WorkflowEventsResp } from "../../api";
import { formatUnixMs } from "./workflowPanelUtils";

type WorkflowTimelineSectionProps = {
  workflowId: string;
  taskIdFilter: string;
  setTaskIdFilter: (value: string) => void;
  eventTypeFilter: string;
  setEventTypeFilter: (value: string) => void;
  limit: string;
  setLimit: (value: string) => void;
  autoRefresh: boolean;
  setAutoRefresh: (value: boolean) => void;
  eventsQuery: UseQueryResult<WorkflowEventsResp, Error>;
  onCopyText: (label: string, value?: string | null) => Promise<void> | void;
};

const eventTypeClass = (type?: string) => {
  const t = String(type || "").toLowerCase();
  if (t.includes("done")) return "border-emerald-500/30 bg-emerald-500/15 text-emerald-200";
  if (t.includes("error") || t.includes("exceeded")) return "border-rose-500/30 bg-rose-500/15 text-rose-200";
  if (t.includes("status")) return "border-sky-500/30 bg-sky-500/15 text-sky-200";
  if (t.includes("checkpoint")) return "border-amber-500/30 bg-amber-500/15 text-amber-200";
  return "border-white/10 bg-white/10 text-white/70";
};

function WorkflowEventCard({ event }: { event: WorkflowEvent }) {
  return (
    <div className="rounded-md border border-white/10 bg-black/30 px-3 py-2">
      <div className="flex min-w-0 flex-wrap items-center gap-2 text-[11px] text-white/70">
        <span className="font-mono text-white/50">#{event.event_id}</span>
        <span className={`rounded border px-2 py-0.5 text-[10px] ${eventTypeClass(event.type)}`}>
          {event.type || "event"}
        </span>
        {event.task_id ? <span className="font-mono text-white/70">task {event.task_id}</span> : null}
        {event.schema ? <span className="text-white/40">{event.schema}</span> : null}
        <span className="ml-auto text-white/50">{formatUnixMs(event.ts_unix_ms)}</span>
      </div>
      {event.data !== undefined ? (
        <pre className="mt-2 max-h-44 overflow-auto rounded bg-black/40 p-2 text-[10px] text-white/70">
          {JSON.stringify(event.data, null, 2)}
        </pre>
      ) : null}
    </div>
  );
}

export default function WorkflowTimelineSection(props: WorkflowTimelineSectionProps) {
  if (!props.workflowId) return null;
  const events = props.eventsQuery.data?.events ?? [];
  const filtersActive = props.taskIdFilter.trim() || props.eventTypeFilter.trim();

  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-3" data-testid="workflow-timeline-panel">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div>
          <div className="text-xs font-semibold text-white/70">Workflow timeline</div>
          <div className="mt-1 text-[10px] text-white/45">
            {events.length} events · cursor {props.eventsQuery.data?.cursor_next ?? 0}
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <label className="flex items-center gap-1 text-[11px] text-white/60">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={props.autoRefresh}
              onChange={(event) => props.setAutoRefresh(event.target.checked)}
            />
            auto refresh
          </label>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={props.eventsQuery.isFetching}
            onClick={() => void props.eventsQuery.refetch()}
          >
            {props.eventsQuery.isFetching ? "Loading..." : "Refresh"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/60 hover:bg-black/40"
            type="button"
            onClick={() => void props.onCopyText("workflow events", JSON.stringify(props.eventsQuery.data ?? {}, null, 2))}
          >
            copy events
          </button>
        </div>
      </div>

      <div className="mt-3 grid gap-2 sm:grid-cols-[1fr_1fr_120px_auto]">
        <input
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 placeholder:text-white/40"
          placeholder="task_id"
          value={props.taskIdFilter}
          onChange={(event) => props.setTaskIdFilter(event.target.value)}
        />
        <input
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 placeholder:text-white/40"
          placeholder="event_type"
          value={props.eventTypeFilter}
          onChange={(event) => props.setEventTypeFilter(event.target.value)}
        />
        <input
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 placeholder:text-white/40"
          placeholder="limit"
          inputMode="numeric"
          value={props.limit}
          onChange={(event) => props.setLimit(event.target.value)}
        />
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!filtersActive}
          onClick={() => {
            props.setTaskIdFilter("");
            props.setEventTypeFilter("");
          }}
        >
          Clear filters
        </button>
      </div>

      {props.eventsQuery.error ? (
        <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          {String(props.eventsQuery.error)}
        </div>
      ) : null}

      <div className="mt-3 grid gap-2">
        {events.map((event) => (
          <WorkflowEventCard key={`${event.event_id}:${event.type}:${event.task_id || ""}`} event={event} />
        ))}
        {!props.eventsQuery.isFetching && events.length === 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-xs text-white/45">
            No workflow events match the current filters.
          </div>
        ) : null}
      </div>
    </div>
  );
}
