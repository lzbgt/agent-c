import React from "react";
import FieldLabel from "../FieldLabel";
import {
  apiBrokerTeamRunGoalUpdate,
  apiBrokerTeamRunHandoff,
  type ApiAuth,
} from "../../api";

type MemberSession = {
  memberId: string;
  sessionId: string;
};

type TeamRunStatusPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  runId: string;
  runLookupResult: any | null;
  fmtTs: (ms?: number | null) => string;
  fmtSummary: (summary?: any) => string;
  runtimeUpdateBusy: boolean;
  memberSessions: MemberSession[];
  onRuntimeMemberToggle: (member: any) => Promise<void> | void;
  onRuntimeMemberRemove: (member: any) => Promise<void> | void;
  onRefreshRun: (runId: string) => Promise<void> | void;
};

const parseLineList = (raw: string): string[] =>
  String(raw || "")
    .split(/[\n,]+/)
    .map((item) => item.trim())
    .filter(Boolean);

export default function TeamRunStatusPanel(props: TeamRunStatusPanelProps) {
  const run = props.runLookupResult;
  const runId = String(props.runId || run?.team_run_id || "").trim();
  const teamId = String(props.teamId || "").trim();
  const canWrite = props.canQuery && !!teamId && !!runId;

  const goalContract = run?.goal_contract && typeof run.goal_contract === "object" ? run.goal_contract : null;
  const goalEvents = Array.isArray(run?.goal_events) ? run.goal_events : [];
  const handoffEvents = Array.isArray(run?.handoff_events) ? run.handoff_events : [];
  const sharedMemoryScope = run?.shared_memory_scope_id ? String(run.shared_memory_scope_id) : "";
  const sharedMemoryMode = run?.shared_memory_mode ? String(run.shared_memory_mode) : "";
  const autoAllocateRoles = run?.auto_allocate_roles === true;
  const autoAllocateAllocated = Array.isArray(run?.auto_allocate_allocated_roles)
    ? run.auto_allocate_allocated_roles
    : [];
  const autoAllocateMissing = Array.isArray(run?.auto_allocate_missing_roles) ? run.auto_allocate_missing_roles : [];
  const autoAllocateWarning = run?.auto_allocate_warning ? String(run.auto_allocate_warning) : "";

  const lastInitRunId = React.useRef<string>("");
  const [goalContractGoal, setGoalContractGoal] = React.useState<string>("");
  const [goalContractCriteria, setGoalContractCriteria] = React.useState<string>("");
  const [goalContractConstraints, setGoalContractConstraints] = React.useState<string>("");
  const [goalEventType, setGoalEventType] = React.useState<string>("progress");
  const [goalEventMessage, setGoalEventMessage] = React.useState<string>("");
  const [goalEventData, setGoalEventData] = React.useState<string>("");
  const [goalUpdateBusy, setGoalUpdateBusy] = React.useState<boolean>(false);
  const [goalUpdateError, setGoalUpdateError] = React.useState<string | null>(null);
  const [goalUpdateNote, setGoalUpdateNote] = React.useState<string | null>(null);

  const [handoffFromRole, setHandoffFromRole] = React.useState<string>("");
  const [handoffToRole, setHandoffToRole] = React.useState<string>("");
  const [handoffReason, setHandoffReason] = React.useState<string>("");
  const [handoffMessage, setHandoffMessage] = React.useState<string>("");
  const [handoffData, setHandoffData] = React.useState<string>("");
  const [handoffBusy, setHandoffBusy] = React.useState<boolean>(false);
  const [handoffError, setHandoffError] = React.useState<string | null>(null);
  const [handoffNote, setHandoffNote] = React.useState<string | null>(null);

  React.useEffect(() => {
    if (!runId) return;
    if (lastInitRunId.current === runId) return;
    lastInitRunId.current = runId;
    const goal = goalContract && typeof goalContract.goal === "string" ? goalContract.goal : "";
    const criteria = Array.isArray(goalContract?.success_criteria) ? goalContract.success_criteria : [];
    const constraints = Array.isArray(goalContract?.constraints) ? goalContract.constraints : [];
    setGoalContractGoal(goal);
    setGoalContractCriteria(criteria.join("\n"));
    setGoalContractConstraints(constraints.join("\n"));
    setGoalUpdateError(null);
    setGoalUpdateNote(null);
    setHandoffError(null);
    setHandoffNote(null);
  }, [runId, goalContract]);

  const handleGoalContractUpdate = async () => {
    setGoalUpdateError(null);
    setGoalUpdateNote(null);
    if (!canWrite) {
      setGoalUpdateError("missing team or run id");
      return;
    }
    const goal = goalContractGoal.trim();
    const criteria = parseLineList(goalContractCriteria);
    const constraints = parseLineList(goalContractConstraints);
    if (!goal && criteria.length === 0 && constraints.length === 0) {
      setGoalUpdateError("goal contract is empty");
      return;
    }
    const contract: Record<string, any> = {};
    if (goal) contract.goal = goal;
    if (criteria.length > 0) contract.success_criteria = criteria;
    if (constraints.length > 0) contract.constraints = constraints;
    setGoalUpdateBusy(true);
    try {
      const resp = await apiBrokerTeamRunGoalUpdate(props.base, teamId, runId, { goal_contract: contract }, props.auth);
      if (!resp.ok) {
        setGoalUpdateError(resp.error || resp.err || "goal update failed");
        return;
      }
      setGoalUpdateNote("goal contract updated");
      await props.onRefreshRun(runId);
    } catch (err) {
      setGoalUpdateError(String(err));
    } finally {
      setGoalUpdateBusy(false);
    }
  };

  const handleGoalEvent = async () => {
    setGoalUpdateError(null);
    setGoalUpdateNote(null);
    if (!canWrite) {
      setGoalUpdateError("missing team or run id");
      return;
    }
    const eventType = String(goalEventType || "").trim().toLowerCase();
    if (eventType !== "progress" && eventType !== "drift") {
      setGoalUpdateError("goal event type must be progress or drift");
      return;
    }
    let dataObj: Record<string, any> | undefined;
    const rawData = goalEventData.trim();
    if (rawData) {
      try {
        const parsed = JSON.parse(rawData);
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          setGoalUpdateError("goal event data must be a JSON object");
          return;
        }
        dataObj = parsed as Record<string, any>;
      } catch (err) {
        setGoalUpdateError(`goal event data invalid json: ${String(err)}`);
        return;
      }
    }
    const event: Record<string, any> = { type: eventType };
    const message = goalEventMessage.trim();
    if (message) event.message = message;
    if (dataObj) event.data = dataObj;
    setGoalUpdateBusy(true);
    try {
      const resp = await apiBrokerTeamRunGoalUpdate(props.base, teamId, runId, { event }, props.auth);
      if (!resp.ok) {
        setGoalUpdateError(resp.error || resp.err || "goal event failed");
        return;
      }
      setGoalUpdateNote(`goal ${eventType} event emitted`);
      await props.onRefreshRun(runId);
    } catch (err) {
      setGoalUpdateError(String(err));
    } finally {
      setGoalUpdateBusy(false);
    }
  };

  const handleHandoffEvent = async () => {
    setHandoffError(null);
    setHandoffNote(null);
    if (!canWrite) {
      setHandoffError("missing team or run id");
      return;
    }
    const fromRole = handoffFromRole.trim();
    const toRole = handoffToRole.trim();
    if (!fromRole || !toRole) {
      setHandoffError("handoff requires from_role and to_role");
      return;
    }
    let dataObj: Record<string, any> | undefined;
    const rawData = handoffData.trim();
    if (rawData) {
      try {
        const parsed = JSON.parse(rawData);
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          setHandoffError("handoff data must be a JSON object");
          return;
        }
        dataObj = parsed as Record<string, any>;
      } catch (err) {
        setHandoffError(`handoff data invalid json: ${String(err)}`);
        return;
      }
    }
    const event: Record<string, any> = { from_role: fromRole, to_role: toRole };
    const reason = handoffReason.trim();
    const message = handoffMessage.trim();
    if (reason) event.reason = reason;
    if (message) event.message = message;
    if (dataObj) event.data = dataObj;
    setHandoffBusy(true);
    try {
      const resp = await apiBrokerTeamRunHandoff(props.base, teamId, runId, { event }, props.auth);
      if (!resp.ok) {
        setHandoffError(resp.error || resp.err || "handoff event failed");
        return;
      }
      setHandoffNote("handoff event emitted");
      await props.onRefreshRun(runId);
    } catch (err) {
      setHandoffError(String(err));
    } finally {
      setHandoffBusy(false);
    }
  };

  if (!run) {
    return <div className="text-[11px] text-white/50">No run loaded yet.</div>;
  }

  const goalEventRows = goalEvents.slice(-6).reverse();
  const handoffEventRows = handoffEvents.slice(-6).reverse();

  return (
    <div className="grid gap-2">
      <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
        status {String(run?.status || "")}
        {run?.created_unix_ms ? ` · ${props.fmtTs(run.created_unix_ms)}` : ""}
        {run?.mode ? ` · mode ${String(run.mode)}` : ""}
        {typeof run?.cancel_requested_unix_ms === "number"
          ? ` · cancel requested ${props.fmtTs(run.cancel_requested_unix_ms)}`
          : ""}
      </div>
      {run?.run_overrides_mode ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          overrides mode: {String(run.run_overrides_mode)}
        </div>
      ) : null}
      {sharedMemoryScope ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          shared memory: {sharedMemoryScope}
          {sharedMemoryMode ? ` (${sharedMemoryMode})` : ""}
        </div>
      ) : null}
      {autoAllocateRoles ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          auto-allocate: on
          {autoAllocateAllocated.length > 0 ? ` · allocated ${autoAllocateAllocated.join(", ")}` : ""}
          {autoAllocateMissing.length > 0 ? ` · missing ${autoAllocateMissing.join(", ")}` : ""}
          {autoAllocateWarning ? ` · ${autoAllocateWarning}` : ""}
        </div>
      ) : null}
      {run?.role_overrides_applied || run?.member_overrides_applied ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          <div className="text-[11px] text-white/80">applied overrides</div>
          <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/60">
            {JSON.stringify(
              {
                role_overrides_applied: run?.role_overrides_applied ?? null,
                member_overrides_applied: run?.member_overrides_applied ?? null,
              },
              null,
              2,
            )}
          </pre>
        </div>
      ) : null}
      {props.fmtSummary(run?.member_job_summary) ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          summary: {props.fmtSummary(run?.member_job_summary)}
        </div>
      ) : null}
      {Array.isArray(run?.dispatch_errors) && run.dispatch_errors.length > 0 ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-200">
          dispatch errors:
          {run.dispatch_errors.map((err: any, idx: number) => {
            const mid = err?.member_id ? String(err.member_id) : "";
            const aid = err?.agent_id ? String(err.agent_id) : "";
            const msg = err?.error ? String(err.error) : "dispatch error";
            return (
              <div key={`dispatch-error-${mid || aid}-${idx}`}>
                {mid ? `${mid} · ` : ""}
                {aid ? `agent ${aid}` : "agent ?"} · {msg}
              </div>
            );
          })}
        </div>
      ) : null}
      {Array.isArray(run?.cancel_results) && run.cancel_results.length > 0 ? (
        <div className="rounded-md border border-amber-400/20 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          cancel results:
          {run.cancel_results.map((row: any, idx: number) => {
            const mid = row?.member_id ? String(row.member_id) : "";
            const aid = row?.agent_id ? String(row.agent_id) : "";
            const jobId = row?.job_id ? String(row.job_id) : "";
            const ok = typeof row?.ok === "boolean" ? String(row.ok) : "";
            const http = typeof row?.http_status === "number" ? `http ${row.http_status}` : "";
            const err = row?.error ? String(row.error) : "";
            return (
              <div key={`cancel-result-${mid || aid}-${idx}`}>
                {mid ? `${mid} · ` : ""}
                {aid ? `agent ${aid}` : "agent ?"}
                {jobId ? ` · job ${jobId}` : ""}
                {ok ? ` · ok ${ok}` : ""}
                {http ? ` · ${http}` : ""}
                {err ? ` · ${err}` : ""}
              </div>
            );
          })}
        </div>
      ) : null}
      {Array.isArray(run?.member_jobs) && run.member_jobs.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          member jobs:
          {run.member_jobs.map((job: any, idx: number) => {
            const mid = job?.member_id ? String(job.member_id) : "";
            const aid = job?.agent_id ? String(job.agent_id) : "";
            const jobId = job?.job_id ? String(job.job_id) : "";
            const status = job?.status ? String(job.status) : "";
            const ok = typeof job?.ok === "boolean" ? String(job.ok) : "";
            const err = job?.error ? String(job.error) : job?.dispatch_error ? String(job.dispatch_error) : "";
            const updated = typeof job?.updated_unix_ms === "number" ? props.fmtTs(job.updated_unix_ms) : "";
            return (
              <div key={`member-job-${mid || aid}-${idx}`} className="mt-1">
                <div>
                  {mid ? `${mid} · ` : ""}
                  {aid ? `agent ${aid}` : "agent ?"}
                  {jobId ? ` · job ${jobId}` : ""}
                </div>
                <div className="text-[10px] text-white/50">
                  {status ? `status ${status}` : ""}
                  {ok ? ` · ok ${ok}` : ""}
                  {updated ? ` · ${updated}` : ""}
                  {err ? ` · ${err}` : ""}
                </div>
              </div>
            );
          })}
        </div>
      ) : null}
      {props.memberSessions.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          member sessions:
          {props.memberSessions.map((row, idx) => (
            <div key={`member-session-${row.memberId}-${idx}`} className="mt-1 text-[10px] text-white/60">
              {row.memberId} · {row.sessionId}
            </div>
          ))}
        </div>
      ) : null}
      {Array.isArray(run?.runtime_members) && run.runtime_members.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          runtime members:
          {run.runtime_members.map((m: any, idx: number) => {
            const agentId = m?.agent_id ? String(m.agent_id) : "";
            const role = m?.role ? String(m.role) : "";
            const mid = m?.member_id ? String(m.member_id) : "";
            const status = m?.status ? String(m.status) : "";
            const rawKey = mid || agentId || `row-${idx}`;
            const runtimeKey = rawKey.replace(/[^a-zA-Z0-9_-]/g, "_");
            return (
              <div
                key={`runtime-member-${mid || agentId}-${idx}`}
                data-testid={`team-run-runtime-row-${runtimeKey}`}
                className="flex flex-wrap items-center justify-between gap-2"
              >
                <div>
                  {mid ? `${mid} · ` : ""}
                  {agentId ? `agent ${agentId}` : "agent ?"}
                  {role ? ` · role ${role}` : ""}
                  {status ? ` · ${status}` : ""}
                </div>
                <div className="flex items-center gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    data-testid={`team-run-runtime-toggle-${runtimeKey}`}
                    disabled={!props.canQuery || props.runtimeUpdateBusy}
                    onClick={() => void props.onRuntimeMemberToggle(m)}
                  >
                    {String(status || "active").toLowerCase() === "paused" ? "Resume" : "Pause"}
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    data-testid={`team-run-runtime-remove-${runtimeKey}`}
                    disabled={!props.canQuery || props.runtimeUpdateBusy}
                    onClick={() => void props.onRuntimeMemberRemove(m)}
                  >
                    Remove
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      ) : null}

      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Goal contract</div>
        {goalContract ? (
          <div className="text-[11px] text-white/60">
            <div>goal: {typeof goalContract.goal === "string" ? goalContract.goal : "(missing)"}</div>
            {Array.isArray(goalContract.success_criteria) && goalContract.success_criteria.length > 0 ? (
              <div>success: {goalContract.success_criteria.join(" · ")}</div>
            ) : null}
            {Array.isArray(goalContract.constraints) && goalContract.constraints.length > 0 ? (
              <div>constraints: {goalContract.constraints.join(" · ")}</div>
            ) : null}
            {typeof run?.goal_updated_unix_ms === "number" ? (
              <div>updated: {props.fmtTs(run.goal_updated_unix_ms)}</div>
            ) : null}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No goal contract recorded.</div>
        )}
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Goal</FieldLabel>
            <input
              className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={goalContractGoal}
              onChange={(e) => setGoalContractGoal(e.target.value)}
              placeholder="Primary run goal"
              disabled={!canWrite || goalUpdateBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Success criteria (one per line)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={goalContractCriteria}
              onChange={(e) => setGoalContractCriteria(e.target.value)}
              disabled={!canWrite || goalUpdateBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Constraints (one per line)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={goalContractConstraints}
              onChange={(e) => setGoalContractConstraints(e.target.value)}
              disabled={!canWrite || goalUpdateBusy}
            />
          </div>
          <button
            className="self-start rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canWrite || goalUpdateBusy}
            onClick={() => void handleGoalContractUpdate()}
          >
            {goalUpdateBusy ? "Updating..." : "Apply goal contract"}
          </button>
          {goalUpdateError ? <div className="text-[11px] text-rose-200">{goalUpdateError}</div> : null}
          {goalUpdateNote ? <div className="text-[11px] text-white/60">{goalUpdateNote}</div> : null}
        </div>
      </div>

      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Goal events (progress / drift)</div>
        {goalEventRows.length > 0 ? (
          <div className="grid gap-1 text-[11px] text-white/60">
            {goalEventRows.map((ev: any, idx: number) => {
              const evType = ev?.type ? String(ev.type) : "event";
              const ts = typeof ev?.ts_unix_ms === "number" ? props.fmtTs(ev.ts_unix_ms) : "";
              const msg = ev?.message ? String(ev.message) : "";
              return (
                <div key={`goal-event-${ev?.event_index || idx}`} className="rounded-md border border-white/5 bg-black/20 px-2 py-1">
                  <div className="text-[11px] text-white/70">
                    {evType}
                    {ts ? ` · ${ts}` : ""}
                    {ev?.event_index ? ` · #${ev.event_index}` : ""}
                  </div>
                  {msg ? <div className="text-[11px] text-white/60">{msg}</div> : null}
                  {ev?.data ? (
                    <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/50">
                      {JSON.stringify(ev.data, null, 2)}
                    </pre>
                  ) : null}
                </div>
              );
            })}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No goal events yet.</div>
        )}
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Type</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={goalEventType}
              onChange={(e) => setGoalEventType(e.target.value)}
              disabled={!canWrite || goalUpdateBusy}
            >
              <option value="progress">progress</option>
              <option value="drift">drift</option>
            </select>
            <FieldLabel>Message</FieldLabel>
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={goalEventMessage}
              onChange={(e) => setGoalEventMessage(e.target.value)}
              placeholder="checkpoint or drift note"
              disabled={!canWrite || goalUpdateBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Data (JSON object, optional)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={goalEventData}
              onChange={(e) => setGoalEventData(e.target.value)}
              placeholder='{"evidence":"artifact:..."}'
              disabled={!canWrite || goalUpdateBusy}
            />
          </div>
          <button
            className="self-start rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canWrite || goalUpdateBusy}
            onClick={() => void handleGoalEvent()}
          >
            {goalUpdateBusy ? "Sending..." : "Emit goal event"}
          </button>
        </div>
      </div>

      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Role handoff events</div>
        {handoffEventRows.length > 0 ? (
          <div className="grid gap-1 text-[11px] text-white/60">
            {handoffEventRows.map((ev: any, idx: number) => {
              const fromRole = ev?.from_role ? String(ev.from_role) : "";
              const toRole = ev?.to_role ? String(ev.to_role) : "";
              const ts = typeof ev?.ts_unix_ms === "number" ? props.fmtTs(ev.ts_unix_ms) : "";
              const reason = ev?.reason ? String(ev.reason) : "";
              const msg = ev?.message ? String(ev.message) : "";
              return (
                <div key={`handoff-${ev?.event_index || idx}`} className="rounded-md border border-white/5 bg-black/20 px-2 py-1">
                  <div className="text-[11px] text-white/70">
                    {fromRole || "role"} {"->"} {toRole || "role"}
                    {ts ? ` · ${ts}` : ""}
                    {ev?.event_index ? ` · #${ev.event_index}` : ""}
                  </div>
                  {reason ? <div className="text-[11px] text-white/60">reason: {reason}</div> : null}
                  {msg ? <div className="text-[11px] text-white/60">{msg}</div> : null}
                  {ev?.data ? (
                    <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/50">
                      {JSON.stringify(ev.data, null, 2)}
                    </pre>
                  ) : null}
                </div>
              );
            })}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No handoff events yet.</div>
        )}
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>From</FieldLabel>
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffFromRole}
              onChange={(e) => setHandoffFromRole(e.target.value)}
              placeholder="planner"
              disabled={!canWrite || handoffBusy}
            />
            <FieldLabel>To</FieldLabel>
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffToRole}
              onChange={(e) => setHandoffToRole(e.target.value)}
              placeholder="executor"
              disabled={!canWrite || handoffBusy}
            />
            <FieldLabel>Reason</FieldLabel>
            <input
              className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffReason}
              onChange={(e) => setHandoffReason(e.target.value)}
              placeholder="optional"
              disabled={!canWrite || handoffBusy}
            />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Message</FieldLabel>
            <input
              className="min-w-[240px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffMessage}
              onChange={(e) => setHandoffMessage(e.target.value)}
              placeholder="handoff note"
              disabled={!canWrite || handoffBusy}
            />
          </div>
          <div className="grid gap-1">
            <FieldLabel>Data (JSON object, optional)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
              value={handoffData}
              onChange={(e) => setHandoffData(e.target.value)}
              placeholder='{"artifact":"..."}'
              disabled={!canWrite || handoffBusy}
            />
          </div>
          <button
            className="self-start rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canWrite || handoffBusy}
            onClick={() => void handleHandoffEvent()}
          >
            {handoffBusy ? "Sending..." : "Emit handoff"}
          </button>
          {handoffError ? <div className="text-[11px] text-rose-200">{handoffError}</div> : null}
          {handoffNote ? <div className="text-[11px] text-white/60">{handoffNote}</div> : null}
        </div>
      </div>
    </div>
  );
}
