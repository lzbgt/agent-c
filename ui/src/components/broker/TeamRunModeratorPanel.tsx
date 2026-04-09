import React from "react";
import FieldLabel from "../FieldLabel";
import type { MemberSession, TeamRunModeratorEventRow } from "./teamRunStatusTypes";

export type TeamRunModeratorPanelProps = {
  canQuery: boolean;
  runId: string;
  memberSessions: MemberSession[];
  roleOptions: string[];
  memberOptions: string[];
  agentOptions: string[];
  directive: string;
  directiveScope: string;
  taskTitle: string;
  taskDetail: string;
  taskStatus: string;
  targetRoles: string;
  targetMembers: string;
  targetAgents: string;
  assignees: string;
  appendToSession: boolean;
  busy: boolean;
  error: string | null;
  success: string | null;
  events: TeamRunModeratorEventRow[];
  eventsBusy: boolean;
  eventsError: string | null;
  eventsTypes: string;
  eventsMaxBytes: string;
  eventsLimit: string;
  eventsExpanded: boolean;
  onDirectiveChange: (value: string) => void;
  onDirectiveScopeChange: (value: string) => void;
  onTaskTitleChange: (value: string) => void;
  onTaskDetailChange: (value: string) => void;
  onTaskStatusChange: (value: string) => void;
  onTargetRolesChange: (value: string) => void;
  onTargetMembersChange: (value: string) => void;
  onTargetAgentsChange: (value: string) => void;
  onAssigneesChange: (value: string) => void;
  onAppendToSessionChange: (value: boolean) => void;
  onPublishDirective: () => void;
  onPublishTask: () => void;
  onEventsTypesChange: (value: string) => void;
  onEventsMaxBytesChange: (value: string) => void;
  onEventsLimitChange: (value: string) => void;
  onEventsLoad: () => void;
  onEventsToggleExpanded: () => void;
};

