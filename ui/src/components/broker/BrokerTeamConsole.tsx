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
import type { BrokerEventRow } from "./types";

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
  quorumEvents?: BrokerEventRow[];
};

export default function BrokerTeamConsole(props: BrokerTeamConsoleProps) {
  const authToken = props.auth?.token ? String(props.auth.token).trim() : "";
  const canQuery = props.base.length > 0 && authToken.length > 0;

  const [teamsBusy, setTeamsBusy] = React.useState<boolean>(false);
  const [teamsError, setTeamsError] = React.useState<string | null>(null);
  type TeamRow = {
    team_id?: string;
    display_name?: string;
    owner_sub?: string;
    created_unix_ms?: number;
    tags?: string[];
    policy_ref?: string;
    shared_memory_scope_id?: string;
    meta?: Record<string, any>;
  };

  const [teams, setTeams] = React.useState<TeamRow[] | null>(null);
  const [teamId, setTeamId] = React.useState<string>("");
  const [newTeamId, setNewTeamId] = React.useState<string>("");
  const [newTeamName, setNewTeamName] = React.useState<string>("");
  const [teamDetails, setTeamDetails] = React.useState<any | null>(null);

  const [membersBusy, setMembersBusy] = React.useState<boolean>(false);
  const [membersError, setMembersError] = React.useState<string | null>(null);
  type TeamMemberRow = {
    member_id?: string;
    role?: string;
    status?: string;
    agent_id?: string;
    deployment_id?: string;
    created_unix_ms?: number;
    weight?: number;
    capabilities?: string[];
    meta?: Record<string, any>;
  };
  type TeamQuorumRuleRow = {
    rule_id?: string;
    action?: string;
    min_approvals?: number;
    quorum_mode?: string;
    created_unix_ms?: number;
  };
  type TeamRunApprovalRow = {
    approval_id?: string;
    team_id?: string;
    team_run_id?: string;
    rule_id?: string;
    member_id?: string;
    role?: string;
    decision?: string;
    reason?: string;
    created_by?: string;
    created_unix_ms?: number;
  };

  const [members, setMembers] = React.useState<TeamMemberRow[] | null>(null);
  const [memberId, setMemberId] = React.useState<string>("");
  const [memberRole, setMemberRole] = React.useState<string>("executor");
  const [memberStatus, setMemberStatus] = React.useState<string>("active");
  const [memberWeight, setMemberWeight] = React.useState<string>("1");
  const [memberCapabilities, setMemberCapabilities] = React.useState<string>("");
  const [memberAgentId, setMemberAgentId] = React.useState<string>("");
  const [memberDeploymentId, setMemberDeploymentId] = React.useState<string>("");
  const [memberBackendLabel, setMemberBackendLabel] = React.useState<string>("");
  const [memberModel, setMemberModel] = React.useState<string>("");
  const [memberBaseUrl, setMemberBaseUrl] = React.useState<string>("");
  const [memberSummaryModel, setMemberSummaryModel] = React.useState<string>("");
  const [memberTools, setMemberTools] = React.useState<string>("");
  const [memberTimeoutMs, setMemberTimeoutMs] = React.useState<string>("");

  const [rulesBusy, setRulesBusy] = React.useState<boolean>(false);
  const [rulesError, setRulesError] = React.useState<string | null>(null);
  const [rules, setRules] = React.useState<TeamQuorumRuleRow[] | null>(null);
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
  const [runOverridesMode, setRunOverridesMode] = React.useState<string>("member_meta");
  const [runMemberOverridesJson, setRunMemberOverridesJson] = React.useState<string>("");
  const [runRuntimeMembersJson, setRunRuntimeMembersJson] = React.useState<string>("");
  const [runtimeMemberId, setRuntimeMemberId] = React.useState<string>("");
  const [runtimeMemberAgentId, setRuntimeMemberAgentId] = React.useState<string>("");
  const [runtimeMemberDeploymentId, setRuntimeMemberDeploymentId] = React.useState<string>("");
  const [runtimeMemberRole, setRuntimeMemberRole] = React.useState<string>("executor");
  const [runtimeMemberCapabilities, setRuntimeMemberCapabilities] = React.useState<string>("");
  const [runtimeMemberBackendLabel, setRuntimeMemberBackendLabel] = React.useState<string>("");
  const [runtimeMemberModel, setRuntimeMemberModel] = React.useState<string>("");
  const [runtimeMemberBaseUrl, setRuntimeMemberBaseUrl] = React.useState<string>("");
  const [runtimeMemberSummaryModel, setRuntimeMemberSummaryModel] = React.useState<string>("");
  const [runtimeMemberTools, setRuntimeMemberTools] = React.useState<string>("");
  const [runtimeMemberTimeoutMs, setRuntimeMemberTimeoutMs] = React.useState<string>("");
  type InlineApproval = {
    member_id: string;
    decision: "approve" | "deny";
    rule_id?: string;
    reason?: string;
  };
  type QuorumRuleEval = {
    rule_id?: string;
    action?: string;
    quorum_mode?: string;
    min_approvals?: number;
    approved?: number;
    missing?: number;
    ok?: boolean;
    role_allowlist?: string[];
    require_distinct_roles?: boolean;
    approved_member_ids?: string[];
    approved_roles?: string[];
  };
  type QuorumEval = {
    strict_ok?: boolean;
    rules?: QuorumRuleEval[];
  };

  const [runApprovals, setRunApprovals] = React.useState<InlineApproval[]>([]);
  const [runApprovalMemberId, setRunApprovalMemberId] = React.useState<string>("");
  const [runApprovalRuleId, setRunApprovalRuleId] = React.useState<string>("");
  const [runApprovalDecision, setRunApprovalDecision] = React.useState<"approve" | "deny">("approve");
  const [runApprovalReason, setRunApprovalReason] = React.useState<string>("");
  const [runBusy, setRunBusy] = React.useState<boolean>(false);
  const [runError, setRunError] = React.useState<string | null>(null);
  const [runResult, setRunResult] = React.useState<any | null>(null);
  const [runQuorum, setRunQuorum] = React.useState<QuorumEval | null>(null);
  const [runLookupId, setRunLookupId] = React.useState<string>("");
  const [runLookupBusy, setRunLookupBusy] = React.useState<boolean>(false);
  const [runLookupError, setRunLookupError] = React.useState<string | null>(null);
  const [runLookupResult, setRunLookupResult] = React.useState<any | null>(null);
  const [approvalsBusy, setApprovalsBusy] = React.useState<boolean>(false);
  const [approvalsError, setApprovalsError] = React.useState<string | null>(null);
  const [approvals, setApprovals] = React.useState<TeamRunApprovalRow[] | null>(null);
  const [approvalsLastSyncMs, setApprovalsLastSyncMs] = React.useState<number | null>(null);
  const [approvalRunId, setApprovalRunId] = React.useState<string>("");
  const [approvalMemberId, setApprovalMemberId] = React.useState<string>("");
  const [approvalRuleId, setApprovalRuleId] = React.useState<string>("");
  const [approvalDecision, setApprovalDecision] = React.useState<string>("approve");
  const [approvalReason, setApprovalReason] = React.useState<string>("");
  const lastAutoApprovalRunIdRef = React.useRef<string>("");

  const teamList = Array.isArray(teams) ? teams : [];
  const teamIdTrimmed = String(teamId || "").trim();
  const approvalRunIdTrimmed = String(approvalRunId || runLookupId || "").trim();
  const membersList = Array.isArray(members) ? members : [];
  const rulesList = Array.isArray(rules) ? rules : [];
  const quorumRequestRows = React.useMemo(() => {
    const rows = Array.isArray(props.quorumEvents) ? props.quorumEvents : [];
    const filtered = rows.filter((ev) => ev?.type === "team_quorum_request");
    if (!teamIdTrimmed) return filtered.slice(0, 6);
    return filtered.filter((ev) => String(ev?.payload?.team_id || "") === teamIdTrimmed).slice(0, 6);
  }, [props.quorumEvents, teamIdTrimmed]);

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
    setApprovalsLastSyncMs(null);
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
      const caps = String(memberCapabilities || "")
        .split(",")
        .map((c) => c.trim())
        .filter(Boolean);
      if (caps.length > 0) payload.capabilities = caps;
      const meta: Record<string, any> = {};
      const backendLabel = String(memberBackendLabel || "").trim();
      if (backendLabel) meta.backend_label = backendLabel;
      const runOverrides: Record<string, any> = {};
      const model = String(memberModel || "").trim();
      if (model) runOverrides.model = model;
      const baseUrl = String(memberBaseUrl || "").trim();
      if (baseUrl) runOverrides.base_url = baseUrl;
      const summaryModel = String(memberSummaryModel || "").trim();
      if (summaryModel) runOverrides.summary_model = summaryModel;
      const tools = String(memberTools || "").trim();
      if (tools) runOverrides.tools = tools;
      const timeoutMs = Number.parseInt(String(memberTimeoutMs || "").trim(), 10);
      if (Number.isFinite(timeoutMs)) runOverrides.timeout_ms = timeoutMs;
      if (Object.keys(runOverrides).length > 0) meta.run_overrides = runOverrides;
      if (Object.keys(meta).length > 0) payload.meta = meta;
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

  const handleAddRunApproval = () => {
    const memberId = String(runApprovalMemberId || "").trim();
    const decision = String(runApprovalDecision || "").trim().toLowerCase();
    const ruleId = String(runApprovalRuleId || "").trim();
    const reason = String(runApprovalReason || "").trim();
    if (!memberId) {
      setRunError("approval member_id required");
      return;
    }
    if (decision !== "approve" && decision !== "deny") {
      setRunError("approval decision must be approve or deny");
      return;
    }
    setRunError(null);
    const entry: InlineApproval = {
      member_id: memberId,
      decision: decision === "deny" ? "deny" : "approve",
    };
    if (ruleId) entry.rule_id = ruleId;
    if (reason) entry.reason = reason;
    setRunApprovals((prev) => [...prev, entry]);
    setRunApprovalMemberId("");
    setRunApprovalRuleId("");
    setRunApprovalReason("");
  };

  const handleAddRuntimeMember = () => {
    const agentId = String(runtimeMemberAgentId || "").trim();
    if (!agentId) {
      setRunError("runtime member agent_id required");
      return;
    }
    const role = String(runtimeMemberRole || "").trim();
    if (!role) {
      setRunError("runtime member role required");
      return;
    }
    const entry: Record<string, any> = { agent_id: agentId, role };
    const memberId = String(runtimeMemberId || "").trim();
    if (memberId) entry.member_id = memberId;
    const deploymentId = String(runtimeMemberDeploymentId || "").trim();
    if (deploymentId) entry.deployment_id = deploymentId;
    const caps = String(runtimeMemberCapabilities || "")
      .split(",")
      .map((c) => c.trim())
      .filter(Boolean);
    if (caps.length > 0) entry.capabilities = caps;
    const meta: Record<string, any> = {};
    const backendLabel = String(runtimeMemberBackendLabel || "").trim();
    if (backendLabel) meta.backend_label = backendLabel;
    const runOverrides: Record<string, any> = {};
    const model = String(runtimeMemberModel || "").trim();
    if (model) runOverrides.model = model;
    const baseUrl = String(runtimeMemberBaseUrl || "").trim();
    if (baseUrl) runOverrides.base_url = baseUrl;
    const summaryModel = String(runtimeMemberSummaryModel || "").trim();
    if (summaryModel) runOverrides.summary_model = summaryModel;
    const tools = String(runtimeMemberTools || "").trim();
    if (tools) runOverrides.tools = tools;
    const timeoutMs = Number.parseInt(String(runtimeMemberTimeoutMs || "").trim(), 10);
    if (Number.isFinite(timeoutMs)) runOverrides.timeout_ms = timeoutMs;
    if (Object.keys(runOverrides).length > 0) meta.run_overrides = runOverrides;
    if (Object.keys(meta).length > 0) entry.meta = meta;

    let items: any[] = [];
    const raw = String(runRuntimeMembersJson || "").trim();
    if (raw) {
      try {
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) {
          items = parsed;
        } else {
          setRunError("runtime_members json must be an array");
          return;
        }
      } catch (err) {
        setRunError(`invalid runtime_members json: ${String(err)}`);
        return;
      }
    }
    items.push(entry);
    setRunRuntimeMembersJson(JSON.stringify(items, null, 2));
    setRunError(null);
    setRuntimeMemberId("");
    setRuntimeMemberAgentId("");
    setRuntimeMemberDeploymentId("");
    setRuntimeMemberCapabilities("");
    setRuntimeMemberBackendLabel("");
    setRuntimeMemberModel("");
    setRuntimeMemberBaseUrl("");
    setRuntimeMemberSummaryModel("");
    setRuntimeMemberTools("");
    setRuntimeMemberTimeoutMs("");
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
    setRunQuorum(null);
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
      const overridesMode = String(runOverridesMode || "").trim();
      if (overridesMode) teamPayload.run_overrides_mode = overridesMode;
      if (overridesMode === "explicit") {
        const rawOverrides = String(runMemberOverridesJson || "").trim();
        if (rawOverrides) {
          let parsed: any = null;
          try {
            parsed = JSON.parse(rawOverrides);
          } catch (err) {
            setRunError(`invalid member_overrides json: ${String(err)}`);
            return;
          }
          if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
            setRunError("member_overrides must be an object keyed by member_id");
            return;
          }
          teamPayload.member_overrides = parsed;
        }
      }
      const runtimeMembersRaw = String(runRuntimeMembersJson || "").trim();
      if (runtimeMembersRaw) {
        let parsed: any = null;
        try {
          parsed = JSON.parse(runtimeMembersRaw);
        } catch (err) {
          setRunError(`invalid runtime_members json: ${String(err)}`);
          return;
        }
        if (!Array.isArray(parsed)) {
          setRunError("runtime_members must be a JSON array of member objects");
          return;
        }
        teamPayload.runtime_members = parsed;
      }
      if (runApprovals.length > 0) {
        teamPayload.approvals = runApprovals;
      }
      const resp = await apiBrokerTeamRunCreate(props.base, tid, { run: runPayload, team: teamPayload }, props.auth);
      if (!resp.ok) {
        setRunResult(null);
        setRunError(resp.error || resp.err || resp.code || "team run failed");
        const quorum = (resp as any)?.quorum;
        if (quorum && typeof quorum === "object") {
          setRunQuorum(quorum as QuorumEval);
        }
        return;
      }
      setRunResult(resp);
      setRunQuorum(null);
      if (resp?.team_run_id) setRunLookupId(String(resp.team_run_id));
      setRunApprovals([]);
      setRunApprovalMemberId("");
      setRunApprovalRuleId("");
      setRunApprovalReason("");
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
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "approvals fetch failed");
      }
      const rows = Array.isArray(resp?.approvals) ? resp.approvals : [];
      setApprovals(rows);
      setApprovalsLastSyncMs(Date.now());
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
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "approvals update failed");
      }
      const rows = Array.isArray(resp?.approvals) ? resp.approvals : [];
      setApprovals(rows);
      setApprovalsLastSyncMs(Date.now());
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

  React.useEffect(() => {
    if (!canQuery || !teamIdTrimmed) return;
    void refreshMembers(teamIdTrimmed);
    void refreshRules(teamIdTrimmed);
  }, [canQuery, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    setRunResult(null);
    setRunError(null);
    setRunQuorum(null);
    setRunLookupResult(null);
    setRunLookupError(null);
    setRunLookupId("");
    setRunApprovals([]);
    setRunApprovalMemberId("");
    setRunApprovalRuleId("");
    setRunApprovalReason("");
    setApprovalRunId("");
    setApprovals(null);
    setApprovalsError(null);
    lastAutoApprovalRunIdRef.current = "";
  }, [teamIdTrimmed]);

  React.useEffect(() => {
    if (!approvalRunIdTrimmed) {
      lastAutoApprovalRunIdRef.current = "";
      return;
    }
    if (!canQuery || !teamIdTrimmed) return;
    if (approvalsBusy || approvals !== null) return;
    if (lastAutoApprovalRunIdRef.current === approvalRunIdTrimmed) return;
    lastAutoApprovalRunIdRef.current = approvalRunIdTrimmed;
    void handleApprovalsRefresh();
  }, [approvalRunIdTrimmed, approvals, approvalsBusy, canQuery, handleApprovalsRefresh, teamIdTrimmed]);

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
            data-testid="team-select"
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
          <FieldLabel>Capabilities</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberCapabilities}
            onChange={(e) => setMemberCapabilities(e.target.value)}
            placeholder="vision,audio"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Backend label</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberBackendLabel}
            onChange={(e) => setMemberBackendLabel(e.target.value)}
            placeholder="openrouter-main"
          />
          <FieldLabel>Model</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberModel}
            onChange={(e) => setMemberModel(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Base URL</FieldLabel>
          <input
            className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberBaseUrl}
            onChange={(e) => setMemberBaseUrl(e.target.value)}
            placeholder="https://api.openai.com/v1"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Summary model</FieldLabel>
          <input
            className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberSummaryModel}
            onChange={(e) => setMemberSummaryModel(e.target.value)}
            placeholder="optional"
          />
          <FieldLabel>Tools</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberTools}
            onChange={(e) => setMemberTools(e.target.value)}
          >
            <option value="">inherit</option>
            <option value="none">none</option>
            <option value="basic">basic</option>
            <option value="host">host</option>
          </select>
          <FieldLabel>Timeout ms</FieldLabel>
          <input
            className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={memberTimeoutMs}
            onChange={(e) => setMemberTimeoutMs(e.target.value)}
            placeholder="60000"
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
              const meta = m?.meta && typeof m.meta === "object" ? (m.meta as Record<string, any>) : null;
              const backendLabel = meta?.backend_label ? String(meta.backend_label) : "";
              const overridesRaw =
                meta?.run_overrides && typeof meta.run_overrides === "object" ? meta.run_overrides : null;
              const overrideBits: string[] = [];
              if (overridesRaw && typeof overridesRaw === "object") {
                const model = (overridesRaw as any).model ? String((overridesRaw as any).model) : "";
                const baseUrl = (overridesRaw as any).base_url ? String((overridesRaw as any).base_url) : "";
                const summaryModel = (overridesRaw as any).summary_model ? String((overridesRaw as any).summary_model) : "";
                const tools = (overridesRaw as any).tools ? String((overridesRaw as any).tools) : "";
                const timeoutMs = (overridesRaw as any).timeout_ms;
                const maxSteps = (overridesRaw as any).max_steps;
                const streamAssistant = (overridesRaw as any).stream_assistant;
                if (model) overrideBits.push(`model ${model}`);
                if (summaryModel) overrideBits.push(`summary ${summaryModel}`);
                if (baseUrl) overrideBits.push(`base ${baseUrl}`);
                if (tools) overrideBits.push(`tools ${tools}`);
                if (Number.isFinite(timeoutMs)) overrideBits.push(`timeout ${timeoutMs}ms`);
                if (Number.isFinite(maxSteps)) overrideBits.push(`max_steps ${maxSteps}`);
                if (typeof streamAssistant === "boolean") {
                  overrideBits.push(`stream ${streamAssistant ? "on" : "off"}`);
                }
              }
              const infoBits: string[] = [];
              if (typeof m?.weight === "number") infoBits.push(`weight ${m.weight}`);
              const caps = Array.isArray(m?.capabilities)
                ? m.capabilities.map((c) => String(c).trim()).filter(Boolean)
                : [];
              if (caps.length > 0) infoBits.push(`caps ${caps.join(",")}`);
              if (backendLabel) infoBits.push(`backend ${backendLabel}`);
              if (overrideBits.length > 0) infoBits.push(`overrides ${overrideBits.join(", ")}`);
              return (
                <div
                  key={`member-${mid}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-[11px] text-white/70">
                    <div>
                      <span className="text-white/90">{mid || "member"}</span>
                      {m?.role ? ` · role ${m.role}` : ""}
                      {m?.status ? ` · ${m.status}` : ""}
                      {m?.agent_id ? ` · agent ${m.agent_id}` : ""}
                      {m?.deployment_id ? ` · dep ${m.deployment_id}` : ""}
                      {m?.created_unix_ms ? ` · ${fmtTs(m.created_unix_ms)}` : ""}
                    </div>
                    {infoBits.length > 0 ? (
                      <div className="text-[10px] text-white/50">{infoBits.join(" · ")}</div>
                    ) : null}
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
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Run overrides</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={runOverridesMode}
            onChange={(e) => setRunOverridesMode(e.target.value)}
          >
            <option value="off">off</option>
            <option value="member_meta">member meta</option>
            <option value="explicit">explicit</option>
          </select>
          <span className="text-[11px] text-white/50">
            Apply per-member backend overrides when enabled.
          </span>
        </div>
        {runOverridesMode === "explicit" ? (
          <div className="grid gap-1">
            <FieldLabel>Member overrides JSON</FieldLabel>
            <textarea
              className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
              value={runMemberOverridesJson}
              onChange={(e) => setRunMemberOverridesJson(e.target.value)}
              placeholder='{"member_1":{"model":"gpt-4.1-mini","base_url":"https://api.openai.com/v1","tools":"basic"}}'
            />
            <div className="text-[11px] text-white/50">
              Allowed fields: model, base_url, summary_model, tools, timeout_ms, max_steps, stream_assistant.
            </div>
          </div>
        ) : null}
        <div className="grid gap-1">
          <FieldLabel>Runtime members JSON (optional)</FieldLabel>
          <textarea
            className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
            value={runRuntimeMembersJson}
            onChange={(e) => setRunRuntimeMembersJson(e.target.value)}
            placeholder='[{"member_id":"rt-1","agent_id":"agent_a","role":"executor","capabilities":["vision"],"meta":{"backend_label":"openai-mini"}}]'
          />
          <div className="text-[11px] text-white/50">
            Each entry needs agent_id + role; member_id is optional (required for explicit overrides).
          </div>
          <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
            <div className="text-[11px] text-white/70">Quick add runtime member</div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Member ID</FieldLabel>
              <input
                className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberId}
                onChange={(e) => setRuntimeMemberId(e.target.value)}
                placeholder="optional"
              />
              <FieldLabel>Agent ID</FieldLabel>
              <input
                className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberAgentId}
                onChange={(e) => setRuntimeMemberAgentId(e.target.value)}
                placeholder="agent1"
              />
              <FieldLabel>Role</FieldLabel>
              <input
                className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberRole}
                onChange={(e) => setRuntimeMemberRole(e.target.value)}
                placeholder="executor"
              />
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Deployment</FieldLabel>
              <input
                className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberDeploymentId}
                onChange={(e) => setRuntimeMemberDeploymentId(e.target.value)}
                placeholder="default"
              />
              <FieldLabel>Capabilities</FieldLabel>
              <input
                className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberCapabilities}
                onChange={(e) => setRuntimeMemberCapabilities(e.target.value)}
                placeholder="vision,audio"
              />
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Backend label</FieldLabel>
              <input
                className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberBackendLabel}
                onChange={(e) => setRuntimeMemberBackendLabel(e.target.value)}
                placeholder="openrouter-main"
              />
              <FieldLabel>Model</FieldLabel>
              <input
                className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberModel}
                onChange={(e) => setRuntimeMemberModel(e.target.value)}
                placeholder="optional"
              />
              <FieldLabel>Base URL</FieldLabel>
              <input
                className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberBaseUrl}
                onChange={(e) => setRuntimeMemberBaseUrl(e.target.value)}
                placeholder="https://api.openai.com/v1"
              />
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Summary model</FieldLabel>
              <input
                className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberSummaryModel}
                onChange={(e) => setRuntimeMemberSummaryModel(e.target.value)}
                placeholder="optional"
              />
              <FieldLabel>Tools</FieldLabel>
              <select
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberTools}
                onChange={(e) => setRuntimeMemberTools(e.target.value)}
              >
                <option value="">inherit</option>
                <option value="none">none</option>
                <option value="basic">basic</option>
                <option value="host">host</option>
              </select>
              <FieldLabel>Timeout ms</FieldLabel>
              <input
                className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={runtimeMemberTimeoutMs}
                onChange={(e) => setRuntimeMemberTimeoutMs(e.target.value)}
                placeholder="60000"
              />
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleAddRuntimeMember()}
              >
                Add runtime member
              </button>
              {runRuntimeMembersJson.trim() ? (
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setRunRuntimeMembersJson("")}
                >
                  Clear runtime members
                </button>
              ) : null}
            </div>
          </div>
        </div>
        <div
          className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2"
          data-testid="team-inline-approvals"
        >
          <div className="text-[11px] text-white/70">Inline approvals (optional)</div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Member ID</FieldLabel>
            <input
              className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runApprovalMemberId}
              onChange={(e) => setRunApprovalMemberId(e.target.value)}
              placeholder="member id"
              list="team-approvals-members"
            />
            <FieldLabel>Decision</FieldLabel>
            <select
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runApprovalDecision}
              onChange={(e) => setRunApprovalDecision(e.target.value as "approve" | "deny")}
            >
              <option value="approve">approve</option>
              <option value="deny">deny</option>
            </select>
            <FieldLabel>Rule ID</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runApprovalRuleId}
              onChange={(e) => setRunApprovalRuleId(e.target.value)}
              placeholder="optional"
              list="team-approvals-rules"
            />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Reason</FieldLabel>
            <input
              className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runApprovalReason}
              onChange={(e) => setRunApprovalReason(e.target.value)}
              placeholder="optional"
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!canQuery || !teamIdTrimmed || runBusy}
              onClick={() => handleAddRunApproval()}
            >
              Add approval
            </button>
            {runApprovals.length > 0 ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => setRunApprovals([])}
              >
                Clear approvals
              </button>
            ) : null}
          </div>
          {runApprovals.length > 0 ? (
            <div className="grid gap-2">
              {runApprovals.map((a, idx) => (
                <div
                  key={`run-approval-${a.member_id}-${a.rule_id || "any"}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div className="text-[11px] text-white/70">
                    <span className="text-white/90">{a.member_id}</span>
                    {a.decision ? ` · ${a.decision}` : ""}
                    {a.rule_id ? ` · rule ${a.rule_id}` : ""}
                    {a.reason ? ` · ${a.reason}` : ""}
                  </div>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() =>
                      setRunApprovals((prev) => prev.filter((_, i) => i !== idx))
                    }
                  >
                    Remove
                  </button>
                </div>
              ))}
            </div>
          ) : (
            <div className="text-[11px] text-white/50">No inline approvals.</div>
          )}
        </div>
        {runError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {runError}
          </div>
        ) : null}
        {runQuorum?.rules && runQuorum.rules.length > 0 ? (
          <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
            {runQuorum.rules.map((r, idx) => (
              <div key={`quorum-eval-${r.rule_id || idx}`}>
                {r.rule_id ? `rule ${r.rule_id}` : "rule"} · min {r.min_approvals ?? "?"} · approved{" "}
                {r.approved ?? 0} · missing {r.missing ?? 0}
              </div>
            ))}
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
          {approvalsLastSyncMs ? (
            <div className="text-[11px] text-white/50">Last sync: {fmtTs(approvalsLastSyncMs)}</div>
          ) : null}
          {quorumRequestRows.length > 0 ? (
            <div className="grid gap-2">
              <div className="text-[11px] text-white/60">Recent quorum requests</div>
              {quorumRequestRows.map((row, idx) => {
                const payload = row?.payload ?? {};
                const teamId = payload?.team_id ? String(payload.team_id) : "";
                const runId = payload?.team_run_id ? String(payload.team_run_id) : "";
                const ruleId = payload?.rule_id ? String(payload.rule_id) : "";
                const action = payload?.action ? String(payload.action) : "";
                const min = payload?.min_approvals;
                const ts = fmtTs(row?.ts_unix_ms);
                return (
                  <div
                    key={`quorum-request-${teamId}-${runId}-${ruleId}-${idx}`}
                    className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    <div className="text-[11px] text-white/70">
                      <span className="text-white/90">{action || "team_run"}</span>
                      {min !== undefined ? ` · min ${min}` : ""}
                      {ruleId ? ` · rule ${ruleId}` : ""}
                      {ts ? ` · ${ts}` : ""}
                    </div>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => {
                        if (teamId) setTeamId(teamId);
                        if (runId) {
                          setApprovalRunId(runId);
                          setRunLookupId(runId);
                        }
                        if (ruleId) setApprovalRuleId(ruleId);
                      }}
                    >
                      Use
                    </button>
                  </div>
                );
              })}
            </div>
          ) : (
            <div className="text-[11px] text-white/50">No quorum requests captured yet.</div>
          )}
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
