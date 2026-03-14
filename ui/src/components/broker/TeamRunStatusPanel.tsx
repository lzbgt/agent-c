import React from "react";
import FieldLabel from "../FieldLabel";
import {
  apiBrokerTeamRunGoalUpdate,
  apiBrokerTeamRunHandoff,
  type ApiAuth,
} from "../../api";
import RoleGraphPreview from "./RoleGraphPreview";
import { normalizeRoleGraphEdges, normalizeRoleInstructionMap } from "./teamRunUtils";

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

const HANDOFF_STATES = new Set(["proposed", "accepted", "declined", "cancelled"]);

type TeamRunHandoffEventRecord = {
  handoff_id?: string;
  kind?: string;
  state?: string;
  from_role?: string;
  to_role?: string;
  reason?: string;
  message?: string;
  source_deployment_id?: string;
  source_session_id?: string;
  target_deployment_id?: string;
  target_session_id?: string;
  ts_unix_ms?: number;
  event_index?: number;
  data?: Record<string, unknown>;
};

function normalizeHandoffEventRecord(raw: any): TeamRunHandoffEventRecord {
  if (!raw || typeof raw !== "object") return {};
  const obj = raw as Record<string, unknown>;
  const kind = typeof obj.kind === "string" && obj.kind.trim() ? obj.kind.trim().toLowerCase() : "role";
  const state = typeof obj.state === "string" && obj.state.trim() ? obj.state.trim().toLowerCase() : "proposed";
  return {
    handoff_id: typeof obj.handoff_id === "string" ? obj.handoff_id.trim() : undefined,
    kind,
    state: HANDOFF_STATES.has(state) ? state : "proposed",
    from_role: typeof obj.from_role === "string" ? obj.from_role.trim() : undefined,
    to_role: typeof obj.to_role === "string" ? obj.to_role.trim() : undefined,
    reason: typeof obj.reason === "string" ? obj.reason.trim() : undefined,
    message: typeof obj.message === "string" ? obj.message.trim() : undefined,
    source_deployment_id:
      typeof obj.source_deployment_id === "string" ? obj.source_deployment_id.trim() : undefined,
    source_session_id: typeof obj.source_session_id === "string" ? obj.source_session_id.trim() : undefined,
    target_deployment_id:
      typeof obj.target_deployment_id === "string" ? obj.target_deployment_id.trim() : undefined,
    target_session_id: typeof obj.target_session_id === "string" ? obj.target_session_id.trim() : undefined,
    ts_unix_ms: typeof obj.ts_unix_ms === "number" ? obj.ts_unix_ms : undefined,
    event_index: typeof obj.event_index === "number" ? obj.event_index : undefined,
    data: obj.data && typeof obj.data === "object" && !Array.isArray(obj.data) ? (obj.data as Record<string, unknown>) : undefined,
  };
}

