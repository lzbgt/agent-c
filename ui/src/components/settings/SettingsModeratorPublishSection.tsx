import React from "react";
import type { ConnectionSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";

type SettingsModeratorPublishSectionProps = {
  connection: ConnectionSettings;
  sessionId: string;
  moderatorDirective: string;
  setModeratorDirective: React.Dispatch<React.SetStateAction<string>>;
  moderatorDirectiveScope: string;
  setModeratorDirectiveScope: React.Dispatch<React.SetStateAction<string>>;
  moderatorDirectiveAssignees: string;
  setModeratorDirectiveAssignees: React.Dispatch<React.SetStateAction<string>>;
  moderatorDirectivePick: string;
  setModeratorDirectivePick: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskTitle: string;
  setModeratorTaskTitle: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskDetail: string;
  setModeratorTaskDetail: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskAssignees: string;
  setModeratorTaskAssignees: React.Dispatch<React.SetStateAction<string>>;
  moderatorTaskPick: string;
  setModeratorTaskPick: React.Dispatch<React.SetStateAction<string>>;
  moderatorAppendToSession: boolean;
  setModeratorAppendToSession: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorBusy: boolean;
  moderatorError: string | null;
  moderatorSuccess: string | null;
  brokerAgentOptions: Array<{ id: string; label: string; connected: boolean }>;
  brokerAgentsBusy: boolean;
  listBrokerAgents: () => Promise<void>;
  moderatorRolePresets: string[];
  addDirectiveAssignee: (value: string) => void;
  addTaskAssignee: (value: string) => void;
  applyRuntimeMemberTaskTemplate: () => void;
  publishModeratorDirective: () => Promise<void>;
  publishModeratorTask: () => Promise<void>;
  moderatorDirectivesEnabled: boolean;
  moderatorTasksEnabled: boolean;
};

export default function SettingsModeratorPublishSection(props: SettingsModeratorPublishSectionProps) {
  const {
    connection,
    sessionId,
    moderatorDirective,
    setModeratorDirective,
    moderatorDirectiveScope,
    setModeratorDirectiveScope,
    moderatorDirectiveAssignees,
    setModeratorDirectiveAssignees,
    moderatorDirectivePick,
    setModeratorDirectivePick,
    moderatorTaskTitle,
    setModeratorTaskTitle,
    moderatorTaskDetail,
    setModeratorTaskDetail,
    moderatorTaskAssignees,
    setModeratorTaskAssignees,
    moderatorTaskPick,
    setModeratorTaskPick,
    moderatorAppendToSession,
    setModeratorAppendToSession,
    moderatorBusy,
    moderatorError,
    moderatorSuccess,
    brokerAgentOptions,
    brokerAgentsBusy,
    listBrokerAgents,
    moderatorRolePresets,
    addDirectiveAssignee,
    addTaskAssignee,
    applyRuntimeMemberTaskTemplate,
    publishModeratorDirective,
    publishModeratorTask,
    moderatorDirectivesEnabled,
    moderatorTasksEnabled,
  } = props;

  return (
    <div className="grid gap-3 text-[11px] text-white/70">
      <div>
        <FieldLabel>Directive</FieldLabel>
        <textarea
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder="Publish a nonblocking moderator directive"
          rows={3}
          value={moderatorDirective}
          onChange={(e) => setModeratorDirective(e.target.value)}
        />
      </div>
      <div>
        <FieldLabel>Directive scope (optional)</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder="e.g. all, team:ops, agent:planner"
          value={moderatorDirectiveScope}
          onChange={(e) => setModeratorDirectiveScope(e.target.value)}
        />
        <div className="mt-1 text-[11px] text-white/50">
          Scope is an advisory label used by collaborating agents to route directives.
        </div>
      </div>
      <div>
        <FieldLabel>Directive assignees (comma-separated, optional)</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder="agent-a, agent-b, role:planner"
          value={moderatorDirectiveAssignees}
          onChange={(e) => setModeratorDirectiveAssignees(e.target.value)}
        />
        <div className="mt-1 text-[11px] text-white/50">Leave empty to broadcast to all listening agents.</div>
        <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <span>Quick roles:</span>
          {moderatorRolePresets.map((role) => (
            <button
              key={`moderator-directive-role-${role}`}
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => addDirectiveAssignee(role)}
            >
              {role}
            </button>
          ))}
        </div>
        {connection.mode === "broker" ? (
          <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <select
              className="min-w-[200px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={moderatorDirectivePick}
              onChange={(e) => setModeratorDirectivePick(e.target.value)}
              disabled={brokerAgentOptions.length === 0}
            >
              <option value="">(pick broker agent)</option>
              {brokerAgentOptions.map((opt) => (
                <option key={`moderator-directive-agent-${opt.id}`} value={opt.id}>
                  {opt.label}
                </option>
              ))}
            </select>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => addDirectiveAssignee(moderatorDirectivePick)}
              disabled={!moderatorDirectivePick}
            >
              Add
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void listBrokerAgents()}
              disabled={brokerAgentsBusy}
            >
              {brokerAgentsBusy ? "Refreshing…" : "Refresh agents"}
            </button>
          </div>
        ) : null}
      </div>
      <div>
        <FieldLabel>Task</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder="Task title"
          value={moderatorTaskTitle}
          onChange={(e) => setModeratorTaskTitle(e.target.value)}
        />
        <textarea
          className="mt-2 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder="Task detail (optional)"
          rows={2}
          value={moderatorTaskDetail}
          onChange={(e) => setModeratorTaskDetail(e.target.value)}
        />
        <div className="mt-2">
          <FieldLabel>Task assignees (comma-separated, optional)</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            placeholder="agent-a, agent-b, role:executor"
            value={moderatorTaskAssignees}
            onChange={(e) => setModeratorTaskAssignees(e.target.value)}
          />
          <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <span>Quick roles:</span>
            {moderatorRolePresets.map((role) => (
              <button
                key={`moderator-task-role-${role}`}
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => addTaskAssignee(role)}
              >
                {role}
              </button>
            ))}
          </div>
          {connection.mode === "broker" ? (
            <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              <select
                className="min-w-[200px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={moderatorTaskPick}
                onChange={(e) => setModeratorTaskPick(e.target.value)}
                disabled={brokerAgentOptions.length === 0}
              >
                <option value="">(pick broker agent)</option>
                {brokerAgentOptions.map((opt) => (
                  <option key={`moderator-task-agent-${opt.id}`} value={opt.id}>
                    {opt.label}
                  </option>
                ))}
              </select>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => addTaskAssignee(moderatorTaskPick)}
                disabled={!moderatorTaskPick}
              >
                Add
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => void listBrokerAgents()}
                disabled={brokerAgentsBusy}
              >
                {brokerAgentsBusy ? "Refreshing…" : "Refresh agents"}
              </button>
            </div>
          ) : null}
        </div>
        <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => applyRuntimeMemberTaskTemplate()}
            disabled={moderatorBusy || !moderatorTasksEnabled}
          >
            Insert runtime member update template
          </button>
        </div>
      </div>
      <label className="flex items-center justify-between gap-2">
        <span>Append to session history</span>
        <input
          type="checkbox"
          checked={moderatorAppendToSession}
          onChange={(e) => setModeratorAppendToSession(e.target.checked)}
        />
      </label>
      <div className="flex flex-wrap gap-2">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
          type="button"
          onClick={() => void publishModeratorDirective()}
          disabled={moderatorBusy || !sessionId.trim() || !moderatorDirectivesEnabled}
        >
          Publish directive
        </button>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
          type="button"
          onClick={() => void publishModeratorTask()}
          disabled={moderatorBusy || !sessionId.trim() || !moderatorTasksEnabled}
        >
          Publish task
        </button>
        {moderatorBusy ? <span className="text-white/50">publishing…</span> : null}
      </div>
      {!moderatorDirectivesEnabled || !moderatorTasksEnabled ? (
        <div className="text-amber-200">Moderator publishing disabled by daemon caps.</div>
      ) : null}
      {moderatorError ? <div className="text-rose-200">moderator error: {moderatorError}</div> : null}
      {moderatorSuccess ? <div className="text-emerald-200">{moderatorSuccess}</div> : null}
    </div>
  );
}