export default function TeamRunModeratorPanel(props: TeamRunModeratorPanelProps) {
  const sessionsEmpty = props.memberSessions.length === 0;
  const disabled = props.busy || !props.canQuery || !props.runId;
  const eventsDisabled = props.eventsBusy || !props.canQuery || !props.runId;

  return (
    <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
      <div className="text-xs font-semibold text-white/80">Moderator broadcast</div>
      <div className="text-[11px] text-white/50">
        Publish directives or tasks to team run member sessions without stopping the run.
      </div>
      {props.runId && sessionsEmpty ? (
        <div className="text-[11px] text-amber-200">No member sessions stored; broadcasts will skip missing sessions.</div>
      ) : null}
      <div className="grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Target roles</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.targetRoles}
            onChange={(e) => props.onTargetRolesChange(e.target.value)}
            placeholder="planner, executor"
            list="team-run-moderator-roles"
          />
          <datalist id="team-run-moderator-roles">
            {props.roleOptions.map((role, idx) => (
              <option key={`moderator-role-${role}-${idx}`} value={role} />
            ))}
          </datalist>
          <FieldLabel>Target members</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.targetMembers}
            onChange={(e) => props.onTargetMembersChange(e.target.value)}
            placeholder="member_id, member_id"
            list="team-run-moderator-members"
          />
          <datalist id="team-run-moderator-members">
            {props.memberOptions.map((mid, idx) => (
              <option key={`moderator-member-${mid}-${idx}`} value={mid} />
            ))}
          </datalist>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Target agents</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.targetAgents}
            onChange={(e) => props.onTargetAgentsChange(e.target.value)}
            placeholder="agent_id, agent_id"
            list="team-run-moderator-agents"
          />
          <datalist id="team-run-moderator-agents">
            {props.agentOptions.map((aid, idx) => (
              <option key={`moderator-agent-${aid}-${idx}`} value={aid} />
            ))}
          </datalist>
          <FieldLabel>Assignees</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.assignees}
            onChange={(e) => props.onAssigneesChange(e.target.value)}
            placeholder="optional assignees"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Directive</FieldLabel>
          <input
            className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.directive}
            onChange={(e) => props.onDirectiveChange(e.target.value)}
            placeholder="Publish a nonblocking directive"
          />
          <FieldLabel>Scope</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.directiveScope}
            onChange={(e) => props.onDirectiveScopeChange(e.target.value)}
            placeholder="optional scope"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={disabled}
            onClick={() => props.onPublishDirective()}
          >
            {props.busy ? "Sending…" : "Send directive"}
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Task title</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.taskTitle}
            onChange={(e) => props.onTaskTitleChange(e.target.value)}
            placeholder="Task title"
          />
          <FieldLabel>Detail</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.taskDetail}
            onChange={(e) => props.onTaskDetailChange(e.target.value)}
            placeholder="Optional details"
          />
          <FieldLabel>Status</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.taskStatus}
            onChange={(e) => props.onTaskStatusChange(e.target.value)}
            placeholder="open"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={disabled}
            onClick={() => props.onPublishTask()}
          >
            {props.busy ? "Publishing…" : "Publish task"}
          </button>
        </div>
        <label className="flex items-center gap-2 text-[11px] text-white/60">
          <input
            type="checkbox"
            className="rounded border-white/20 bg-black/40"
            checked={props.appendToSession}
            onChange={(e) => props.onAppendToSessionChange(e.target.checked)}
          />
          Append to session history
        </label>
        {props.error ? <div className="text-[11px] text-rose-200">{props.error}</div> : null}
        {props.success ? <div className="text-[11px] text-emerald-200">{props.success}</div> : null}
        <div className="mt-2 grid gap-2 rounded-md border border-white/5 bg-black/20 p-2">
          <div className="text-[11px] text-white/60">Moderator events (aggregated)</div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Types</FieldLabel>
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.eventsTypes}
              onChange={(e) => props.onEventsTypesChange(e.target.value)}
              placeholder="moderator_directive,moderator_task_published"
            />
            <FieldLabel>Max bytes</FieldLabel>
            <input
              className="min-w-[100px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.eventsMaxBytes}
              onChange={(e) => props.onEventsMaxBytesChange(e.target.value)}
              placeholder="1048576"
            />
            <FieldLabel>Limit</FieldLabel>
            <input
              className="min-w-[80px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={props.eventsLimit}
              onChange={(e) => props.onEventsLimitChange(e.target.value)}
              placeholder="200"
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={eventsDisabled}
              onClick={() => props.onEventsLoad()}
            >
              {props.eventsBusy ? "Loading…" : "Load events"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={props.events.length === 0}
              onClick={() => props.onEventsToggleExpanded()}
            >
              {props.eventsExpanded ? "Hide JSON" : "Show JSON"}
            </button>
          </div>
          {props.eventsError ? <div className="text-[11px] text-rose-200">{props.eventsError}</div> : null}
          {props.events.length === 0 ? (
            <div className="text-[11px] text-white/50">No events loaded.</div>
          ) : props.eventsExpanded ? (
            <pre className="whitespace-pre-wrap break-words text-[10px] text-white/60">
              {JSON.stringify(props.events, null, 2)}
            </pre>
          ) : (
            <div className="grid gap-1 text-[10px] text-white/70">
              {props.events.map((ev, idx) => {
                const mid = ev?.member_id ? String(ev.member_id) : "";
                const typ = ev?.type ? String(ev.type) : "";
                const ts = ev?.ts_unix_ms ? new Date(Number(ev.ts_unix_ms)).toLocaleString() : "";
                const eventPayload = ev?.event;
                const data =
                  eventPayload && typeof eventPayload === "object" && !Array.isArray(eventPayload)
                    ? (eventPayload as Record<string, unknown>).data
                    : null;
                const summaryData =
                  data && typeof data === "object" && !Array.isArray(data) ? (data as Record<string, unknown>) : null;
                const summary =
                  summaryData
                    ? String(summaryData.directive || summaryData.title || summaryData.detail || "")
                    : "";
                return (
                  <div key={`moderator-event-${mid}-${idx}`} className="flex flex-wrap gap-2">
                    <span>{typ || "event"}</span>
                    {mid ? <span>· {mid}</span> : null}
                    {ts ? <span>· {ts}</span> : null}
                    {summary ? <span className="text-white/60">· {summary}</span> : null}
                  </div>
                );
              })}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