export default function TeamRunStatusPanel(props: TeamRunStatusPanelProps) {
  const run = props.runLookupResult;
  const runId = String(props.runId || run?.team_run_id || "").trim();
  const teamId = String(props.teamId || "").trim();
  const canWrite = props.canQuery && !!teamId && !!runId;

  const goalContract = run?.goal_contract && typeof run.goal_contract === "object" ? run.goal_contract : null;
  const goalEvents = Array.isArray(run?.goal_events) ? run.goal_events : [];
  const handoffEvents = Array.isArray(run?.handoff_events) ? run.handoff_events : [];
  const roleInstructions = normalizeRoleInstructionMap(run?.role_instructions);
  const rolePromptMode = typeof run?.role_prompt_mode === "string" ? String(run.role_prompt_mode) : "";
  const roleGraphEdges = normalizeRoleGraphEdges(run?.role_graph);
  const roleGraphRoles = React.useMemo(() => {
    const set = new Set<string>();
    for (const edge of roleGraphEdges) {
      const from = String(edge.from_role || "").trim().toLowerCase();
      const to = String(edge.to_role || "").trim().toLowerCase();
      if (from) set.add(from);
      if (to) set.add(to);
    }
    for (const role of Object.keys(roleInstructions)) {
      const key = String(role || "").trim().toLowerCase();
      if (key) set.add(key);
    }
    if (Array.isArray(run?.members)) {
      for (const member of run.members) {
        const role = String(member?.role || "").trim().toLowerCase();
        if (role) set.add(role);
      }
    }
    if (Array.isArray(run?.runtime_members)) {
      for (const member of run.runtime_members) {
        const role = String(member?.role || "").trim().toLowerCase();
        if (role) set.add(role);
      }
    }
    return Array.from(set).filter(Boolean).sort();
  }, [roleGraphEdges, roleInstructions, run?.members, run?.runtime_members]);
  const sharedMemoryScope = run?.shared_memory_scope_id ? String(run.shared_memory_scope_id) : "";
  const sharedMemoryMode = run?.shared_memory_mode ? String(run.shared_memory_mode) : "";
  const autoAllocateRoles = run?.auto_allocate_roles === true;
  const autoAllocateAllocated = Array.isArray(run?.auto_allocate_allocated_roles)
    ? run.auto_allocate_allocated_roles
    : [];
  const autoAllocateMissing = Array.isArray(run?.auto_allocate_missing_roles) ? run.auto_allocate_missing_roles : [];
  const autoAllocateWarning = run?.auto_allocate_warning ? String(run.auto_allocate_warning) : "";
  const roleInstructionCount = Object.keys(roleInstructions).length;

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
  const [handoffKind, setHandoffKind] = React.useState<string>("role");
  const [handoffReason, setHandoffReason] = React.useState<string>("");
  const [handoffMessage, setHandoffMessage] = React.useState<string>("");
  const [handoffData, setHandoffData] = React.useState<string>("");
  const [handoffSourceDeployment, setHandoffSourceDeployment] = React.useState<string>("");
  const [handoffSourceSession, setHandoffSourceSession] = React.useState<string>("");
  const [handoffTargetDeployment, setHandoffTargetDeployment] = React.useState<string>("");
  const [handoffTargetSession, setHandoffTargetSession] = React.useState<string>("");
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

  const handoffLatestById = React.useMemo(() => {
    const latest = new Map<string, TeamRunHandoffEventRecord>();
    for (const raw of handoffEvents) {
      const ev = normalizeHandoffEventRecord(raw);
      const hid = String(ev.handoff_id || "").trim();
      if (!hid) continue;
      latest.set(hid, ev);
    }
    return latest;
  }, [handoffEvents]);

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
    if (eventType !== "progress" && eventType !== "drift" && eventType !== "spawn_validation") {
      setGoalUpdateError("goal event type must be progress, drift, or spawn_validation");
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

  const emitHandoffEvent = async (
    mode: "manual" | "transition",
    nextState: "proposed" | "accepted" | "declined" | "cancelled" = "proposed",
    seed?: TeamRunHandoffEventRecord,
  ) => {
    setHandoffError(null);
    setHandoffNote(null);
    if (!canWrite) {
      setHandoffError("missing team or run id");
      return;
    }
    const fromRole = (mode === "manual" ? handoffFromRole : seed?.from_role || "").trim();
    const toRole = (mode === "manual" ? handoffToRole : seed?.to_role || "").trim();
    const kind = (mode === "manual" ? handoffKind : seed?.kind || "role").trim().toLowerCase();
    const handoffId = (mode === "manual" ? "" : seed?.handoff_id || "").trim();
    if (!handoffId && (!fromRole || !toRole)) {
      setHandoffError("handoff requires from_role and to_role");
      return;
    }
    if (kind !== "role" && kind !== "cross_deployment") {
      setHandoffError("handoff kind must be role or cross_deployment");
      return;
    }
    let dataObj: Record<string, any> | undefined;
    const rawData = mode === "manual" ? handoffData.trim() : "";
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
    const event: Record<string, any> = {
      kind,
      state: nextState,
    };
    if (handoffId) event.handoff_id = handoffId;
    if (fromRole) event.from_role = fromRole;
    if (toRole) event.to_role = toRole;
    const reason = (mode === "manual" ? handoffReason : seed?.reason || "").trim();
    const message = (mode === "manual" ? handoffMessage : "").trim();
    if (reason) event.reason = reason;
    if (message) event.message = message;
    else if (mode === "transition") event.message = nextState;
    const sourceDeploymentId = (
      mode === "manual" ? handoffSourceDeployment : seed?.source_deployment_id || ""
    ).trim();
    const sourceSessionId = (mode === "manual" ? handoffSourceSession : seed?.source_session_id || "").trim();
    const targetDeploymentId = (
      mode === "manual" ? handoffTargetDeployment : seed?.target_deployment_id || ""
    ).trim();
    const targetSessionId = (mode === "manual" ? handoffTargetSession : seed?.target_session_id || "").trim();
    if (kind === "cross_deployment") {
      if (!sourceDeploymentId || !sourceSessionId || !targetDeploymentId || !targetSessionId) {
        setHandoffError("cross-deployment handoff requires source/target deployment and session ids");
        return;
      }
      event.source_deployment_id = sourceDeploymentId;
      event.source_session_id = sourceSessionId;
      event.target_deployment_id = targetDeploymentId;
      event.target_session_id = targetSessionId;
    }
    if (dataObj) event.data = dataObj;
    setHandoffBusy(true);
    try {
      const resp = await apiBrokerTeamRunHandoff(props.base, teamId, runId, { event }, props.auth);
      if (!resp.ok) {
        setHandoffError(resp.error || resp.err || "handoff event failed");
        return;
      }
      setHandoffNote(nextState === "proposed" ? "handoff event emitted" : `handoff ${nextState}`);
      await props.onRefreshRun(runId);
    } catch (err) {
      setHandoffError(String(err));
    } finally {
      setHandoffBusy(false);
    }
  };

  const handleHandoffEvent = async () => emitHandoffEvent("manual", "proposed");

  const handleHandoffTransition = async (
    seed: TeamRunHandoffEventRecord,
    nextState: "accepted" | "declined" | "cancelled",
  ) => emitHandoffEvent("transition", nextState, seed);

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
      {roleGraphEdges.length > 0 || roleGraphRoles.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-xs font-semibold text-white/80">Role graph</div>
          <RoleGraphPreview
            roles={roleGraphRoles}
            edges={roleGraphEdges}
            title="Role graph"
            emptyLabel="No role graph recorded."
          />
          {roleGraphEdges.length > 0 ? (
            <div className="mt-2 grid gap-1 text-[11px] text-white/60">
              {roleGraphEdges.map((edge, idx) => (
                <div key={`role-graph-edge-${edge.from_role}-${edge.to_role}-${idx}`}>
                  {edge.from_role || "role"} {"->"} {edge.to_role || "role"}
                  {edge.reason ? ` · ${edge.reason}` : ""}
                </div>
              ))}
            </div>
          ) : null}
          {rolePromptMode || roleInstructionCount > 0 ? (
            <div className="mt-1 text-[11px] text-white/60">
              {rolePromptMode ? `prompt mode ${rolePromptMode}` : ""}
              {roleInstructionCount > 0 ? `${rolePromptMode ? " · " : ""}instructions ${roleInstructionCount}` : ""}
            </div>
          ) : null}
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
        <div className="text-xs font-semibold text-white/80">Goal events (progress / drift / spawn_validation)</div>
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
              <option value="spawn_validation">spawn_validation</option>
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
        <div className="text-xs font-semibold text-white/80">Handoff events</div>
        {handoffEventRows.length > 0 ? (
          <div className="grid gap-1 text-[11px] text-white/60">
            {handoffEventRows.map((ev: any, idx: number) => {
              const record = normalizeHandoffEventRecord(ev);
              const fromRole = record.from_role || "";
              const toRole = record.to_role || "";
              const ts = typeof record.ts_unix_ms === "number" ? props.fmtTs(record.ts_unix_ms) : "";
              const reason = record.reason || "";
              const msg = record.message || "";
              const kind = record.kind || "role";
              const state = record.state || "proposed";
              const handoffId = record.handoff_id || "";
              const latest = handoffId ? handoffLatestById.get(handoffId) : undefined;
              const isLatest = !handoffId || latest?.event_index === record.event_index;
              const canResolve = kind === "cross_deployment" && state === "proposed" && isLatest;
              const sourceRef =
                record.source_deployment_id || record.source_session_id
                  ? `${record.source_deployment_id || "deployment"} / ${record.source_session_id || "session"}`
                  : "";
              const targetRef =
                record.target_deployment_id || record.target_session_id
                  ? `${record.target_deployment_id || "deployment"} / ${record.target_session_id || "session"}`
                  : "";
              return (
                <div
                  key={`handoff-${record.event_index || idx}`}
                  data-testid={
                    handoffId ? `team-run-handoff-row-${handoffId}-${record.event_index || idx}` : undefined
                  }
                  className="rounded-md border border-white/5 bg-black/20 px-2 py-1"
                >
                  <div className="text-[11px] text-white/70">
                    {fromRole || "role"} {"->"} {toRole || "role"}
                    {ts ? ` · ${ts}` : ""}
                    {record.event_index ? ` · #${record.event_index}` : ""}
                  </div>
                  <div className="text-[10px] uppercase tracking-[0.18em] text-white/45">
                    {kind === "cross_deployment" ? "cross-deployment" : "role"} · {state}
                    {handoffId ? ` · ${handoffId}` : ""}
                  </div>
                  {kind === "cross_deployment" && sourceRef && targetRef ? (
                    <div className="text-[11px] text-sky-100/75">
                      {sourceRef} {"->"} {targetRef}
                    </div>
                  ) : null}
                  {reason ? <div className="text-[11px] text-white/60">reason: {reason}</div> : null}
                  {msg ? <div className="text-[11px] text-white/60">{msg}</div> : null}
                  {record.data ? (
                    <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/50">
                      {JSON.stringify(record.data, null, 2)}
                    </pre>
                  ) : null}
                  {canResolve ? (
                    <div className="mt-2 flex flex-wrap gap-2">
                      <button
                        className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-2 py-1 text-[10px] text-emerald-100 hover:bg-emerald-500/20 disabled:opacity-50"
                        type="button"
                        disabled={!canWrite || handoffBusy}
                        onClick={() => void handleHandoffTransition(record, "accepted")}
                      >
                        Accept
                      </button>
                      <button
                        className="rounded-md border border-rose-400/30 bg-rose-500/10 px-2 py-1 text-[10px] text-rose-100 hover:bg-rose-500/20 disabled:opacity-50"
                        type="button"
                        disabled={!canWrite || handoffBusy}
                        onClick={() => void handleHandoffTransition(record, "declined")}
                      >
                        Decline
                      </button>
                    </div>
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
            <FieldLabel>Kind</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffKind}
              onChange={(e) => setHandoffKind(e.target.value)}
              data-testid="team-run-handoff-kind"
              disabled={!canWrite || handoffBusy}
            >
              <option value="role">role</option>
              <option value="cross_deployment">cross_deployment</option>
            </select>
            <FieldLabel>From</FieldLabel>
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffFromRole}
              onChange={(e) => setHandoffFromRole(e.target.value)}
              data-testid="team-run-handoff-from-role"
              placeholder="planner"
              disabled={!canWrite || handoffBusy}
            />
            <FieldLabel>To</FieldLabel>
            <input
              className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffToRole}
              onChange={(e) => setHandoffToRole(e.target.value)}
              data-testid="team-run-handoff-to-role"
              placeholder="executor"
              disabled={!canWrite || handoffBusy}
            />
            <FieldLabel>Reason</FieldLabel>
            <input
              className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={handoffReason}
              onChange={(e) => setHandoffReason(e.target.value)}
              data-testid="team-run-handoff-reason"
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
              data-testid="team-run-handoff-message"
              placeholder="handoff note"
              disabled={!canWrite || handoffBusy}
            />
          </div>
          {handoffKind === "cross_deployment" ? (
            <div className="grid gap-2 rounded-md border border-sky-400/20 bg-sky-500/5 p-2">
              <div className="text-[11px] text-sky-100/80">
                Cross-deployment handoff preserves source/target deployment and session identity for replayable accept/decline flow.
              </div>
              <div className="flex flex-wrap items-center gap-2">
                <FieldLabel>Source deployment</FieldLabel>
                <input
                  className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                  value={handoffSourceDeployment}
                  onChange={(e) => setHandoffSourceDeployment(e.target.value)}
                  data-testid="team-run-handoff-source-deployment"
                  placeholder="dep-a"
                  disabled={!canWrite || handoffBusy}
                />
                <FieldLabel>Source session</FieldLabel>
                <input
                  className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                  value={handoffSourceSession}
                  onChange={(e) => setHandoffSourceSession(e.target.value)}
                  data-testid="team-run-handoff-source-session"
                  placeholder="sess-a"
                  disabled={!canWrite || handoffBusy}
                />
              </div>
              <div className="flex flex-wrap items-center gap-2">
                <FieldLabel>Target deployment</FieldLabel>
                <input
                  className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                  value={handoffTargetDeployment}
                  onChange={(e) => setHandoffTargetDeployment(e.target.value)}
                  data-testid="team-run-handoff-target-deployment"
                  placeholder="dep-b"
                  disabled={!canWrite || handoffBusy}
                />
                <FieldLabel>Target session</FieldLabel>
                <input
                  className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                  value={handoffTargetSession}
                  onChange={(e) => setHandoffTargetSession(e.target.value)}
                  data-testid="team-run-handoff-target-session"
                  placeholder="sess-b"
                  disabled={!canWrite || handoffBusy}
                />
              </div>
            </div>
          ) : null}
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
            data-testid="team-run-handoff-submit"
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
