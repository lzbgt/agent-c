import React from "react";
import {
  apiBrokerTeamCreate,
  apiBrokerTeamDelete,
  apiBrokerTeamGet,
  apiBrokerTeamList,
  apiBrokerTeamMembersDelete,
  apiBrokerTeamMembersList,
  apiBrokerTeamMembersUpsert,
  apiBrokerTeamQuorumDelete,
  apiBrokerTeamQuorumList,
  apiBrokerTeamQuorumUpsert,
  apiBrokerTeamRunCreate,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunApprovalsCreate,
  apiBrokerTeamRunApprovalsList,
  type ApiAuth,
} from "../../api";
import FieldLabel from "../FieldLabel";

const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

export type BrokerTeamConsoleProps = {
  base: string;
  auth: ApiAuth;
  authKey: string;
};

export default function BrokerTeamConsole(props: BrokerTeamConsoleProps) {
  const authToken = props.auth?.token ? String(props.auth.token).trim() : "";
  const canQuery = props.base.length > 0 && authToken.length > 0;

  const [teamsBusy, setTeamsBusy] = React.useState<boolean>(false);
  const [teamsError, setTeamsError] = React.useState<string | null>(null);
  const [teams, setTeams] = React.useState<any[] | null>(null);
  const [teamId, setTeamId] = React.useState<string>("");
  const [newTeamId, setNewTeamId] = React.useState<string>("");
  const [newTeamName, setNewTeamName] = React.useState<string>("");
  const [teamDetails, setTeamDetails] = React.useState<any | null>(null);

  const [membersBusy, setMembersBusy] = React.useState<boolean>(false);
  const [membersError, setMembersError] = React.useState<string | null>(null);
  const [members, setMembers] = React.useState<any[] | null>(null);
  const [memberId, setMemberId] = React.useState<string>("");
  const [memberRole, setMemberRole] = React.useState<string>("executor");
  const [memberStatus, setMemberStatus] = React.useState<string>("active");
  const [memberWeight, setMemberWeight] = React.useState<string>("1");
  const [memberAgentId, setMemberAgentId] = React.useState<string>("");
  const [memberDeploymentId, setMemberDeploymentId] = React.useState<string>("");

  const [rulesBusy, setRulesBusy] = React.useState<boolean>(false);
  const [rulesError, setRulesError] = React.useState<string | null>(null);
  const [rules, setRules] = React.useState<any[] | null>(null);
  const [ruleAction, setRuleAction] = React.useState<string>("team_run");
  const [ruleMinApprovals, setRuleMinApprovals] = React.useState<string>("1");
  const [ruleMode, setRuleMode] = React.useState<string>("strict");

  const [runPrompt, setRunPrompt] = React.useState<string>("");
  const [runModel, setRunModel] = React.useState<string>("");
  const [runTools, setRunTools] = React.useState<string>("basic");
  const [runRole, setRunRole] = React.useState<string>("");
  const [runRoles, setRunRoles] = React.useState<string>("");
  const [runConcurrency, setRunConcurrency] = React.useState<string>("1");
  const [runTimeoutMs, setRunTimeoutMs] = React.useState<string>("60000");
  const [runQuorumMode, setRunQuorumMode] = React.useState<string>("auto");
  const [runBusy, setRunBusy] = React.useState<boolean>(false);
  const [runError, setRunError] = React.useState<string | null>(null);
  const [runResult, setRunResult] = React.useState<any | null>(null);
  const [runLookupId, setRunLookupId] = React.useState<string>("");
  const [runLookupBusy, setRunLookupBusy] = React.useState<boolean>(false);
  const [runLookupError, setRunLookupError] = React.useState<string | null>(null);
  const [runLookupResult, setRunLookupResult] = React.useState<any | null>(null);
  const [approvalsBusy, setApprovalsBusy] = React.useState<boolean>(false);
  const [approvalsError, setApprovalsError] = React.useState<string | null>(null);
  const [approvals, setApprovals] = React.useState<any[] | null>(null);
  const [approvalRunId, setApprovalRunId] = React.useState<string>("");
  const [approvalMemberId, setApprovalMemberId] = React.useState<string>("");
  const [approvalRuleId, setApprovalRuleId] = React.useState<string>("");
  const [approvalDecision, setApprovalDecision] = React.useState<string>("approve");
  const [approvalReason, setApprovalReason] = React.useState<string>("");

  const teamList = Array.isArray(teams) ? teams : [];
  const teamIdTrimmed = String(teamId || "").trim();
  const approvalRunIdTrimmed = String(approvalRunId || runLookupId || "").trim();
  const membersList = Array.isArray(members) ? members : [];
  const rulesList = Array.isArray(rules) ? rules : [];

  React.useEffect(() => {
    if (!approvalRunId && runResult?.team_run_id) {
      setApprovalRunId(String(runResult.team_run_id));
    }
  }, [approvalRunId, runResult]);

  React.useEffect(() => {
    if (!approvalRunId && runLookupResult?.team_run_id) {
      setApprovalRunId(String(runLookupResult.team_run_id));
    }
  }, [approvalRunId, runLookupResult]);

  React.useEffect(() => {
    setApprovals(null);
    setApprovalsError(null);
  }, [approvalRunIdTrimmed, teamIdTrimmed]);

  const refreshTeams = async () => {
    if (!canQuery) return;
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      const resp = await apiBrokerTeamList(props.base, props.auth);
      const rows = Array.isArray(resp?.teams) ? resp.teams : [];
      setTeams(rows);
      if (!teamIdTrimmed && rows.length > 0) {
        setTeamId(String(rows[0]?.team_id || ""));
      }
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  };

  const refreshTeamDetails = async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    try {
      const resp = await apiBrokerTeamGet(props.base, tid, props.auth);
      setTeamDetails(resp?.team ?? null);
    } catch {
      setTeamDetails(null);
    }
  };

  const refreshMembers = async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      const resp = await apiBrokerTeamMembersList(props.base, tid, props.auth);
      setMembers(Array.isArray(resp?.members) ? resp.members : []);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const refreshRules = async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    setRulesError(null);
    setRulesBusy(true);
    try {
      const resp = await apiBrokerTeamQuorumList(props.base, tid, props.auth);
      setRules(Array.isArray(resp?.rules) ? resp.rules : []);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  };

  const handleCreateTeam = async () => {
    const tid = String(newTeamId || "").trim();
    if (!tid) {
      setTeamsError("team_id required");
      return;
    }
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      await apiBrokerTeamCreate(props.base, { team_id: tid, display_name: String(newTeamName || "").trim() }, props.auth);
      setNewTeamId("");
      setNewTeamName("");
      await refreshTeams();
      setTeamId(tid);
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  };

  const handleDeleteTeam = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    if (!window.confirm(`Delete team "${tid}"?`)) return;
    setTeamsError(null);
    setTeamsBusy(true);
    try {
      await apiBrokerTeamDelete(props.base, tid, props.auth);
      setTeamId("");
      setTeamDetails(null);
      await refreshTeams();
    } catch (err) {
      setTeamsError(String(err));
    } finally {
      setTeamsBusy(false);
    }
  };

  const handleAddMember = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const role = String(memberRole || "").trim();
    if (!role) {
      setMembersError("role required");
      return;
    }
    setMembersError(null);
    setMembersBusy(true);
    try {
      const payload: Record<string, any> = { role };
      const mid = String(memberId || "").trim();
      if (mid) payload.member_id = mid;
      const aid = String(memberAgentId || "").trim();
      if (aid) payload.agent_id = aid;
      const dep = String(memberDeploymentId || "").trim();
      if (dep) payload.deployment_id = dep;
      const status = String(memberStatus || "").trim();
      if (status) payload.status = status;
      const w = Number.parseInt(String(memberWeight || ""), 10);
      if (Number.isFinite(w)) payload.weight = w;
      await apiBrokerTeamMembersUpsert(props.base, tid, payload, props.auth);
      setMemberId("");
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleDeleteMember = async (memberIdRaw: string) => {
    const tid = teamIdTrimmed;
    const mid = String(memberIdRaw || "").trim();
    if (!tid || !mid) return;
    if (!window.confirm(`Remove member "${mid}"?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      await apiBrokerTeamMembersDelete(props.base, tid, mid, props.auth);
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  };

  const handleAddRule = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const action = String(ruleAction || "").trim();
    const min = Number.parseInt(String(ruleMinApprovals || ""), 10);
    const mode = String(ruleMode || "").trim();
    if (!action) {
      setRulesError("action required");
      return;
    }
    if (!Number.isFinite(min) || min <= 0) {
      setRulesError("min approvals must be > 0");
      return;
    }
    if (!mode) {
      setRulesError("quorum mode required");
      return;
    }
    setRulesError(null);
    setRulesBusy(true);
    try {
      await apiBrokerTeamQuorumUpsert(props.base, tid, { action, min_approvals: min, quorum_mode: mode }, props.auth);
      await refreshRules(tid);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  };

  const handleDeleteRule = async (ruleIdRaw: string) => {
    const tid = teamIdTrimmed;
    const rid = String(ruleIdRaw || "").trim();
    if (!tid || !rid) return;
    if (!window.confirm(`Delete quorum rule "${rid}"?`)) return;
    setRulesError(null);
    setRulesBusy(true);
    try {
      await apiBrokerTeamQuorumDelete(props.base, tid, rid, props.auth);
      await refreshRules(tid);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  };

  const handleCreateRun = async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const prompt = String(runPrompt || "").trim();
    if (!prompt) {
      setRunError("prompt required");
      return;
    }
    setRunError(null);
    setRunBusy(true);
    try {
      const runPayload: Record<string, any> = { prompt };
      const model = String(runModel || "").trim();
      if (model) runPayload.model = model;
      const tools = String(runTools || "").trim();
      if (tools) runPayload.tools = tools;
      const teamPayload: Record<string, any> = {};
      const role = String(runRole || "").trim();
      if (role) teamPayload.role = role;
      const rolesCsv = String(runRoles || "").trim();
      if (rolesCsv) teamPayload.roles = rolesCsv.split(",").map((r) => r.trim()).filter(Boolean);
      const conc = Number.parseInt(String(runConcurrency || ""), 10);
      if (Number.isFinite(conc)) teamPayload.max_concurrency = conc;
      const timeout = Number.parseInt(String(runTimeoutMs || ""), 10);
      if (Number.isFinite(timeout)) teamPayload.timeout_ms = timeout;
      const qmode = String(runQuorumMode || "").trim();
      if (qmode) teamPayload.quorum_policy = { mode: qmode };
      const resp = await apiBrokerTeamRunCreate(props.base, tid, { run: runPayload, team: teamPayload }, props.auth);
      setRunResult(resp);
      if (resp?.team_run_id) setRunLookupId(String(resp.team_run_id));
    } catch (err) {
      setRunError(String(err));
    } finally {
      setRunBusy(false);
    }
  };

  const handleRunLookup = async () => {
    const tid = teamIdTrimmed;
    const runId = String(runLookupId || "").trim();
    if (!tid || !runId) {
      setRunLookupError("missing team_id or run id");
      return;
    }
    setRunLookupError(null);
    setRunLookupBusy(true);
    try {
      const resp = await apiBrokerTeamRunGet(props.base, tid, runId, props.auth);
      setRunLookupResult(resp);
    } catch (err) {
      setRunLookupError(String(err));
    } finally {
      setRunLookupBusy(false);
    }
  };

  const handleApprovalsRefresh = async () => {
    const tid = teamIdTrimmed;
    const runId = approvalRunIdTrimmed;
    if (!tid || !runId) {
      setApprovalsError("missing team_id or run id");
      return;
    }
    setApprovalsError(null);
    setApprovalsBusy(true);
    try {
      const resp = await apiBrokerTeamRunApprovalsList(props.base, tid, runId, props.auth);
      if (resp.status >= 400) {
        throw new Error(resp?.data?.error || resp?.data?.err || `HTTP ${resp.status}`);
      }
      const rows = Array.isArray(resp?.data?.approvals) ? resp.data.approvals : [];
      setApprovals(rows);
    } catch (err) {
      setApprovalsError(String(err));
    } finally {
      setApprovalsBusy(false);
    }
  };

  const handleApprovalSubmit = async () => {
    const tid = teamIdTrimmed;
    const runId = approvalRunIdTrimmed;
    const memberId = String(approvalMemberId || "").trim();
    const decision = String(approvalDecision || "").trim().toLowerCase();
    const ruleId = String(approvalRuleId || "").trim();
    const reason = String(approvalReason || "").trim();
    if (!tid || !runId) {
      setApprovalsError("missing team_id or run id");
      return;
    }
    if (!memberId) {
      setApprovalsError("member_id required");
      return;
    }
    if (!decision) {
      setApprovalsError("decision required");
      return;
    }
    setApprovalsError(null);
    setApprovalsBusy(true);
    try {
      const payload: Record<string, any> = { member_id: memberId, decision };
      if (ruleId) payload.rule_id = ruleId;
      if (reason) payload.reason = reason;
      const resp = await apiBrokerTeamRunApprovalsCreate(props.base, tid, runId, payload, props.auth);
      if (resp.status >= 400) {
        throw new Error(resp?.data?.error || resp?.data?.err || `HTTP ${resp.status}`);
      }
      const rows = Array.isArray(resp?.data?.approvals) ? resp.data.approvals : [];
      setApprovals(rows);
      setApprovalReason("");
    } catch (err) {
      setApprovalsError(String(err));
    } finally {
      setApprovalsBusy(false);
    }
  };

  React.useEffect(() => {
    if (!canQuery || teamList.length === 0) return;
    if (!teamIdTrimmed) {
      setTeamId(String(teamList[0]?.team_id || ""));
      return;
    }
    void refreshTeamDetails(teamIdTrimmed);
  }, [canQuery, teamIdTrimmed, teamList.length]);

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Teams</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || teamsBusy}
            onClick={() => void refreshTeams()}
          >
            {teamsBusy ? "Loading…" : "Refresh"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || teamsBusy || !teamIdTrimmed}
            onClick={() => void handleDeleteTeam()}
          >
            Delete
          </button>
        </div>
      </div>
      <div className="mb-2 text-[11px] text-white/50">Manage team members, quorum rules, and team runs.</div>

      <div className="grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Team</FieldLabel>
          <select
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={teamIdTrimmed}
            onChange={(e) => setTeamId(e.target.value)}
            disabled={teamList.length === 0}
          >
            {teamList.length === 0 ? <option value="">(no teams)</option> : null}
            {teamList.map((t) => (
              <option key={String(t?.team_id)} value={String(t?.team_id)}>
                {String(t?.display_name || t?.team_id || "")}
              </option>
            ))}
          </select>
        </div>
        {teamDetails ? (
          <div className="text-[11px] text-white/50">
            owner {String(teamDetails?.owner_sub || "unknown")}
            {teamDetails?.created_unix_ms ? ` · ${fmtTs(teamDetails.created_unix_ms)}` : ""}
          </div>
        ) : null}
      </div>

      <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Create team</div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>ID</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={newTeamId}
            onChange={(e) => setNewTeamId(e.target.value)}
            placeholder="team-ops"
          />
          <FieldLabel>Name</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={newTeamName}
            onChange={(e) => setNewTeamName(e.target.value)}
            placeholder="Ops team"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || teamsBusy}
            onClick={() => void handleCreateTeam()}
          >
            Create
          </button>
        </div>
      </div>

      <div className="mt-4 grid gap-3">
        <div className="flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Team members</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || membersBusy}
            onClick={() => void refreshMembers(teamIdTrimmed)}
          >
            {membersBusy ? "Loading…" : "Refresh"}
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberId}
            onChange={(e) => setMemberId(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Role</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberRole}
            onChange={(e) => setMemberRole(e.target.value)}
          />
          <FieldLabel>Status</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberStatus}
            onChange={(e) => setMemberStatus(e.target.value)}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Agent ID</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberAgentId}
            onChange={(e) => setMemberAgentId(e.target.value)}
            placeholder="agent1"
          />
          <FieldLabel>Deployment</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberDeploymentId}
            onChange={(e) => setMemberDeploymentId(e.target.value)}
            placeholder="default"
          />
          <FieldLabel>Weight</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberWeight}
            onChange={(e) => setMemberWeight(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || membersBusy}
            onClick={() => void handleAddMember()}
          >
            Add member
          </button>
        </div>
        {membersError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {membersError}
          </div>
        ) : null}
        {members && members.length > 0 ? (
          <div className="grid gap-2">
            {members.map((m, idx) => {
              const mid = String(m?.member_id || "");
              return (
                <div
                  key={`member-${mid}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-[11px] text-white/70">
                    <span className="text-white/90">{mid || "member"}</span>
                    {m?.role ? ` · role ${m.role}` : ""}
                    {m?.status ? ` · ${m.status}` : ""}
                    {m?.agent_id ? ` · agent ${m.agent_id}` : ""}
                    {m?.deployment_id ? ` · dep ${m.deployment_id}` : ""}
                    {m?.created_unix_ms ? ` · ${fmtTs(m.created_unix_ms)}` : ""}
                  </div>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => void handleDeleteMember(mid)}
                  >
                    Remove
                  </button>
                </div>
              );
            })}
          </div>
        ) : null}
      </div>

      <div className="mt-4 grid gap-3">
        <div className="flex items-center justify-between gap-2">
          <div className="text-xs font-semibold text-white/80">Quorum rules</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || rulesBusy}
            onClick={() => void refreshRules(teamIdTrimmed)}
          >
            {rulesBusy ? "Loading…" : "Refresh"}
          </button>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Action</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={ruleAction}
            onChange={(e) => setRuleAction(e.target.value)}
          />
          <FieldLabel>Min approvals</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={ruleMinApprovals}
            onChange={(e) => setRuleMinApprovals(e.target.value)}
          />
          <FieldLabel>Mode</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={ruleMode}
            onChange={(e) => setRuleMode(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || rulesBusy}
            onClick={() => void handleAddRule()}
          >
            Add rule
          </button>
        </div>
        {rulesError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {rulesError}
          </div>
        ) : null}
        {rules && rules.length > 0 ? (
          <div className="grid gap-2">
            {rules.map((r, idx) => {
              const rid = String(r?.rule_id || "");
              return (
                <div
                  key={`rule-${rid}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-[11px] text-white/70">
                    <span className="text-white/90">{r?.action || "team_run"}</span>
                    {r?.min_approvals ? ` · min ${r.min_approvals}` : ""}
                    {r?.quorum_mode ? ` · ${r.quorum_mode}` : ""}
                    {r?.created_unix_ms ? ` · ${fmtTs(r.created_unix_ms)}` : ""}
                  </div>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => void handleDeleteRule(rid)}
                  >
                    Remove
                  </button>
                </div>
              );
            })}
          </div>
        ) : null}
      </div>

      <div className="mt-4 grid gap-3">
        <div className="text-xs font-semibold text-white/80">Team run</div>
        <div className="text-[11px] text-white/50">Creates a run across team members; prompt is required.</div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Prompt</FieldLabel>
          <input
            className="min-w-[240px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runPrompt}
            onChange={(e) => setRunPrompt(e.target.value)}
            placeholder="Summarize today’s alerts"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Model</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runModel}
            onChange={(e) => setRunModel(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Tools</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runTools}
            onChange={(e) => setRunTools(e.target.value)}
          >
            <option value="none">none</option>
            <option value="basic">basic</option>
            <option value="host">host</option>
          </select>
          <FieldLabel>Role</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runRole}
            onChange={(e) => setRunRole(e.target.value)}
            placeholder="planner"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Roles</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runRoles}
            onChange={(e) => setRunRoles(e.target.value)}
            placeholder="planner,executor"
          />
          <FieldLabel>Max concurrency</FieldLabel>
          <input
            className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runConcurrency}
            onChange={(e) => setRunConcurrency(e.target.value)}
          />
          <FieldLabel>Timeout ms</FieldLabel>
          <input
            className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runTimeoutMs}
            onChange={(e) => setRunTimeoutMs(e.target.value)}
          />
          <FieldLabel>Quorum</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runQuorumMode}
            onChange={(e) => setRunQuorumMode(e.target.value)}
          >
            <option value="auto">auto</option>
            <option value="off">off</option>
          </select>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || runBusy}
            onClick={() => void handleCreateRun()}
          >
            {runBusy ? "Submitting…" : "Create run"}
          </button>
        </div>
        {runError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {runError}
          </div>
        ) : null}
        {runResult ? (
          <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
            run id {String(runResult?.team_run_id || "unknown")} · status {String(runResult?.status || "")}
          </div>
        ) : null}

        <div className="mt-2 flex flex-wrap items-center gap-2">
          <FieldLabel>Run ID</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runLookupId}
            onChange={(e) => setRunLookupId(e.target.value)}
            placeholder="team_run_id"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || !teamIdTrimmed || runLookupBusy}
            onClick={() => void handleRunLookup()}
          >
            {runLookupBusy ? "Loading…" : "Get status"}
          </button>
        </div>
        {runLookupError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {runLookupError}
          </div>
        ) : null}
        {runLookupResult ? (
          <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
            status {String(runLookupResult?.status || "")}
            {runLookupResult?.created_unix_ms ? ` · ${fmtTs(runLookupResult.created_unix_ms)}` : ""}
          </div>
        ) : null}

        <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
          <div className="text-xs font-semibold text-white/80">Run approvals</div>
          <div className="text-[11px] text-white/50">
            Submit or review approvals for a team run (quorum rules apply).
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Run ID</FieldLabel>
            <input
              className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={approvalRunId}
              onChange={(e) => setApprovalRunId(e.target.value)}
              placeholder="defaults to run id above"
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !teamIdTrimmed || approvalsBusy}
              onClick={() => void handleApprovalsRefresh()}
            >
              {approvalsBusy ? "Loading…" : "Load approvals"}
            </button>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Member ID</FieldLabel>
            <input
              className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={approvalMemberId}
              onChange={(e) => setApprovalMemberId(e.target.value)}
              placeholder="member id"
              list="team-approvals-members"
            />
            <datalist id="team-approvals-members">
              {membersList.map((m, idx) => {
                const mid = String(m?.member_id || "");
                if (!mid) return null;
                return <option key={`member-opt-${mid}-${idx}`} value={mid} />;
              })}
            </datalist>
            <FieldLabel>Decision</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={approvalDecision}
              onChange={(e) => setApprovalDecision(e.target.value)}
            >
              <option value="approve">approve</option>
              <option value="deny">deny</option>
            </select>
            <FieldLabel>Rule ID</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={approvalRuleId}
              onChange={(e) => setApprovalRuleId(e.target.value)}
              placeholder="optional"
              list="team-approvals-rules"
            />
            <datalist id="team-approvals-rules">
              {rulesList.map((r, idx) => {
                const rid = String(r?.rule_id || "");
                if (!rid) return null;
                return <option key={`rule-opt-${rid}-${idx}`} value={rid} />;
              })}
            </datalist>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Reason</FieldLabel>
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={approvalReason}
              onChange={(e) => setApprovalReason(e.target.value)}
              placeholder="optional"
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !teamIdTrimmed || approvalsBusy}
              onClick={() => void handleApprovalSubmit()}
            >
              {approvalsBusy ? "Submitting…" : "Submit approval"}
            </button>
          </div>
          {approvalsError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {approvalsError}
            </div>
          ) : null}
          {approvals && approvals.length > 0 ? (
            <div className="grid gap-2">
              {approvals.map((a, idx) => (
                <div
                  key={`approval-${a?.approval_id || idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-[11px] text-white/70">
                    <span className="text-white/90">{String(a?.member_id || "member")}</span>
                    {a?.decision ? ` · ${a.decision}` : ""}
                    {a?.rule_id ? ` · rule ${a.rule_id}` : ""}
                    {a?.role ? ` · role ${a.role}` : ""}
                    {a?.created_by ? ` · by ${a.created_by}` : ""}
                    {a?.created_unix_ms ? ` · ${fmtTs(a.created_unix_ms)}` : ""}
                    {a?.reason ? ` · ${a.reason}` : ""}
                  </div>
                </div>
              ))}
            </div>
          ) : approvals ? (
            <div className="text-[11px] text-white/50">No approvals yet.</div>
          ) : null}
        </div>
      </div>
    </section>
  );
}
