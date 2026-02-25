import React from "react";
import {
  apiBrokerListAgents,
  apiBrokerTeamMembersUpsert,
  apiBrokerTeamRunApprovalsCreate,
  apiBrokerTeamRunApprovalsList,
  apiBrokerTeamRunCancel,
  apiBrokerTeamRunCreate,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunList,
  apiBrokerTeamRunModeratorDirective,
  apiBrokerTeamRunModeratorEvents,
  apiBrokerTeamRunModeratorTask,
  apiBrokerTeamRunRuntimeMembersUpdate,
  type ApiAuth,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import FieldLabel from "../FieldLabel";
import TeamRunModeratorPanel from "./TeamRunModeratorPanel";
import type { BrokerEventRow, TeamMemberRow, TeamQuorumRuleRow } from "./types";

const fmtTs = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
};

const fmtSummary = (summary?: any) => {
  if (!summary || typeof summary !== "object") return "";
  const parts: string[] = [];
  const pushIf = (key: string, label: string) => {
    const val = summary?.[key];
    if (typeof val === "number") parts.push(`${label} ${val}`);
  };
  pushIf("total", "total");
  pushIf("queued", "queued");
  pushIf("running", "running");
  pushIf("done", "done");
  pushIf("error", "error");
  pushIf("cancelled", "cancelled");
  pushIf("interrupted", "interrupted");
  pushIf("unknown", "unknown");
  pushIf("ok", "ok");
  pushIf("failed", "failed");
  pushIf("dispatch_errors", "dispatch_errors");
  return parts.join(" · ");
};

const parseCsvList = (raw?: string | null) => {
  if (!raw) return [];
  return String(raw)
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean);
};

const TEAM_RUN_EVENT_TYPES = new Set([
  "team_run_created",
  "team_run_status",
  "team_runtime_members_updated",
  "team_quorum_request",
  "team_quorum_result",
]);

export type BrokerTeamRunPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  members: TeamMemberRow[];
  rules: TeamQuorumRuleRow[];
  quorumEvents?: BrokerEventRow[];
  teamMeta?: Record<string, any> | null;
  onMembersRefresh?: (teamId: string) => Promise<void> | void;
  onTeamSelect?: (teamId: string) => void;
};

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

