import React from "react";
import type { ModeratorEvent } from "../../api";
import type { ClientSettings, ConnectionSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";
import { SectionHeader } from "./SettingsControls";

type JsonDiffEntry = { path: string; a: unknown; b: unknown };

function truncateText(value: string, maxLen: number) {
  if (value.length <= maxLen) return value;
  return `${value.slice(0, Math.max(0, maxLen - 1))}…`;
}

function formatDiffValue(value: unknown) {
  if (value === undefined) return "(undefined)";
  if (value === null) return "null";
  if (typeof value === "string") return truncateText(JSON.stringify(value), 200);
  try {
    return truncateText(JSON.stringify(value), 200);
  } catch {
    return truncateText(String(value), 200);
  }
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === "object" && !Array.isArray(value);
}

function collectJsonDiffs(a: unknown, b: unknown, path: string, out: JsonDiffEntry[], maxDiffs: number) {
  if (out.length >= maxDiffs) return;
  if (a === b) return;
  const aIsArr = Array.isArray(a);
  const bIsArr = Array.isArray(b);
  if (aIsArr || bIsArr) {
    if (!aIsArr || !bIsArr) {
      out.push({ path: path || "<root>", a, b });
      return;
    }
    const max = Math.max(a.length, b.length);
    for (let i = 0; i < max; i += 1) {
      collectJsonDiffs(a[i], b[i], `${path}[${i}]`, out, maxDiffs);
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  if (isPlainObject(a) && isPlainObject(b)) {
    const keys = new Set<string>([...Object.keys(a), ...Object.keys(b)]);
    for (const key of keys) {
      const nextPath = path ? `${path}.${key}` : key;
      collectJsonDiffs(a[key], b[key], nextPath, out, maxDiffs);
      if (out.length >= maxDiffs) return;
    }
    return;
  }
  out.push({ path: path || "<root>", a, b });
}

function formatModeratorEventSummary(event: ModeratorEvent) {
  const type = typeof event?.type === "string" ? event.type : "";
  const data = event?.data && typeof event.data === "object" ? (event.data as any) : {};
  if (type === "moderator_directive") {
    const directive = typeof data?.directive === "string" ? data.directive : "";
    return directive || "(directive)";
  }
  if (type === "moderator_task_published") {
    const task = data?.task && typeof data.task === "object" ? data.task : {};
    const title = typeof task?.title === "string" ? task.title : "";
    return title || "(task)";
  }
  return "";
}

type SettingsModeratorSectionProps = {
  connection: ConnectionSettings;
  client: ClientSettings;
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
  moderatorEventsAuto: boolean;
  setModeratorEventsAuto: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsMaxBytes: string;
  setModeratorEventsMaxBytes: React.Dispatch<React.SetStateAction<string>>;
  moderatorEventsIncludeDirectives: boolean;
  setModeratorEventsIncludeDirectives: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsIncludeTasks: boolean;
  setModeratorEventsIncludeTasks: React.Dispatch<React.SetStateAction<boolean>>;
  moderatorEventsFilter: string;
  setModeratorEventsFilter: React.Dispatch<React.SetStateAction<string>>;
  moderatorEventsExpanded: Record<string, boolean>;
  setModeratorEventsExpanded: React.Dispatch<React.SetStateAction<Record<string, boolean>>>;
  brokerAgentOptions: Array<{ id: string; label: string; connected: boolean }>;
  brokerAgentsBusy: boolean;
  listBrokerAgents: () => Promise<void>;
  moderatorRolePresets: string[];
  addDirectiveAssignee: (value: string) => void;
  addTaskAssignee: (value: string) => void;
  applyRuntimeMemberTaskTemplate: () => void;
  publishModeratorDirective: () => Promise<void>;
  publishModeratorTask: () => Promise<void>;
  moderatorEventsEnabled: boolean;
  moderatorEventsRefetch: () => void;
  moderatorEventsFetching: boolean;
  moderatorEventsError: string | null;
  moderatorEventsList: ModeratorEvent[];
  moderatorEventsFiltered: ModeratorEvent[];
  moderatorPinnedEvents: Record<string, ModeratorEvent>;
  moderatorPinnedEntries: Array<[string, ModeratorEvent]>;
  updateModeratorPinnedEvents: (
    updater: Record<string, ModeratorEvent> | ((prev: Record<string, ModeratorEvent>) => Record<string, ModeratorEvent>),
  ) => void;
  pinImportRef: React.RefObject<HTMLInputElement>;
  showPinNotice: (msg: string, ok: boolean) => void;
  handleCopy: (label: string, text: string) => Promise<void>;
  pinnedCompareOptions: Array<{ key: string; label: string }>;
  pinnedCompareA: string;
  setPinnedCompareA: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareB: string;
  setPinnedCompareB: React.Dispatch<React.SetStateAction<string>>;
  pinnedCompareDiffOnly: boolean;
  setPinnedCompareDiffOnly: React.Dispatch<React.SetStateAction<boolean>>;
  copyNotice: string | null;
  pinNotice: string | null;
  pinError: string | null;
  moderatorDirectivesEnabled: boolean;
  moderatorTasksEnabled: boolean;
};

export default function SettingsModeratorSection(props: SettingsModeratorSectionProps) {
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
    moderatorEventsAuto,
    setModeratorEventsAuto,
    moderatorEventsMaxBytes,
    setModeratorEventsMaxBytes,
    moderatorEventsIncludeDirectives,
    setModeratorEventsIncludeDirectives,
    moderatorEventsIncludeTasks,
    setModeratorEventsIncludeTasks,
    moderatorEventsFilter,
    setModeratorEventsFilter,
    moderatorEventsExpanded,
    setModeratorEventsExpanded,
    brokerAgentOptions,
    brokerAgentsBusy,
    listBrokerAgents,
    moderatorRolePresets,
    addDirectiveAssignee,
    addTaskAssignee,
    applyRuntimeMemberTaskTemplate,
    publishModeratorDirective,
    publishModeratorTask,
    moderatorEventsEnabled,
    moderatorEventsRefetch,
    moderatorEventsFetching,
    moderatorEventsError,
    moderatorEventsList,
    moderatorEventsFiltered,
    moderatorPinnedEvents,
    moderatorPinnedEntries,
    updateModeratorPinnedEvents,
    pinImportRef,
    showPinNotice,
    handleCopy,
    pinnedCompareOptions,
    pinnedCompareA,
    setPinnedCompareA,
    pinnedCompareB,
    setPinnedCompareB,
    pinnedCompareDiffOnly,
    setPinnedCompareDiffOnly,
    copyNotice,
    pinNotice,
    pinError,
    moderatorDirectivesEnabled,
    moderatorTasksEnabled,
  } = props;

  const moderatorEventsFilterValue = String(moderatorEventsFilter || "").trim().toLowerCase();

  return (
    <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
      <SectionHeader title="Moderator" />
      <div className="mt-2 grid gap-3 text-[11px] text-white/70">
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
          <div className="mt-1 text-[11px] text-white/50">
            Leave empty to broadcast to all listening agents.
          </div>
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
        <div className="mt-3 rounded-md border border-white/10 bg-black/30 p-2">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div className="text-xs font-semibold text-white/70">Moderator events</div>
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => moderatorEventsRefetch()}
                disabled={!moderatorEventsEnabled || !sessionId.trim() || moderatorEventsFetching}
              >
                {moderatorEventsFetching ? "Loading…" : "Load"}
              </button>
              <label className="flex items-center gap-2">
                <span>auto</span>
                <input
                  type="checkbox"
                  checked={moderatorEventsAuto}
                  onChange={(e) => setModeratorEventsAuto(e.target.checked)}
                  disabled={!moderatorEventsEnabled || !sessionId.trim()}
                />
              </label>
            </div>
          </div>
          <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={moderatorEventsIncludeDirectives}
                onChange={(e) => setModeratorEventsIncludeDirectives(e.target.checked)}
              />
              directives
            </label>
            <label className="flex items-center gap-2">
              <input
                type="checkbox"
                checked={moderatorEventsIncludeTasks}
                onChange={(e) => setModeratorEventsIncludeTasks(e.target.checked)}
              />
              tasks
            </label>
            <label className="flex items-center gap-2">
              <span>max bytes</span>
              <input
                className="w-28 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                value={moderatorEventsMaxBytes}
                onChange={(e) => setModeratorEventsMaxBytes(e.target.value)}
              />
            </label>
            <label className="flex items-center gap-2">
              <span>filter</span>
              <input
                className="w-40 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                value={moderatorEventsFilter}
                onChange={(e) => setModeratorEventsFilter(e.target.value)}
                placeholder="type/actor/text"
              />
            </label>
            {moderatorEventsFilterValue ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => setModeratorEventsFilter("")}
              >
                Clear
              </button>
            ) : null}
          </div>
          {moderatorPinnedEntries.length > 0 ? (
            <div className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
              <div className="flex items-center justify-between gap-2 text-[11px] text-white/70">
                <span>Pinned events ({moderatorPinnedEntries.length})</span>
                <div className="flex flex-wrap items-center gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => {
                      try {
                        const payload = JSON.stringify(moderatorPinnedEvents, null, 2);
                        const blob = new Blob([payload], { type: "application/json" });
                        const url = URL.createObjectURL(blob);
                        const anchor = document.createElement("a");
                        anchor.href = url;
                        anchor.download = `moderator_pins_${Date.now()}.json`;
                        anchor.click();
                        URL.revokeObjectURL(url);
                        showPinNotice("Exported pins", true);
                      } catch (err: any) {
                        showPinNotice(String(err?.message || "export failed"), false);
                      }
                    }}
                  >
                    Export pins
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => pinImportRef.current?.click()}
                  >
                    Import pins
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => updateModeratorPinnedEvents({})}
                  >
                    Clear pins
                  </button>
                </div>
              </div>
              <input
                ref={pinImportRef}
                type="file"
                accept="application/json"
                className="hidden"
                onChange={async (e) => {
                  const file = e.target.files?.[0];
                  if (!file) return;
                  try {
                    const text = await file.text();
                    const parsed = JSON.parse(text);
                    let nextPins: Record<string, ModeratorEvent> = {};
                    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
                      for (const [key, value] of Object.entries(parsed as Record<string, unknown>)) {
                        if (value && typeof value === "object") {
                          nextPins[key] = value as ModeratorEvent;
                        }
                      }
                    } else if (Array.isArray(parsed)) {
                      parsed.forEach((event, idx) => {
                        if (event && typeof event === "object") {
                          nextPins[`import-${idx}`] = event as ModeratorEvent;
                        }
                      });
                    }
                    updateModeratorPinnedEvents(nextPins);
                    showPinNotice("Imported pins", true);
                  } catch (err: any) {
                    showPinNotice(String(err?.message || "import failed"), false);
                  } finally {
                    if (pinImportRef.current) pinImportRef.current.value = "";
                  }
                }}
              />
              <div className="mt-2 grid gap-2">
                {moderatorPinnedEntries.map(([key, event]) => {
                  const type = typeof event?.type === "string" ? event.type : "event";
                  const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
                  const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
                  const actorId = typeof actor?.id === "string" ? actor.id : "";
                  const summary = formatModeratorEventSummary(event);
                  return (
                    <div key={`pinned-${key}`} className="rounded-md border border-white/5 bg-black/30 p-2 text-[11px] text-white/70">
                      <div className="flex flex-wrap items-center justify-between gap-2">
                        <div className="text-white/80">
                          {type}
                          {actorId ? ` · ${actorId}` : ""}
                        </div>
                        <div className="text-white/40">{ts}</div>
                      </div>
                      {summary ? <div className="text-white/60">{summary}</div> : null}
                      <div className="mt-1 flex flex-wrap items-center gap-2">
                        {summary ? (
                          <button
                            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() => void handleCopy("summary", summary)}
                          >
                            Copy summary
                          </button>
                        ) : null}
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() => void handleCopy("JSON", JSON.stringify(event, null, 2))}
                        >
                          Copy JSON
                        </button>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() =>
                            updateModeratorPinnedEvents((prev) => {
                              const next = { ...prev };
                              delete next[key];
                              return next;
                            })
                          }
                        >
                          Unpin
                        </button>
                      </div>
                    </div>
                  );
                })}
              </div>
              {moderatorPinnedEntries.length > 1 ? (
                <div className="mt-3 rounded-md border border-white/10 bg-black/20 p-2">
                  <div className="text-[11px] text-white/70">Compare pinned events</div>
                  <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                    <label className="flex items-center gap-2">
                      <span>A</span>
                      <select
                        className="min-w-[220px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                        value={pinnedCompareA}
                        onChange={(e) => setPinnedCompareA(e.target.value)}
                      >
                        {pinnedCompareOptions.map((opt) => (
                          <option key={`pin-a-${opt.key}`} value={opt.key}>
                            {opt.label}
                          </option>
                        ))}
                      </select>
                    </label>
                    <label className="flex items-center gap-2">
                      <span>B</span>
                      <select
                        className="min-w-[220px] rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
                        value={pinnedCompareB}
                        onChange={(e) => setPinnedCompareB(e.target.value)}
                      >
                        {pinnedCompareOptions.map((opt) => (
                          <option key={`pin-b-${opt.key}`} value={opt.key}>
                            {opt.label}
                          </option>
                        ))}
                      </select>
                    </label>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      onClick={() => {
                        setPinnedCompareA(pinnedCompareB);
                        setPinnedCompareB(pinnedCompareA);
                      }}
                      disabled={!pinnedCompareA || !pinnedCompareB}
                    >
                      Swap
                    </button>
                  </div>
                  {(() => {
                    if (!pinnedCompareA || !pinnedCompareB) return null;
                    const eventA = moderatorPinnedEvents[pinnedCompareA];
                    const eventB = moderatorPinnedEvents[pinnedCompareB];
                    const jsonA = JSON.stringify(eventA, null, 2);
                    const jsonB = JSON.stringify(eventB, null, 2);
                    const same = jsonA === jsonB;
                    const diffText = JSON.stringify({ a: eventA, b: eventB }, null, 2);
                    const diffs: JsonDiffEntry[] = [];
                    if (!same) {
                      collectJsonDiffs(eventA, eventB, "", diffs, 200);
                    }
                    return (
                      <div className="mt-2 grid gap-2">
                        <div className={`text-[11px] ${same ? "text-emerald-200" : "text-amber-200"}`}>
                          {same ? "Pinned events are identical." : "Pinned events differ."}
                        </div>
                        <label className="flex items-center gap-2 text-[11px] text-white/60">
                          <input
                            type="checkbox"
                            checked={pinnedCompareDiffOnly}
                            onChange={(e) => setPinnedCompareDiffOnly(e.target.checked)}
                          />
                          Diff-only view
                        </label>
                        <div className="grid gap-2 md:grid-cols-2">
                          <div className="rounded-md border border-white/10 bg-black/30 p-2">
                            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                              <span>Event A</span>
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopy("JSON A", jsonA)}
                              >
                                Copy JSON A
                              </button>
                            </div>
                            {!pinnedCompareDiffOnly ? (
                              <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">
                                {jsonA}
                              </pre>
                            ) : null}
                          </div>
                          <div className="rounded-md border border-white/10 bg-black/30 p-2">
                            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                              <span>Event B</span>
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopy("JSON B", jsonB)}
                              >
                                Copy JSON B
                              </button>
                            </div>
                            {!pinnedCompareDiffOnly ? (
                              <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">
                                {jsonB}
                              </pre>
                            ) : null}
                          </div>
                        </div>
                        {pinnedCompareDiffOnly ? (
                          <div className="rounded-md border border-white/10 bg-black/30 p-2">
                            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                              <span>Combined diff view</span>
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopy("diff JSON", diffText)}
                              >
                                Copy diff JSON
                              </button>
                            </div>
                            <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap text-[10px] text-white/70">
                              {diffText}
                            </pre>
                          </div>
                        ) : null}
                        {pinnedCompareDiffOnly ? (
                          <div className="rounded-md border border-white/10 bg-black/30 p-2">
                            <div className="flex items-center justify-between gap-2 text-[10px] text-white/60">
                              <span>Key diffs</span>
                              <span>{diffs.length} changes</span>
                            </div>
                            {diffs.length === 0 ? (
                              <div className="mt-2 text-[10px] text-white/40">No key-level differences detected.</div>
                            ) : (
                              <div className="mt-2 max-h-64 overflow-auto">
                                {diffs.slice(0, 200).map((diff, idx) => (
                                  <div key={`diff-${idx}`} className="border-b border-white/5 py-1 last:border-b-0">
                                    <div className="text-[10px] text-white/70">{diff.path}</div>
                                    <div className="mt-1 flex flex-wrap items-center gap-2 text-[10px]">
                                      <span className="rounded-md bg-emerald-500/10 px-2 py-1 text-emerald-200">
                                        A: {formatDiffValue(diff.a)}
                                      </span>
                                      <span className="rounded-md bg-amber-500/10 px-2 py-1 text-amber-200">
                                        B: {formatDiffValue(diff.b)}
                                      </span>
                                    </div>
                                  </div>
                                ))}
                              </div>
                            )}
                            {diffs.length >= 200 ? (
                              <div className="mt-2 text-[10px] text-white/40">Diffs truncated at 200 entries.</div>
                            ) : null}
                          </div>
                        ) : null}
                      </div>
                    );
                  })()}
                </div>
              ) : null}
            </div>
          ) : null}
          {moderatorEventsError ? <div className="mt-2 text-[11px] text-rose-200">{moderatorEventsError}</div> : null}
          {copyNotice ? <div className="mt-2 text-[11px] text-emerald-200">{copyNotice}</div> : null}
          {pinNotice ? <div className="mt-2 text-[11px] text-emerald-200">{pinNotice}</div> : null}
          {pinError ? <div className="mt-2 text-[11px] text-rose-200">{pinError}</div> : null}
          {!moderatorEventsEnabled ? (
            <div className="mt-2 text-[11px] text-amber-200">Moderator events disabled by daemon caps.</div>
          ) : null}
          {sessionId.trim().length === 0 ? (
            <div className="mt-2 text-[11px] text-amber-200">Set a session id to read events.</div>
          ) : null}
          <div className="mt-2 max-h-48 overflow-auto rounded-md border border-white/10 bg-black/20 p-2 text-[11px] text-white/70">
            {moderatorEventsFiltered.length === 0 ? (
              <div className="text-white/40">
                {moderatorEventsList.length === 0 ? "No events loaded yet." : "No events match the filter."}
              </div>
            ) : (
              moderatorEventsFiltered.map((event, idx) => {
                const type = typeof event?.type === "string" ? event.type : "event";
                const ts = typeof event?.ts_unix_ms === "number" ? new Date(event.ts_unix_ms).toLocaleString() : "";
                const actor = event?.actor && typeof event.actor === "object" ? (event.actor as any) : {};
                const actorId = typeof actor?.id === "string" ? actor.id : "";
                const summary = formatModeratorEventSummary(event);
                const key = `${type}-${event?.ts_unix_ms ?? "0"}-${idx}`;
                const isPinned = !!moderatorPinnedEvents[key];
                const isExpanded = moderatorEventsExpanded[key] === true;
                return (
                  <div key={key} className="border-b border-white/5 py-1 last:border-b-0">
                    <div className="flex flex-wrap items-center justify-between gap-2">
                      <div className="text-white/80">
                        {type}
                        {actorId ? ` · ${actorId}` : ""}
                      </div>
                      <div className="flex items-center gap-2 text-white/40">
                        <span>{ts}</span>
                        {summary ? (
                          <button
                            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                            type="button"
                            onClick={() => void handleCopy("summary", summary)}
                          >
                            Copy summary
                          </button>
                        ) : null}
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() => void handleCopy("JSON", JSON.stringify(event, null, 2))}
                        >
                          Copy JSON
                        </button>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() =>
                            updateModeratorPinnedEvents((prev) => {
                              const next = { ...prev };
                              if (next[key]) {
                                delete next[key];
                              } else {
                                next[key] = event;
                              }
                              return next;
                            })
                          }
                        >
                          {isPinned ? "Unpin" : "Pin"}
                        </button>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() =>
                            setModeratorEventsExpanded((prev) => ({
                              ...prev,
                              [key]: !isExpanded,
                            }))
                          }
                        >
                          {isExpanded ? "Hide JSON" : "Show JSON"}
                        </button>
                      </div>
                    </div>
                    {summary ? <div className="text-white/60">{summary}</div> : null}
                    {isExpanded ? (
                      <pre className="mt-2 max-h-64 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/40 p-2 text-[10px] text-white/70">
                        {JSON.stringify(event, null, 2)}
                      </pre>
                    ) : null}
                  </div>
                );
              })
            )}
          </div>
        </div>
        <div className="text-[11px] text-white/50">
          Moderator directives/tasks are stored as client events. Assignees and scope are advisory hints; empty assignees
          broadcast to all listening agents.
        </div>
      </div>
    </div>
  );
}