export default function BrokerTeamRunPanel(props: BrokerTeamRunPanelProps) {
  const teamIdTrimmed = String(props.teamId || "").trim();
  const membersList = Array.isArray(props.members) ? props.members : [];
  const rulesList = Array.isArray(props.rules) ? props.rules : [];
  const quorumRequestRows = React.useMemo(() => {
    const rows = Array.isArray(props.quorumEvents) ? props.quorumEvents : [];
    const filtered = rows.filter((ev) => ev?.type === "team_quorum_request");
    if (!teamIdTrimmed) return filtered.slice(0, 6);
    return filtered.filter((ev) => String(ev?.payload?.team_id || "") === teamIdTrimmed).slice(0, 6);
  }, [props.quorumEvents, teamIdTrimmed]);

  const [runPrompt, setRunPrompt] = React.useState<string>("");
  const [runModel, setRunModel] = React.useState<string>("");
  const [runTools, setRunTools] = React.useState<string>("basic");
  const [runMode, setRunMode] = React.useState<string>("async");
  const [runRole, setRunRole] = React.useState<string>("");
  const [runRoles, setRunRoles] = React.useState<string>("");
  const [runConcurrency, setRunConcurrency] = React.useState<string>("1");
  const [runTimeoutMs, setRunTimeoutMs] = React.useState<string>("60000");
  const [runQuorumMode, setRunQuorumMode] = React.useState<string>("auto");
  const [runOverridesMode, setRunOverridesMode] = React.useState<string>("member_meta");
  const [runMemberOverridesJson, setRunMemberOverridesJson] = React.useState<string>("");
  const [runRoleOverridesJson, setRunRoleOverridesJson] = React.useState<string>("");
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
  const [runtimeAgentsBusy, setRuntimeAgentsBusy] = React.useState<boolean>(false);
  const [runtimeAgentsError, setRuntimeAgentsError] = React.useState<string | null>(null);
  const [runtimeAgents, setRuntimeAgents] = React.useState<any[] | null>(null);
  const [runtimeSaveBusy, setRuntimeSaveBusy] = React.useState<boolean>(false);
  const [runtimeSaveError, setRuntimeSaveError] = React.useState<string | null>(null);
  const runtimeImportRef = React.useRef<HTMLInputElement | null>(null);
  const [runtimeImportMerge, setRuntimeImportMerge] = React.useState<boolean>(true);
  const [runtimeUpdateMode, setRuntimeUpdateMode] = React.useState<string>("replace");
  const [runtimeUpdateBusy, setRuntimeUpdateBusy] = React.useState<boolean>(false);
  const [runtimeUpdateError, setRuntimeUpdateError] = React.useState<string | null>(null);
  const [runtimeUpdateNote, setRuntimeUpdateNote] = React.useState<string>("");

  const teamMetaObj = props.teamMeta && typeof props.teamMeta === "object" ? props.teamMeta : null;
  const teamRoleOverridesDefaults =
    teamMetaObj?.role_overrides && typeof teamMetaObj.role_overrides === "object"
      ? (teamMetaObj.role_overrides as Record<string, any>)
      : null;
  const teamRoleOverrideKeys = teamRoleOverridesDefaults
    ? Object.keys(teamRoleOverridesDefaults).map((k) => String(k)).filter(Boolean)
    : [];

  const runtimeMembersPreview = React.useMemo(() => {
    const raw = String(runRuntimeMembersJson || "").trim();
    if (!raw) return { items: [] as any[], error: "" };
    try {
      const parsed = JSON.parse(raw);
      if (!Array.isArray(parsed)) {
        return { items: [] as any[], error: "runtime_members must be a JSON array" };
      }
      return { items: parsed, error: "" };
    } catch (err) {
      return { items: [] as any[], error: `invalid runtime_members json: ${String(err)}` };
    }
  }, [runRuntimeMembersJson]);

  const runtimeAgentOptions = Array.isArray(runtimeAgents) ? runtimeAgents : [];
  const runtimeSelectedAgent = runtimeAgentOptions.find(
    (agent) => String(agent?.agent_id || "") === String(runtimeMemberAgentId || "").trim(),
  );
  const runtimeAgentDeployments = Array.isArray(runtimeSelectedAgent?.deployments)
    ? (runtimeSelectedAgent?.deployments as any[])
    : [];

  const runtimeSavePreview = React.useMemo(() => {
    const existingIDs = new Set<string>();
    for (const m of membersList) {
      const id = String(m?.member_id || "").trim();
      if (id) existingIDs.add(id);
    }
    const items = runtimeMembersPreview.items;
    if (!Array.isArray(items) || items.length === 0) {
      return { newMembers: [] as any[], skipped: [] as any[], invalid: [] as any[] };
    }
    const newMembers: any[] = [];
    const skipped: any[] = [];
    const invalid: any[] = [];
    for (const item of items) {
      const memberId = item?.member_id ? String(item.member_id).trim() : "";
      const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
      const role = item?.role ? String(item.role).trim() : "";
      if (!agentId || !role) {
        invalid.push({
          item,
          reason: !agentId && !role ? "missing agent_id and role" : !agentId ? "missing agent_id" : "missing role",
        });
        continue;
      }
      if (memberId && existingIDs.has(memberId)) {
        skipped.push({ item, reason: "member_id already exists" });
        continue;
      }
      newMembers.push({ item, reason: "new" });
    }
    return { newMembers, skipped, invalid };
  }, [membersList, runtimeMembersPreview.items]);

  const runtimeTeamDiff = React.useMemo(() => {
    const runtimeItems = runtimeMembersPreview.items;
    if (!Array.isArray(runtimeItems)) {
      return { runtimeOnly: [] as any[], teamOnly: [] as TeamMemberRow[], mismatched: [] as any[] };
    }
    const teamByMemberId = new Map<string, TeamMemberRow>();
    const teamByAgentId = new Map<string, TeamMemberRow>();
    for (const member of membersList) {
      const mid = String(member?.member_id || "").trim();
      const aid = String(member?.agent_id || "").trim();
      if (mid) teamByMemberId.set(mid, member);
      if (aid) teamByAgentId.set(aid, member);
    }
    const matched = new Set<string>();
    const runtimeOnly: any[] = [];
    const mismatched: any[] = [];
    for (const item of runtimeItems) {
      const memberId = item?.member_id ? String(item.member_id).trim() : "";
      const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
      const teamMatch =
        (memberId && teamByMemberId.get(memberId)) || (agentId && teamByAgentId.get(agentId));
      if (!teamMatch) {
        runtimeOnly.push(item);
        continue;
      }
      if (teamMatch?.member_id) matched.add(String(teamMatch.member_id));
      if (!teamMatch?.member_id && teamMatch?.agent_id) matched.add(String(teamMatch.agent_id));
      const diffs: string[] = [];
      const runtimeRole = item?.role ? String(item.role) : "";
      const runtimeAgent = agentId;
      const runtimeDep = item?.deployment_id ? String(item.deployment_id) : "";
      const runtimeStatus = item?.status ? String(item.status) : "";
      if (runtimeRole && teamMatch?.role && runtimeRole !== teamMatch.role) diffs.push("role");
      if (runtimeAgent && teamMatch?.agent_id && runtimeAgent !== teamMatch.agent_id) diffs.push("agent_id");
      if (runtimeDep && teamMatch?.deployment_id && runtimeDep !== teamMatch.deployment_id) {
        diffs.push("deployment_id");
      }
      if (runtimeStatus && teamMatch?.status && runtimeStatus !== teamMatch.status) diffs.push("status");
      if (diffs.length > 0) {
        mismatched.push({ item, team: teamMatch, diffs });
      }
    }
    const teamOnly = membersList.filter((member) => {
      const mid = String(member?.member_id || "").trim();
      const aid = String(member?.agent_id || "").trim();
      if (mid && matched.has(mid)) return false;
      if (!mid && aid && matched.has(aid)) return false;
      if (!mid && !aid) return false;
      return true;
    });
    return { runtimeOnly, teamOnly, mismatched };
  }, [membersList, runtimeMembersPreview.items]);

  const [runApprovals, setRunApprovals] = React.useState<InlineApproval[]>([]);
  const [runApprovalMemberId, setRunApprovalMemberId] = React.useState<string>("");
  const [runApprovalRuleId, setRunApprovalRuleId] = React.useState<string>("");
  const [runApprovalDecision, setRunApprovalDecision] = React.useState<"approve" | "deny">("approve");
  const [runApprovalReason, setRunApprovalReason] = React.useState<string>("");
  const [runBusy, setRunBusy] = React.useState<boolean>(false);
  const [runError, setRunError] = React.useState<string | null>(null);
  const [runResult, setRunResult] = React.useState<any | null>(null);
  const [runQuorum, setRunQuorum] = React.useState<QuorumEval | null>(null);
  const [runLookupId, setRunLookupId] = useLocalStorageState<string>("agentui.teamRunLookupId", "");
  const [runLookupBusy, setRunLookupBusy] = React.useState<boolean>(false);
  const [runLookupError, setRunLookupError] = React.useState<string | null>(null);
  const [runLookupResult, setRunLookupResult] = React.useState<any | null>(null);
  const [runCancelBusy, setRunCancelBusy] = React.useState<boolean>(false);
  const [runCancelError, setRunCancelError] = React.useState<string | null>(null);
  const [runCancelNote, setRunCancelNote] = React.useState<string>("");
  const [runOverridesExpanded, setRunOverridesExpanded] = React.useState<boolean>(false);
  const [autoRefreshRunLookup, setAutoRefreshRunLookup] = useLocalStorageState<boolean>(
    "agentui.teamRunLookupAuto",
    true,
  );
  const [runLookupLastEvent, setRunLookupLastEvent] = React.useState<string>("");
  const [moderatorDirective, setModeratorDirective] = React.useState<string>("");
  const [moderatorDirectiveScope, setModeratorDirectiveScope] = React.useState<string>("");
  const [moderatorTaskTitle, setModeratorTaskTitle] = React.useState<string>("");
  const [moderatorTaskDetail, setModeratorTaskDetail] = React.useState<string>("");
  const [moderatorTaskStatus, setModeratorTaskStatus] = React.useState<string>("");
  const [moderatorTargetRoles, setModeratorTargetRoles] = React.useState<string>("");
  const [moderatorTargetMembers, setModeratorTargetMembers] = React.useState<string>("");
  const [moderatorTargetAgents, setModeratorTargetAgents] = React.useState<string>("");
  const [moderatorAssignees, setModeratorAssignees] = React.useState<string>("");
  const [moderatorAppendToSession, setModeratorAppendToSession] = React.useState<boolean>(false);
  const [moderatorBusy, setModeratorBusy] = React.useState<boolean>(false);
  const [moderatorError, setModeratorError] = React.useState<string | null>(null);
  const [moderatorSuccess, setModeratorSuccess] = React.useState<string | null>(null);
  const [moderatorEvents, setModeratorEvents] = React.useState<any[]>([]);
  const [moderatorEventsBusy, setModeratorEventsBusy] = React.useState<boolean>(false);
  const [moderatorEventsError, setModeratorEventsError] = React.useState<string | null>(null);
  const [moderatorEventsTypes, setModeratorEventsTypes] = React.useState<string>(
    "moderator_directive,moderator_task_published",
  );
  const [moderatorEventsMaxBytes, setModeratorEventsMaxBytes] = React.useState<string>("1048576");
  const [moderatorEventsLimit, setModeratorEventsLimit] = React.useState<string>("200");
  const [moderatorEventsExpanded, setModeratorEventsExpanded] = React.useState<boolean>(false);
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
  const [recentRuns, setRecentRuns] = React.useState<any[]>([]);
  const [recentRunsBusy, setRecentRunsBusy] = React.useState<boolean>(false);
  const [recentRunsError, setRecentRunsError] = React.useState<string | null>(null);
  const [recentRunsLive, setRecentRunsLive] = useLocalStorageState<boolean>("agentui.teamRunListAuto", false);
  const [recentRunsLimit, setRecentRunsLimit] = useLocalStorageState<number>("agentui.teamRunListLimit", 10);
  const [recentRunsStatus, setRecentRunsStatus] = useLocalStorageState<string>("agentui.teamRunListStatus", "");
  const recentRunsEventRef = React.useRef<string>("");

  const approvalRunIdTrimmed = String(approvalRunId || runLookupId || "").trim();
  const recentRunsItems = Array.isArray(recentRuns) ? recentRuns : [];
  const runMemberSessions = React.useMemo(() => {
    const raw = runLookupResult?.member_sessions;
    if (!raw || typeof raw !== "object") return [] as Array<{ memberId: string; sessionId: string }>;
    return Object.entries(raw as Record<string, any>)
      .map(([memberId, sessionId]) => ({
        memberId: String(memberId || ""),
        sessionId: String(sessionId || ""),
      }))
      .filter((row) => row.memberId && row.sessionId);
  }, [runLookupResult]);
  const runMemberOptions = React.useMemo(() => {
    const out = new Set<string>();
    const members = Array.isArray(runLookupResult?.members) ? runLookupResult.members : [];
    const runtime = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    for (const m of members) {
      const mid = String(m?.member_id || "").trim();
      if (mid) out.add(mid);
    }
    for (const m of runtime) {
      const mid = String(m?.member_id || "").trim();
      if (mid) out.add(mid);
    }
    return Array.from(out);
  }, [runLookupResult]);
  const runAgentOptions = React.useMemo(() => {
    const out = new Set<string>();
    const members = Array.isArray(runLookupResult?.members) ? runLookupResult.members : [];
    const runtime = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    for (const m of members) {
      const aid = String(m?.agent_id || "").trim();
      if (aid) out.add(aid);
    }
    for (const m of runtime) {
      const aid = String(m?.agent_id || "").trim();
      if (aid) out.add(aid);
    }
    return Array.from(out);
  }, [runLookupResult]);
  const runRoleOptions = React.useMemo(() => {
    const out = new Set<string>();
    const members = Array.isArray(runLookupResult?.members) ? runLookupResult.members : [];
    const runtime = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    for (const m of members) {
      const role = String(m?.role || "").trim();
      if (role) out.add(role);
    }
    for (const m of runtime) {
      const role = String(m?.role || "").trim();
      if (role) out.add(role);
    }
    return Array.from(out);
  }, [runLookupResult]);

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

  const handleCreateRun = async () => {
    if (!teamIdTrimmed) return;
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
      const mode = String(runMode || "").trim();
      if (mode) teamPayload.mode = mode;
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
      const roleOverridesRaw = String(runRoleOverridesJson || "").trim();
      if (roleOverridesRaw) {
        let parsed: any = null;
        try {
          parsed = JSON.parse(roleOverridesRaw);
        } catch (err) {
          setRunError(`invalid role_overrides json: ${String(err)}`);
          return;
        }
        if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
          setRunError("role_overrides must be an object keyed by role");
          return;
        }
        teamPayload.role_overrides = parsed;
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
      const resp = await apiBrokerTeamRunCreate(props.base, teamIdTrimmed, { run: runPayload, team: teamPayload }, props.auth);
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

  const handleSeedRoleOverrides = () => {
    if (!teamRoleOverridesDefaults) return;
    try {
      setRunRoleOverridesJson(JSON.stringify(teamRoleOverridesDefaults, null, 2));
    } catch {
      setRunRoleOverridesJson("");
    }
  };

  const fetchRunStatus = React.useCallback(
    async (runId: string) => {
      if (!teamIdTrimmed || !runId) {
        setRunLookupError("missing team_id or run id");
        return;
      }
      setRunLookupError(null);
      setRunLookupBusy(true);
      try {
        const resp = await apiBrokerTeamRunGet(props.base, teamIdTrimmed, runId, props.auth);
        setRunLookupResult(resp);
      } catch (err) {
        setRunLookupError(String(err));
      } finally {
        setRunLookupBusy(false);
      }
    },
    [teamIdTrimmed, props.base, props.auth],
  );

  const handleRunLookup = async () => {
    const runId = String(runLookupId || "").trim();
    await fetchRunStatus(runId);
  };

  const loadRecentRuns = React.useCallback(async () => {
    if (!teamIdTrimmed) {
      setRecentRuns([]);
      return;
    }
    setRecentRunsBusy(true);
    setRecentRunsError(null);
    try {
      const resp = await apiBrokerTeamRunList(props.base, teamIdTrimmed, props.auth, {
        limit: recentRunsLimit,
        status: recentRunsStatus.trim() ? recentRunsStatus.trim() : undefined,
      });
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team runs list failed");
      }
      setRecentRuns(Array.isArray(resp.runs) ? resp.runs : []);
    } catch (err) {
      setRecentRunsError(String(err));
    } finally {
      setRecentRunsBusy(false);
    }
  }, [teamIdTrimmed, props.base, props.auth, recentRunsLimit, recentRunsStatus]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRecentRuns([]);
      return;
    }
    void loadRecentRuns();
  }, [teamIdTrimmed, loadRecentRuns]);

  React.useEffect(() => {
    if (!recentRunsLive || !props.canQuery) return;
    const rows = Array.isArray(props.quorumEvents) ? props.quorumEvents : [];
    if (rows.length === 0) return;
    let nextKey = "";
    for (let i = rows.length - 1; i >= 0; i -= 1) {
      const ev = rows[i];
      if (!ev) continue;
      const type = String(ev?.type || "");
      if (!TEAM_RUN_EVENT_TYPES.has(type)) continue;
      const evTeamId = String(ev?.payload?.team_id || "");
      if (teamIdTrimmed && evTeamId && evTeamId !== teamIdTrimmed) continue;
      const evRunId = String(ev?.payload?.team_run_id || "");
      nextKey = ev?.event_id
        ? `id:${String(ev.event_id)}`
        : `ts:${Number(ev?.ts_unix_ms || 0)}|type:${type}|run:${evRunId}`;
      break;
    }
    if (!nextKey) return;
    if (recentRunsEventRef.current === nextKey) return;
    recentRunsEventRef.current = nextKey;
    void loadRecentRuns();
  }, [recentRunsLive, props.canQuery, props.quorumEvents, teamIdTrimmed, loadRecentRuns]);

  React.useEffect(() => {
    if (!autoRefreshRunLookup || !props.canQuery) return;
    const runId = resolveRunId();
    if (!runId) return;
    const rows = Array.isArray(props.quorumEvents) ? props.quorumEvents : [];
    if (rows.length === 0) return;
    let nextKey = "";
    for (let i = rows.length - 1; i >= 0; i -= 1) {
      const ev = rows[i];
      if (!ev) continue;
      const type = String(ev?.type || "");
      if (!TEAM_RUN_EVENT_TYPES.has(type)) continue;
      const evTeamId = String(ev?.payload?.team_id || "");
      if (teamIdTrimmed && evTeamId && evTeamId !== teamIdTrimmed) continue;
      const evRunId = String(ev?.payload?.team_run_id || "");
      if (evRunId && evRunId !== runId) continue;
      nextKey = ev?.event_id
        ? `id:${String(ev.event_id)}`
        : `ts:${Number(ev?.ts_unix_ms || 0)}|type:${type}|run:${evRunId}`;
      break;
    }
    if (!nextKey) return;
    if (runLookupLastEvent === nextKey) return;
    setRunLookupLastEvent(nextKey);
    void fetchRunStatus(runId);
  }, [
    autoRefreshRunLookup,
    props.canQuery,
    props.quorumEvents,
    teamIdTrimmed,
    runLookupLastEvent,
    fetchRunStatus,
  ]);

  const handleRunCancel = async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setRunCancelError("missing team_id or run id");
      return;
    }
    setRunCancelError(null);
    setRunCancelNote("");
    setRunCancelBusy(true);
    try {
      const resp = await apiBrokerTeamRunCancel(props.base, teamIdTrimmed, runId, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "team run cancel failed");
      }
      setRunLookupResult(resp);
      if (resp?.team_run_id) setRunLookupId(String(resp.team_run_id));
      setRunCancelNote("cancel requested");
    } catch (err) {
      setRunCancelError(String(err));
    } finally {
      setRunCancelBusy(false);
    }
  };

  const buildModeratorTargets = React.useCallback(() => {
    const roles = parseCsvList(moderatorTargetRoles).map((r) => r.toLowerCase());
    const members = parseCsvList(moderatorTargetMembers);
    const agents = parseCsvList(moderatorTargetAgents);
    if (roles.length === 0 && members.length === 0 && agents.length === 0) return undefined;
    return { roles, member_ids: members, agent_ids: agents };
  }, [moderatorTargetRoles, moderatorTargetMembers, moderatorTargetAgents]);

  const handleModeratorDirectivePublish = async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setModeratorError("missing team_id or run id");
      return;
    }
    const directive = String(moderatorDirective || "").trim();
    if (!directive) {
      setModeratorError("directive required");
      return;
    }
    setModeratorBusy(true);
    setModeratorError(null);
    setModeratorSuccess(null);
    try {
      const payload: Record<string, any> = {
        directive,
        scope: String(moderatorDirectiveScope || "").trim() || undefined,
        assignees: parseCsvList(moderatorAssignees),
        targets: buildModeratorTargets(),
        append_to_session: moderatorAppendToSession,
      };
      if (!payload.scope) delete payload.scope;
      if (!payload.assignees.length) delete payload.assignees;
      if (!payload.targets) delete payload.targets;
      const resp = await apiBrokerTeamRunModeratorDirective(props.base, teamIdTrimmed, runId, payload, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "moderator directive failed");
      }
      const dispatched = Array.isArray(resp.dispatched) ? resp.dispatched.length : 0;
      const skipped = Array.isArray(resp.skipped) ? resp.skipped.length : 0;
      setModeratorSuccess(`directive dispatched to ${dispatched}${skipped ? ` (skipped ${skipped})` : ""}`);
      setModeratorDirective("");
      setModeratorDirectiveScope("");
    } catch (err) {
      setModeratorError(String(err));
    } finally {
      setModeratorBusy(false);
    }
  };

  const handleModeratorTaskPublish = async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setModeratorError("missing team_id or run id");
      return;
    }
    const title = String(moderatorTaskTitle || "").trim();
    if (!title) {
      setModeratorError("task title required");
      return;
    }
    setModeratorBusy(true);
    setModeratorError(null);
    setModeratorSuccess(null);
    try {
      const payload: Record<string, any> = {
        title,
        detail: String(moderatorTaskDetail || "").trim() || undefined,
        status: String(moderatorTaskStatus || "").trim() || undefined,
        assignees: parseCsvList(moderatorAssignees),
        targets: buildModeratorTargets(),
        append_to_session: moderatorAppendToSession,
      };
      if (!payload.detail) delete payload.detail;
      if (!payload.status) delete payload.status;
      if (!payload.assignees.length) delete payload.assignees;
      if (!payload.targets) delete payload.targets;
      const resp = await apiBrokerTeamRunModeratorTask(props.base, teamIdTrimmed, runId, payload, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "moderator task failed");
      }
      const dispatched = Array.isArray(resp.dispatched) ? resp.dispatched.length : 0;
      const skipped = Array.isArray(resp.skipped) ? resp.skipped.length : 0;
      setModeratorSuccess(`task dispatched to ${dispatched}${skipped ? ` (skipped ${skipped})` : ""}`);
      setModeratorTaskTitle("");
      setModeratorTaskDetail("");
      setModeratorTaskStatus("");
    } catch (err) {
      setModeratorError(String(err));
    } finally {
      setModeratorBusy(false);
    }
  };

  const handleModeratorEventsLoad = async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setModeratorEventsError("missing team_id or run id");
      return;
    }
    setModeratorEventsBusy(true);
    setModeratorEventsError(null);
    try {
      const maxBytesRaw = Number.parseInt(String(moderatorEventsMaxBytes || ""), 10);
      const limitRaw = Number.parseInt(String(moderatorEventsLimit || ""), 10);
      const resp = await apiBrokerTeamRunModeratorEvents(
        props.base,
        teamIdTrimmed,
        runId,
        {
          types: String(moderatorEventsTypes || "").trim(),
          maxBytes: Number.isFinite(maxBytesRaw) ? maxBytesRaw : undefined,
          limit: Number.isFinite(limitRaw) ? limitRaw : undefined,
          roles: String(moderatorTargetRoles || "").trim(),
          memberIds: String(moderatorTargetMembers || "").trim(),
          agentIds: String(moderatorTargetAgents || "").trim(),
        },
        props.auth,
      );
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "moderator events failed");
      }
      setModeratorEvents(Array.isArray(resp.events) ? resp.events : []);
      if (resp.skipped && Array.isArray(resp.skipped) && resp.skipped.length > 0) {
        setModeratorSuccess(`loaded ${resp.events?.length ?? 0} events (skipped ${resp.skipped.length})`);
      } else {
        setModeratorSuccess(`loaded ${resp.events?.length ?? 0} events`);
      }
    } catch (err) {
      setModeratorEventsError(String(err));
    } finally {
      setModeratorEventsBusy(false);
    }
  };

  const handleRuntimeMembersLoadFromRun = () => {
    const runtimeMembers = runLookupResult?.runtime_members;
    if (!Array.isArray(runtimeMembers) || runtimeMembers.length === 0) {
      setRuntimeUpdateError("no runtime members to load");
      return;
    }
    setRunRuntimeMembersJson(JSON.stringify(runtimeMembers, null, 2));
    setRuntimeUpdateError(null);
  };

  const resolveRunId = () =>
    String(runLookupResult?.team_run_id || runLookupId || runResult?.team_run_id || "").trim();

  const applyRuntimeMembersUpdate = async (members: any[], mode: string) => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setRuntimeUpdateError("missing team_id or run id");
      return;
    }
    setRuntimeUpdateError(null);
    setRuntimeUpdateNote("");
    setRuntimeUpdateBusy(true);
    try {
      const resp = await apiBrokerTeamRunRuntimeMembersUpdate(
        props.base,
        teamIdTrimmed,
        runId,
        { mode, runtime_members: members },
        props.auth,
      );
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "runtime members update failed");
      }
      setRunLookupResult(resp);
      if (Array.isArray(resp.runtime_members)) {
        setRunRuntimeMembersJson(JSON.stringify(resp.runtime_members, null, 2));
        setRuntimeUpdateNote(`updated ${resp.runtime_members.length} runtime members`);
      } else {
        setRunRuntimeMembersJson(JSON.stringify(members, null, 2));
        setRuntimeUpdateNote(`updated ${members.length} runtime members`);
      }
    } catch (err) {
      setRuntimeUpdateError(String(err));
    } finally {
      setRuntimeUpdateBusy(false);
    }
  };

  const handleRuntimeMembersUpdate = async () => {
    const raw = String(runRuntimeMembersJson || "").trim();
    if (!raw) {
      setRuntimeUpdateError("runtime members json required");
      return;
    }
    let parsed: any = null;
    try {
      parsed = JSON.parse(raw);
    } catch (err) {
      setRuntimeUpdateError(`invalid runtime_members json: ${String(err)}`);
      return;
    }
    if (!Array.isArray(parsed)) {
      setRuntimeUpdateError("runtime_members must be a JSON array of member objects");
      return;
    }
    const mode = String(runtimeUpdateMode || "").trim() || "replace";
    await applyRuntimeMembersUpdate(parsed, mode);
  };

  const handleRuntimeMemberToggle = async (member: any) => {
    const runtimeMembers = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    if (runtimeMembers.length === 0) {
      setRuntimeUpdateError("load run status first");
      return;
    }
    const memberId = String(member?.member_id || "").trim();
    const agentId = String(member?.agent_id || "").trim();
    if (!memberId && !agentId) {
      setRuntimeUpdateError("runtime member missing id");
      return;
    }
    const updated = runtimeMembers.map((item: any) => {
      const mid = String(item?.member_id || "").trim();
      const aid = String(item?.agent_id || "").trim();
      const match = memberId ? mid === memberId : aid === agentId;
      if (!match) return item;
      const current = String(item?.status || "active").toLowerCase();
      const next = current === "paused" ? "active" : "paused";
      return { ...item, status: next };
    });
    await applyRuntimeMembersUpdate(updated, "replace");
  };

  const handleRuntimeMemberRemove = async (member: any) => {
    const runtimeMembers = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    if (runtimeMembers.length === 0) {
      setRuntimeUpdateError("load run status first");
      return;
    }
    const memberId = String(member?.member_id || "").trim();
    const agentId = String(member?.agent_id || "").trim();
    if (!memberId && !agentId) {
      setRuntimeUpdateError("runtime member missing id");
      return;
    }
    const updated = runtimeMembers.filter((item: any) => {
      const mid = String(item?.member_id || "").trim();
      const aid = String(item?.agent_id || "").trim();
      if (memberId) return mid !== memberId;
      return aid !== agentId;
    });
    await applyRuntimeMembersUpdate(updated, "replace");
  };

  const handleApprovalsRefresh = async () => {
    const runId = approvalRunIdTrimmed;
    if (!teamIdTrimmed || !runId) {
      setApprovalsError("missing team_id or run id");
      return;
    }
    setApprovalsError(null);
    setApprovalsBusy(true);
    try {
      const resp = await apiBrokerTeamRunApprovalsList(props.base, teamIdTrimmed, runId, props.auth);
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
    const runId = approvalRunIdTrimmed;
    const memberId = String(approvalMemberId || "").trim();
    const decision = String(approvalDecision || "").trim().toLowerCase();
    const ruleId = String(approvalRuleId || "").trim();
    const reason = String(approvalReason || "").trim();
    if (!teamIdTrimmed || !runId) {
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
      const resp = await apiBrokerTeamRunApprovalsCreate(props.base, teamIdTrimmed, runId, payload, props.auth);
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

  const refreshRuntimeAgents = async () => {
    if (!props.canQuery) return;
    setRuntimeAgentsError(null);
    setRuntimeAgentsBusy(true);
    try {
      const resp = await apiBrokerListAgents(props.base, props.auth);
      const rows = Array.isArray(resp?.agents) ? resp.agents : [];
      setRuntimeAgents(rows);
    } catch (err) {
      setRuntimeAgentsError(String(err));
    } finally {
      setRuntimeAgentsBusy(false);
    }
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

  const handleRemoveRuntimeMember = (idx: number) => {
    const rawItems = runtimeMembersPreview.items;
    if (!Array.isArray(rawItems) || idx < 0 || idx >= rawItems.length) return;
    const next = rawItems.filter((_, i) => i !== idx);
    setRunRuntimeMembersJson(next.length > 0 ? JSON.stringify(next, null, 2) : "");
  };

  const handleToggleRuntimeMemberStatus = (idx: number) => {
    const rawItems = runtimeMembersPreview.items;
    if (!Array.isArray(rawItems) || idx < 0 || idx >= rawItems.length) return;
    const next = rawItems.map((item, i) => {
      if (i !== idx) return item;
      const statusRaw = item?.status ? String(item.status).toLowerCase() : "";
      const nextStatus = statusRaw === "paused" ? "active" : "paused";
      return { ...item, status: nextStatus };
    });
    setRunRuntimeMembersJson(JSON.stringify(next, null, 2));
  };

  const handleSetAllRuntimeStatus = (status: "active" | "paused") => {
    const rawItems = runtimeMembersPreview.items;
    if (!Array.isArray(rawItems) || rawItems.length === 0) return;
    const next = rawItems.map((item) => ({ ...item, status }));
    setRunRuntimeMembersJson(JSON.stringify(next, null, 2));
  };

  const handleRemovePausedRuntimeMembers = () => {
    const rawItems = runtimeMembersPreview.items;
    if (!Array.isArray(rawItems) || rawItems.length === 0) return;
    const filtered = rawItems.filter((item) => {
      const statusRaw = item?.status ? String(item.status).toLowerCase() : "active";
      return statusRaw !== "paused";
    });
    setRunRuntimeMembersJson(filtered.length > 0 ? JSON.stringify(filtered, null, 2) : "");
  };

  const handleCompactRuntimeMembers = () => {
    if (runtimeMembersPreview.error) {
      setRunError(runtimeMembersPreview.error);
      return;
    }
    const rawItems = runtimeMembersPreview.items;
    if (!Array.isArray(rawItems) || rawItems.length === 0) return;
    const compacted = rawItems
      .map((item) => {
        const out: Record<string, any> = {};
        const memberId = item?.member_id ? String(item.member_id).trim() : "";
        const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
        const deploymentId = item?.deployment_id ? String(item.deployment_id).trim() : "";
        const role = item?.role ? String(item.role).trim() : "";
        const status = item?.status ? String(item.status).trim().toLowerCase() : "";
        if (memberId) out.member_id = memberId;
        if (agentId) out.agent_id = agentId;
        if (deploymentId) out.deployment_id = deploymentId;
        if (role) out.role = role;
        if (status && status !== "active") out.status = status;
        if (Array.isArray(item?.capabilities)) {
          const caps = item.capabilities.map((c: any) => String(c).trim()).filter(Boolean);
          if (caps.length > 0) out.capabilities = caps;
        }
        if (typeof item?.weight === "number") out.weight = item.weight;
        if (item?.meta && typeof item.meta === "object") {
          const meta: Record<string, any> = {};
          for (const [k, v] of Object.entries(item.meta as Record<string, any>)) {
            if (v === null || v === undefined) continue;
            if (typeof v === "string" && v.trim() === "") continue;
            if (Array.isArray(v) && v.length === 0) continue;
            if (typeof v === "object" && !Array.isArray(v) && Object.keys(v).length === 0) continue;
            meta[k] = v;
          }
          if (Object.keys(meta).length > 0) out.meta = meta;
        }
        return out;
      })
      .filter((item) => Object.keys(item).length > 0);
    setRunRuntimeMembersJson(compacted.length > 0 ? JSON.stringify(compacted, null, 2) : "");
  };

  const handleCopyRuntimeMembers = async () => {
    const payload = String(runRuntimeMembersJson || "").trim();
    if (!payload) {
      setRunError("runtime members json is empty");
      return;
    }
    if (runtimeMembersPreview.error) {
      setRunError(runtimeMembersPreview.error);
      return;
    }
    try {
      await navigator.clipboard.writeText(payload);
      setRunError(null);
    } catch (err) {
      setRunError(`copy failed: ${String(err)}`);
    }
  };

  const handleImportRuntimeMembers = async (event: React.ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      const text = await file.text();
      const trimmed = text.trim();
      if (!runtimeImportMerge) {
        setRunRuntimeMembersJson(trimmed);
      } else {
        let incoming: any = null;
        try {
          incoming = JSON.parse(trimmed);
        } catch {
          setRunRuntimeMembersJson(trimmed);
          setRunError(null);
          return;
        }
        const existing = runtimeMembersPreview.items;
        if (!Array.isArray(incoming)) {
          setRunRuntimeMembersJson(trimmed);
          setRunError(null);
          return;
        }
        const merged = Array.isArray(existing) ? [...existing] : [];
        for (const item of incoming) {
          const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
          const memberId = item?.member_id ? String(item.member_id).trim() : "";
          const exists = merged.some((m: any) => {
            const mid = m?.member_id ? String(m.member_id).trim() : "";
            const aid = m?.agent_id ? String(m.agent_id).trim() : "";
            if (memberId && mid) return memberId === mid;
            if (agentId && aid) return agentId === aid;
            return false;
          });
          if (!exists) {
            merged.push(item);
          }
        }
        setRunRuntimeMembersJson(merged.length > 0 ? JSON.stringify(merged, null, 2) : "");
      }
      setRunError(null);
    } catch (err) {
      setRunError(`import failed: ${String(err)}`);
    } finally {
      event.target.value = "";
    }
  };

  const handleDownloadRuntimeMembers = () => {
    const payload = String(runRuntimeMembersJson || "").trim();
    if (!payload) {
      setRunError("runtime members json is empty");
      return;
    }
    const blob = new Blob([payload], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "runtime_members.json";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    URL.revokeObjectURL(url);
    setRunError(null);
  };

  const handleExportTeamMembers = () => {
    if (membersList.length === 0) {
      setRunError("no team members to export");
      return;
    }
    const payload = membersList.map((m) => {
      const entry: Record<string, any> = {};
      const memberId = String(m?.member_id || "").trim();
      const agentId = String(m?.agent_id || "").trim();
      const deploymentId = String(m?.deployment_id || "").trim();
      const role = String(m?.role || "").trim();
      const status = String(m?.status || "").trim();
      if (memberId) entry.member_id = memberId;
      if (agentId) entry.agent_id = agentId;
      if (deploymentId) entry.deployment_id = deploymentId;
      if (role) entry.role = role;
      if (status && status !== "active") entry.status = status;
      if (typeof m?.weight === "number") entry.weight = m.weight;
      if (Array.isArray(m?.capabilities)) {
        const caps = m.capabilities.map((c) => String(c).trim()).filter(Boolean);
        if (caps.length > 0) entry.capabilities = caps;
      }
      if (m?.meta && typeof m.meta === "object") {
        const meta: Record<string, any> = {};
        for (const [k, v] of Object.entries(m.meta)) {
          if (v === null || v === undefined) continue;
          if (typeof v === "string" && v.trim() === "") continue;
          if (Array.isArray(v) && v.length === 0) continue;
          if (typeof v === "object" && !Array.isArray(v) && Object.keys(v).length === 0) continue;
          meta[k] = v;
        }
        if (Object.keys(meta).length > 0) entry.meta = meta;
      }
      return entry;
    });
    setRunRuntimeMembersJson(JSON.stringify(payload, null, 2));
    setRunError(null);
  };

  const handleSeedExplicitOverrides = () => {
    if (membersList.length === 0) {
      setRunError("no team members loaded");
      return;
    }
    const seed: Record<string, any> = {};
    for (const member of membersList) {
      const mid = String(member?.member_id || "").trim();
      if (!mid) continue;
      const meta = member?.meta && typeof member.meta === "object" ? (member.meta as Record<string, any>) : null;
      const overridesRaw =
        meta?.run_overrides && typeof meta.run_overrides === "object" ? (meta.run_overrides as Record<string, any>) : null;
      const entry: Record<string, any> = {};
      if (overridesRaw) {
        const model = overridesRaw.model ? String(overridesRaw.model) : "";
        const baseUrl = overridesRaw.base_url ? String(overridesRaw.base_url) : "";
        const summaryModel = overridesRaw.summary_model ? String(overridesRaw.summary_model) : "";
        const tools = overridesRaw.tools ? String(overridesRaw.tools) : "";
        const timeoutMs = overridesRaw.timeout_ms;
        const maxSteps = overridesRaw.max_steps;
        const streamAssistant = overridesRaw.stream_assistant;
        if (model) entry.model = model;
        if (baseUrl) entry.base_url = baseUrl;
        if (summaryModel) entry.summary_model = summaryModel;
        if (tools) entry.tools = tools;
        if (Number.isFinite(timeoutMs)) entry.timeout_ms = timeoutMs;
        if (Number.isFinite(maxSteps)) entry.max_steps = maxSteps;
        if (typeof streamAssistant === "boolean") entry.stream_assistant = streamAssistant;
      }
      if (Object.keys(entry).length > 0) {
        seed[mid] = entry;
      }
    }
    setRunOverridesMode("explicit");
    setRunMemberOverridesJson(JSON.stringify(seed, null, 2));
    setRunError(null);
  };

  const handleFixInvalidRuntimeMembers = () => {
    if (runtimeMembersPreview.error) {
      setRunError(runtimeMembersPreview.error);
      return;
    }
    const items = runtimeMembersPreview.items;
    if (!Array.isArray(items) || items.length === 0) return;
    const roleDefault = String(runtimeMemberRole || "").trim() || "executor";
    const fixed: any[] = [];
    for (const item of items) {
      const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
      const role = item?.role ? String(item.role).trim() : "";
      if (!agentId) {
        continue;
      }
      if (!role) {
        fixed.push({ ...item, role: roleDefault });
      } else {
        fixed.push(item);
      }
    }
    setRunRuntimeMembersJson(fixed.length > 0 ? JSON.stringify(fixed, null, 2) : "");
  };

  const handleAddConnectedAgents = () => {
    if (runtimeMembersPreview.error) {
      setRunError(runtimeMembersPreview.error);
      return;
    }
    if (runtimeAgentOptions.length === 0) {
      setRunError("load agents before adding connected agents");
      return;
    }
    const role = String(runtimeMemberRole || "").trim() || "executor";
    const existingRuntime = runtimeMembersPreview.items;
    const seenAgentIds = new Set<string>();
    for (const m of membersList) {
      const aid = String(m?.agent_id || "").trim();
      if (aid) seenAgentIds.add(aid);
    }
    for (const item of existingRuntime) {
      const aid = item?.agent_id ? String(item.agent_id).trim() : "";
      if (aid) seenAgentIds.add(aid);
    }
    const additions: any[] = [];
    for (const agent of runtimeAgentOptions) {
      const aid = String(agent?.agent_id || "").trim();
      if (!aid || seenAgentIds.has(aid)) continue;
      const deployments = Array.isArray(agent?.deployments) ? agent.deployments : [];
      const connected = agent?.connected === true || deployments.length > 0;
      if (!connected) continue;
      const entry: Record<string, any> = { agent_id: aid, role };
      if (deployments.length > 0) {
        const depId = deployments[0]?.deployment_id ? String(deployments[0].deployment_id) : "";
        if (depId) entry.deployment_id = depId;
      }
      additions.push(entry);
      seenAgentIds.add(aid);
    }
    if (additions.length === 0) {
      setRunError("no connected agents to add (already in team/runtime)");
      return;
    }
    const merged = [...existingRuntime, ...additions];
    setRunRuntimeMembersJson(JSON.stringify(merged, null, 2));
    setRunError(null);
  };

  const handleSaveRuntimeMembers = async () => {
    if (!teamIdTrimmed) {
      setRuntimeSaveError("select a team first");
      return;
    }
    if (runtimeMembersPreview.error) {
      setRuntimeSaveError(runtimeMembersPreview.error);
      return;
    }
    const items = runtimeMembersPreview.items;
    if (!Array.isArray(items) || items.length === 0) {
      setRuntimeSaveError("no runtime members to save");
      return;
    }
    const payloads: Record<string, any>[] = [];
    const invalid: string[] = [];
    for (const row of runtimeSavePreview.newMembers) {
      const item = row?.item ?? {};
      const memberId = item?.member_id ? String(item.member_id).trim() : "";
      const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
      const role = item?.role ? String(item.role).trim() : "";
      if (!agentId || !role) {
        invalid.push(memberId || agentId || "runtime");
        continue;
      }
      const payload: Record<string, any> = { role, agent_id: agentId };
      if (memberId) payload.member_id = memberId;
      if (item?.deployment_id) payload.deployment_id = String(item.deployment_id);
      if (Array.isArray(item?.capabilities)) payload.capabilities = item.capabilities;
      if (item?.status) payload.status = String(item.status);
      if (typeof item?.weight === "number") payload.weight = item.weight;
      if (item?.meta && typeof item.meta === "object") payload.meta = item.meta;
      payloads.push(payload);
    }
    if (payloads.length === 0) {
      setRuntimeSaveError(invalid.length > 0 ? "runtime members missing agent_id or role" : "no new members to save");
      return;
    }
    if (!window.confirm(`Save ${payloads.length} runtime member(s) to team?`)) {
      return;
    }
    setRuntimeSaveError(null);
    setRuntimeSaveBusy(true);
    try {
      for (const payload of payloads) {
        await apiBrokerTeamMembersUpsert(props.base, teamIdTrimmed, payload, props.auth);
      }
      if (props.onMembersRefresh) {
        await props.onMembersRefresh(teamIdTrimmed);
      }
    } catch (err) {
      setRuntimeSaveError(String(err));
    } finally {
      setRuntimeSaveBusy(false);
    }
  };

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
    if (!props.canQuery || !teamIdTrimmed) return;
    if (approvalsBusy || approvals !== null) return;
    if (lastAutoApprovalRunIdRef.current === approvalRunIdTrimmed) return;
    lastAutoApprovalRunIdRef.current = approvalRunIdTrimmed;
    void handleApprovalsRefresh();
  }, [approvalRunIdTrimmed, approvals, approvalsBusy, props.canQuery, handleApprovalsRefresh, teamIdTrimmed]);

  React.useEffect(() => {
    if (!props.canQuery) return;
    if (runtimeAgentsBusy || (runtimeAgents && runtimeAgents.length > 0)) return;
    void refreshRuntimeAgents();
  }, [props.canQuery, runtimeAgentsBusy, runtimeAgents, refreshRuntimeAgents]);

  React.useEffect(() => {
    if (!runtimeMemberAgentId) {
      if (runtimeMemberDeploymentId) {
        setRuntimeMemberDeploymentId("");
      }
      return;
    }
    if (runtimeMemberDeploymentId) return;
    if (runtimeAgentDeployments.length === 0) return;
    const first = runtimeAgentDeployments[0];
    const depId = first?.deployment_id ? String(first.deployment_id) : "";
    if (depId) {
      setRuntimeMemberDeploymentId(depId);
    }
  }, [runtimeMemberAgentId, runtimeMemberDeploymentId, runtimeAgentDeployments]);

  return (
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
        <FieldLabel>Mode</FieldLabel>
        <select
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={runMode}
          onChange={(e) => setRunMode(e.target.value)}
        >
          <option value="async">async</option>
          <option value="sync">sync</option>
        </select>
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
          disabled={!props.canQuery || !teamIdTrimmed || runBusy}
          onClick={() => void handleCreateRun()}
        >
          {runBusy ? "Submitting…" : "Create run"}
        </button>
      </div>
      <div className="text-[11px] text-white/50">
        Async mode dispatches `run_async` per member so the run continues if the UI disconnects.
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
        <span className="text-[11px] text-white/50">Apply per-member backend overrides when enabled.</span>
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
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => handleSeedExplicitOverrides()}
            >
              Seed from team members
            </button>
          </div>
          <div className="text-[11px] text-white/50">
            Allowed fields: model, base_url, summary_model, tools, timeout_ms, max_steps, stream_assistant.
          </div>
        </div>
      ) : null}
      <div className="grid gap-1">
        <FieldLabel>Role overrides JSON (optional)</FieldLabel>
        <textarea
          className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
          value={runRoleOverridesJson}
          onChange={(e) => setRunRoleOverridesJson(e.target.value)}
          placeholder='{"planner":{"model":"gpt-4.1-mini","tools":"basic"},"executor":{"base_url":"https://api.openai.com/v1"}}'
        />
        <div className="text-[11px] text-white/50">
          Role overrides apply before member overrides and use the same allowlist. If empty, broker uses team defaults.
        </div>
        {teamRoleOverrideKeys.length > 0 ? (
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => handleSeedRoleOverrides()}
            >
              Seed from team defaults
            </button>
            <span className="text-[11px] text-white/50">Defaults: {teamRoleOverrideKeys.join(", ")}</span>
          </div>
        ) : (
          <div className="text-[11px] text-white/40">No role override defaults saved on this team.</div>
        )}
      </div>
      <div className="grid gap-1">
        <FieldLabel>Runtime members JSON (optional)</FieldLabel>
        <textarea
          className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90"
          data-testid="team-run-runtime-json"
          value={runRuntimeMembersJson}
          onChange={(e) => setRunRuntimeMembersJson(e.target.value)}
          placeholder='[{"member_id":"rt-1","agent_id":"agent_a","role":"executor","capabilities":["vision"],"meta":{"backend_label":"openai-mini"}}]'
        />
        <div className="text-[11px] text-white/50">
          Each entry needs agent_id + role; member_id is optional (required for explicit overrides).
        </div>
        <div className="text-[11px] text-white/50">
          Runtime members are per-run and let an orchestrator add/pause agents dynamically; use "Save to team" to persist.
        </div>
        {runtimeMembersPreview.error ? (
          <div className="text-[11px] text-rose-200">{runtimeMembersPreview.error}</div>
        ) : null}
        {runtimeMembersPreview.items.length > 0 ? (
          <div className="grid gap-2">
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              <span>Bulk status:</span>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleSetAllRuntimeStatus("paused")}
              >
                Pause all
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleSetAllRuntimeStatus("active")}
              >
                Resume all
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleRemovePausedRuntimeMembers()}
              >
                Remove paused
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleCompactRuntimeMembers()}
              >
                Compact JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => void handleCopyRuntimeMembers()}
              >
                Copy JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => runtimeImportRef.current?.click()}
              >
                Import JSON
              </button>
              <label className="flex items-center gap-2 text-[11px] text-white/60">
                <input
                  type="checkbox"
                  checked={runtimeImportMerge}
                  onChange={(e) => setRuntimeImportMerge(e.target.checked)}
                />
                merge
              </label>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleDownloadRuntimeMembers()}
              >
                Download JSON
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => handleExportTeamMembers()}
              >
                Export team
              </button>
              <input
                ref={runtimeImportRef}
                className="hidden"
                type="file"
                accept="application/json,.json"
                onChange={handleImportRuntimeMembers}
              />
            </div>
            {runtimeMembersPreview.items.map((item, idx) => {
              const memberId = item?.member_id ? String(item.member_id) : "";
              const agentId = item?.agent_id ? String(item.agent_id) : "";
              const role = item?.role ? String(item.role) : "";
              const label = memberId ? `${memberId}` : agentId ? `agent ${agentId}` : "runtime member";
              const status = item?.status ? String(item.status).toLowerCase() : "active";
              return (
                <div
                  key={`runtime-preview-${label}-${idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                >
                  <div>
                    <span className="text-white/90">{label}</span>
                    {agentId && memberId ? ` · agent ${agentId}` : ""}
                    {role ? ` · role ${role}` : ""}
                    {status && status !== "active" ? ` · ${status}` : ""}
                  </div>
                  <div className="flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => handleToggleRuntimeMemberStatus(idx)}
                    >
                      {status === "paused" ? "Resume" : "Pause"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => handleRemoveRuntimeMember(idx)}
                    >
                      Remove
                    </button>
                  </div>
                </div>
              );
            })}
            {runtimeMembersPreview.items.length > 0 ? (
              <div className="mt-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
                <div>
                  team diff · runtime-only {runtimeTeamDiff.runtimeOnly.length} · team-only {runtimeTeamDiff.teamOnly.length} · mismatched {runtimeTeamDiff.mismatched.length}
                </div>
                {runtimeTeamDiff.runtimeOnly.length > 0 ? (
                  <div>
                    runtime-only:
                    {runtimeTeamDiff.runtimeOnly.map((item, idx) => {
                      const mid = item?.member_id ? String(item.member_id) : "";
                      const aid = item?.agent_id ? String(item.agent_id) : "";
                      const label = mid ? mid : aid ? `agent ${aid}` : `runtime-${idx + 1}`;
                      return <div key={`runtime-only-${label}-${idx}`}>{label}</div>;
                    })}
                  </div>
                ) : null}
                {runtimeTeamDiff.teamOnly.length > 0 ? (
                  <div>
                    team-only:
                    {runtimeTeamDiff.teamOnly.map((item, idx) => {
                      const mid = item?.member_id ? String(item.member_id) : "";
                      const aid = item?.agent_id ? String(item.agent_id) : "";
                      const label = mid ? mid : aid ? `agent ${aid}` : `team-${idx + 1}`;
                      return <div key={`team-only-${label}-${idx}`}>{label}</div>;
                    })}
                  </div>
                ) : null}
                {runtimeTeamDiff.mismatched.length > 0 ? (
                  <div>
                    mismatched:
                    {runtimeTeamDiff.mismatched.map((row: any, idx: number) => {
                      const item = row?.item ?? {};
                      const mid = item?.member_id ? String(item.member_id) : "";
                      const aid = item?.agent_id ? String(item.agent_id) : "";
                      const label = mid ? mid : aid ? `agent ${aid}` : `runtime-${idx + 1}`;
                      return (
                        <div key={`runtime-mismatch-${label}-${idx}`}>
                          {label} · {Array.isArray(row?.diffs) ? row.diffs.join(", ") : "diff"}
                        </div>
                      );
                    })}
                  </div>
                ) : null}
              </div>
            ) : null}
          </div>
        ) : null}
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
            <FieldLabel>Agent pick</FieldLabel>
            <select
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runtimeMemberAgentId}
              onChange={(e) => setRuntimeMemberAgentId(e.target.value)}
            >
              <option value="">(select agent)</option>
              {runtimeAgentOptions.map((agent) => {
                const id = String(agent?.agent_id || "");
                const connected = agent?.connected === true;
                const depCount = Array.isArray(agent?.deployments) ? agent.deployments.length : 0;
                const label = id
                  ? `${id}${connected ? " · connected" : ""}${depCount ? ` · ${depCount} dep` : ""}`
                  : "agent";
                return (
                  <option key={`runtime-agent-${id}`} value={id}>
                    {label}
                  </option>
                );
              })}
            </select>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || runtimeAgentsBusy}
              onClick={() => void refreshRuntimeAgents()}
            >
              {runtimeAgentsBusy ? "Loading…" : "Refresh agents"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || runtimeAgentsBusy}
              onClick={() => handleAddConnectedAgents()}
            >
              Add connected agents
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || runtimeSaveBusy || runtimeMembersPreview.items.length === 0}
              onClick={() => void handleSaveRuntimeMembers()}
            >
              {runtimeSaveBusy ? "Saving…" : "Save to team"}
            </button>
            {runtimeAgentsError ? (
              <span className="text-[11px] text-rose-200">{runtimeAgentsError}</span>
            ) : null}
          </div>
          {runtimeSaveError ? (
            <div className="text-[11px] text-rose-200">{runtimeSaveError}</div>
          ) : null}
          {runtimeMembersPreview.items.length > 0 ? (
            <div className="grid gap-1 text-[11px] text-white/50">
              <div>
                save preview: {runtimeSavePreview.newMembers.length} new · {runtimeSavePreview.skipped.length} skipped · {runtimeSavePreview.invalid.length} invalid
              </div>
              {runtimeSavePreview.invalid.length > 0 ? (
                <div>
                  invalid:
                  <button
                    className="ml-2 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() => handleFixInvalidRuntimeMembers()}
                  >
                    Fix invalid
                  </button>
                  {runtimeSavePreview.invalid.map((row, idx) => {
                    const item = row?.item ?? {};
                    const label = item?.member_id
                      ? String(item.member_id)
                      : item?.agent_id
                      ? `agent ${String(item.agent_id)}`
                      : `runtime-${idx + 1}`;
                    return (
                      <div key={`runtime-invalid-${label}-${idx}`}>
                        {label} · {row?.reason || "invalid"}
                      </div>
                    );
                  })}
                </div>
              ) : null}
              {runtimeSavePreview.skipped.length > 0 ? (
                <div>
                  skipped:
                  {runtimeSavePreview.skipped.map((row, idx) => {
                    const item = row?.item ?? {};
                    const label = item?.member_id
                      ? String(item.member_id)
                      : item?.agent_id
                      ? `agent ${String(item.agent_id)}`
                      : `runtime-${idx + 1}`;
                    return (
                      <div key={`runtime-skipped-${label}-${idx}`}>
                        {label} · {row?.reason || "skipped"}
                      </div>
                    );
                  })}
                </div>
              ) : null}
            </div>
          ) : null}
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Deployment</FieldLabel>
            <input
              className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runtimeMemberDeploymentId}
              onChange={(e) => setRuntimeMemberDeploymentId(e.target.value)}
              placeholder="default"
            />
            <FieldLabel>Deployment pick</FieldLabel>
            <select
              className="min-w-[180px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={runtimeMemberDeploymentId}
              onChange={(e) => setRuntimeMemberDeploymentId(e.target.value)}
              disabled={runtimeAgentDeployments.length === 0}
            >
              <option value="">default</option>
              {runtimeAgentDeployments.map((dep, idx) => {
                const depId = String(dep?.deployment_id || "");
                const connected = dep?.connected === true;
                const label = depId ? `${depId}${connected ? " · connected" : ""}` : `deployment-${idx + 1}`;
                return (
                  <option key={`runtime-dep-${depId || idx}`} value={depId}>
                    {label}
                  </option>
                );
              })}
            </select>
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
      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2" data-testid="team-inline-approvals">
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
            disabled={!props.canQuery || !teamIdTrimmed || runBusy}
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
                  onClick={() => setRunApprovals((prev) => prev.filter((_, i) => i !== idx))}
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
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{runError}</div>
      ) : null}
      {runQuorum?.rules && runQuorum.rules.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          {runQuorum.rules.map((r, idx) => (
            <div key={`quorum-eval-${r.rule_id || idx}`}>
              {r.rule_id ? `rule ${r.rule_id}` : "rule"} · min {r.min_approvals ?? "?"} · approved {r.approved ?? 0} · missing {r.missing ?? 0}
            </div>
          ))}
        </div>
      ) : null}
      {runResult ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          run id {String(runResult?.team_run_id || "unknown")} · status {String(runResult?.status || "")}
          {runResult?.mode ? ` · mode ${String(runResult.mode)}` : ""}
        </div>
      ) : null}

      <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="text-[11px] text-white/70">Recent team runs</div>
          <div className="flex flex-wrap items-center gap-2">
            <FieldLabel>Limit</FieldLabel>
            <input
              className="w-16 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={String(recentRunsLimit)}
              onChange={(e) => {
                const next = Number.parseInt(e.target.value, 10);
                if (Number.isFinite(next) && next > 0) {
                  setRecentRunsLimit(next);
                } else if (!e.target.value.trim()) {
                  setRecentRunsLimit(10);
                }
              }}
              placeholder="10"
            />
            <FieldLabel>Status</FieldLabel>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
              value={recentRunsStatus}
              onChange={(e) => setRecentRunsStatus(e.target.value)}
              placeholder="optional"
            />
            <label className="flex items-center gap-2 text-[10px] text-white/60">
              <input
                type="checkbox"
                className="rounded border-white/20 bg-black/40"
                checked={recentRunsLive}
                onChange={(e) => setRecentRunsLive(e.target.checked)}
              />
              live (SSE)
            </label>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || !teamIdTrimmed || recentRunsBusy}
              onClick={() => void loadRecentRuns()}
            >
              {recentRunsBusy ? "Loading…" : "Refresh"}
            </button>
          </div>
        </div>
        {recentRunsError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {recentRunsError}
          </div>
        ) : null}
        {recentRunsItems.length > 0 ? (
          <div className="grid gap-1 text-[11px] text-white/70">
            {recentRunsItems.map((row: any, idx: number) => {
              const runId = String(row?.team_run_id || "");
              return (
                <div
                  key={`team-run-row-${runId || idx}`}
                  className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1"
                >
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="text-white/90">{runId || "?"}</span>
                    {row?.status ? <span>· {row.status}</span> : null}
                    {row?.mode ? <span>· {row.mode}</span> : null}
                    {typeof row?.created_unix_ms === "number" ? <span>· {fmtTs(row.created_unix_ms)}</span> : null}
                    {fmtSummary(row?.member_job_summary) ? <span>· {fmtSummary(row.member_job_summary)}</span> : null}
                  </div>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => {
                      if (!runId) return;
                      setRunLookupId(runId);
                      void fetchRunStatus(runId);
                    }}
                  >
                    Load
                  </button>
                </div>
              );
            })}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No recent runs.</div>
        )}
      </div>

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
          disabled={!props.canQuery || !teamIdTrimmed || runLookupBusy}
          onClick={() => void handleRunLookup()}
        >
          {runLookupBusy ? "Loading…" : "Get status"}
        </button>
        <button
          className="rounded-md border border-amber-400/40 bg-amber-500/10 px-3 py-1 text-[11px] text-amber-100 hover:bg-amber-500/20 disabled:opacity-50"
          type="button"
          disabled={!props.canQuery || runCancelBusy || !resolveRunId()}
          onClick={() => void handleRunCancel()}
        >
          {runCancelBusy ? "Cancelling…" : "Cancel run"}
        </button>
      </div>
      <label className="flex items-center gap-2 text-[11px] text-white/60">
        <input
          type="checkbox"
          className="rounded border-white/20 bg-black/40"
          checked={autoRefreshRunLookup}
          onChange={(e) => setAutoRefreshRunLookup(e.target.checked)}
        />
        Auto refresh run status on team run events
      </label>
      {runLookupError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{runLookupError}</div>
      ) : null}
      {runCancelError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{runCancelError}</div>
      ) : null}
      {runCancelNote ? (
        <div className="rounded-md border border-amber-400/20 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">{runCancelNote}</div>
      ) : null}
      {runLookupResult ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          status {String(runLookupResult?.status || "")}
          {runLookupResult?.created_unix_ms ? ` · ${fmtTs(runLookupResult.created_unix_ms)}` : ""}
          {runLookupResult?.mode ? ` · mode ${String(runLookupResult.mode)}` : ""}
          {typeof runLookupResult?.cancel_requested_unix_ms === "number"
            ? ` · cancel requested ${fmtTs(runLookupResult.cancel_requested_unix_ms)}`
            : ""}
        </div>
      ) : null}
      {runLookupResult?.run_overrides_mode ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          overrides mode: {String(runLookupResult.run_overrides_mode)}
        </div>
      ) : null}
      {runLookupResult?.role_overrides_applied || runLookupResult?.member_overrides_applied ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <span>applied overrides</span>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => setRunOverridesExpanded((prev) => !prev)}
            >
              {runOverridesExpanded ? "Hide" : "Show"}
            </button>
          </div>
          {runOverridesExpanded ? (
            <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/60">
              {JSON.stringify(
                {
                  role_overrides_applied: runLookupResult?.role_overrides_applied ?? null,
                  member_overrides_applied: runLookupResult?.member_overrides_applied ?? null,
                },
                null,
                2,
              )}
            </pre>
          ) : null}
        </div>
      ) : null}
      {fmtSummary(runLookupResult?.member_job_summary) ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          summary: {fmtSummary(runLookupResult?.member_job_summary)}
        </div>
      ) : null}
      {Array.isArray(runLookupResult?.dispatch_errors) && runLookupResult.dispatch_errors.length > 0 ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-200">
          dispatch errors:
          {runLookupResult.dispatch_errors.map((err: any, idx: number) => {
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
      {Array.isArray(runLookupResult?.cancel_results) && runLookupResult.cancel_results.length > 0 ? (
        <div className="rounded-md border border-amber-400/20 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          cancel results:
          {runLookupResult.cancel_results.map((row: any, idx: number) => {
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
      {Array.isArray(runLookupResult?.member_jobs) && runLookupResult.member_jobs.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          member jobs:
          {runLookupResult.member_jobs.map((job: any, idx: number) => {
            const mid = job?.member_id ? String(job.member_id) : "";
            const aid = job?.agent_id ? String(job.agent_id) : "";
            const jobId = job?.job_id ? String(job.job_id) : "";
            const status = job?.status ? String(job.status) : "";
            const ok = typeof job?.ok === "boolean" ? String(job.ok) : "";
            const err = job?.error ? String(job.error) : job?.dispatch_error ? String(job.dispatch_error) : "";
            const updated = typeof job?.updated_unix_ms === "number" ? fmtTs(job.updated_unix_ms) : "";
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
      {runMemberSessions.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          member sessions:
          {runMemberSessions.map((row, idx) => (
            <div key={`member-session-${row.memberId}-${idx}`} className="mt-1 text-[10px] text-white/60">
              {row.memberId} · {row.sessionId}
            </div>
          ))}
        </div>
      ) : null}
      {Array.isArray(runLookupResult?.runtime_members) && runLookupResult.runtime_members.length > 0 ? (
        <div className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70">
          runtime members:
          {runLookupResult.runtime_members.map((m: any, idx: number) => {
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
                    disabled={!props.canQuery || runtimeUpdateBusy}
                    onClick={() => void handleRuntimeMemberToggle(m)}
                  >
                    {String(status || "active").toLowerCase() === "paused" ? "Resume" : "Pause"}
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    data-testid={`team-run-runtime-remove-${runtimeKey}`}
                    disabled={!props.canQuery || runtimeUpdateBusy}
                    onClick={() => void handleRuntimeMemberRemove(m)}
                  >
                    Remove
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      ) : null}
      <TeamRunModeratorPanel
        canQuery={props.canQuery}
        runId={resolveRunId()}
        memberSessions={runMemberSessions}
        roleOptions={runRoleOptions}
        memberOptions={runMemberOptions}
        agentOptions={runAgentOptions}
        directive={moderatorDirective}
        directiveScope={moderatorDirectiveScope}
        taskTitle={moderatorTaskTitle}
        taskDetail={moderatorTaskDetail}
        taskStatus={moderatorTaskStatus}
        targetRoles={moderatorTargetRoles}
        targetMembers={moderatorTargetMembers}
        targetAgents={moderatorTargetAgents}
        assignees={moderatorAssignees}
        appendToSession={moderatorAppendToSession}
        busy={moderatorBusy}
        error={moderatorError}
        success={moderatorSuccess}
        events={moderatorEvents}
        eventsBusy={moderatorEventsBusy}
        eventsError={moderatorEventsError}
        eventsTypes={moderatorEventsTypes}
        eventsMaxBytes={moderatorEventsMaxBytes}
        eventsLimit={moderatorEventsLimit}
        eventsExpanded={moderatorEventsExpanded}
        onDirectiveChange={setModeratorDirective}
        onDirectiveScopeChange={setModeratorDirectiveScope}
        onTaskTitleChange={setModeratorTaskTitle}
        onTaskDetailChange={setModeratorTaskDetail}
        onTaskStatusChange={setModeratorTaskStatus}
        onTargetRolesChange={setModeratorTargetRoles}
        onTargetMembersChange={setModeratorTargetMembers}
        onTargetAgentsChange={setModeratorTargetAgents}
        onAssigneesChange={setModeratorAssignees}
        onAppendToSessionChange={setModeratorAppendToSession}
        onPublishDirective={() => void handleModeratorDirectivePublish()}
        onPublishTask={() => void handleModeratorTaskPublish()}
        onEventsTypesChange={setModeratorEventsTypes}
        onEventsMaxBytesChange={setModeratorEventsMaxBytes}
        onEventsLimitChange={setModeratorEventsLimit}
        onEventsLoad={() => void handleModeratorEventsLoad()}
        onEventsToggleExpanded={() => setModeratorEventsExpanded((prev) => !prev)}
      />
      <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Runtime member updates</div>
        <div className="text-[11px] text-white/50">
          Apply the runtime members JSON (above) to an existing run; merge preserves existing members by member_id.
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Mode</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            data-testid="team-run-runtime-mode"
            value={runtimeUpdateMode}
            onChange={(e) => setRuntimeUpdateMode(e.target.value)}
          >
            <option value="replace">replace</option>
            <option value="merge">merge</option>
          </select>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            data-testid="team-run-runtime-load"
            disabled={!props.canQuery || runtimeUpdateBusy}
            onClick={() => handleRuntimeMembersLoadFromRun()}
          >
            Load from run
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            data-testid="team-run-runtime-update"
            disabled={!props.canQuery || runtimeUpdateBusy}
            onClick={() => void handleRuntimeMembersUpdate()}
          >
            {runtimeUpdateBusy ? "Updating…" : "Update runtime members"}
          </button>
        </div>
        {runtimeUpdateError ? (
          <div className="text-[11px] text-rose-200">{runtimeUpdateError}</div>
        ) : null}
        {runtimeUpdateNote ? <div className="text-[11px] text-white/60">{runtimeUpdateNote}</div> : null}
      </div>

      <div className="mt-3 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
        <div className="text-xs font-semibold text-white/80">Run approvals</div>
        <div className="text-[11px] text-white/50">Submit or review approvals for a team run (quorum rules apply).</div>
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
                      if (teamId && props.onTeamSelect) props.onTeamSelect(teamId);
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
            disabled={!props.canQuery || !teamIdTrimmed || approvalsBusy}
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
            disabled={!props.canQuery || !teamIdTrimmed || approvalsBusy}
            onClick={() => void handleApprovalSubmit()}
          >
            {approvalsBusy ? "Submitting…" : "Submit approval"}
          </button>
        </div>
        {approvalsError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">{approvalsError}</div>
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
  );
}
