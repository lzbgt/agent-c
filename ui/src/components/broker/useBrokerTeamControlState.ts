import React from "react";
import {
  apiBrokerTeamGet,
  apiBrokerTeamMembersDelete,
  apiBrokerTeamMembersList,
  apiBrokerTeamMembersUpsert,
  apiBrokerTeamMemberUpdate,
  apiBrokerTeamQuorumDelete,
  apiBrokerTeamQuorumList,
  apiBrokerTeamQuorumUpsert,
  apiBrokerTeamUpdate,
  type ApiAuth,
  type BrokerAgentInfo,
  type BrokerDeploymentInfo,
  type BrokerTeamMemberUpsertRequest,
  type BrokerTeamMemberUpdateRequest,
  type BrokerTeamQuorumRuleUpsertRequest,
  type BrokerTeamUpdateRequest,
} from "../../api";
import {
  normalizeRoleGraphEdges,
  normalizeRoleInstructionMap,
  normalizeRolePromptMode,
  normalizeSharedMemoryMode,
  type RoleGraphEdge,
} from "./teamRunUtils";
import { asStringList, asUnknownRecord, parseUnknownRecordJson, type UnknownRecord } from "./brokerObjectUtils";
import type { TeamMemberRow, TeamQuorumRuleRow } from "./types";
import type { TeamRow } from "./teamConsoleTypes";

type UseBrokerTeamControlStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  memberAgents: BrokerAgentInfo[] | null;
};

export default function useBrokerTeamControlState(args: UseBrokerTeamControlStateArgs) {
  const { base, auth, canQuery, teamIdTrimmed, memberAgents } = args;
  const [teamDetails, setTeamDetails] = React.useState<TeamRow | null>(null);
  const [teamEditName, setTeamEditName] = React.useState<string>("");
  const [teamEditTags, setTeamEditTags] = React.useState<string>("");
  const [teamEditPolicyRef, setTeamEditPolicyRef] = React.useState<string>("");
  const [teamEditSharedScope, setTeamEditSharedScope] = React.useState<string>("");
  const [teamEditSharedMode, setTeamEditSharedMode] = React.useState<string>("read_write");
  const [teamEditMetaJson, setTeamEditMetaJson] = React.useState<string>("");
  const [teamEditRoleOverridesJson, setTeamEditRoleOverridesJson] = React.useState<string>("");
  const [teamRoleInstructions, setTeamRoleInstructions] = React.useState<Record<string, string>>({});
  const [teamRolePromptMode, setTeamRolePromptMode] = React.useState<string>("prepend");
  const [teamRoleGraphEdges, setTeamRoleGraphEdges] = React.useState<RoleGraphEdge[]>([]);
  const [teamRolePlanTouched, setTeamRolePlanTouched] = React.useState<boolean>(false);
  const [teamEditBusy, setTeamEditBusy] = React.useState<boolean>(false);
  const [teamEditError, setTeamEditError] = React.useState<string | null>(null);

  const [membersBusy, setMembersBusy] = React.useState<boolean>(false);
  const [membersError, setMembersError] = React.useState<string | null>(null);
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

  const [memberEditId, setMemberEditId] = React.useState<string>("");
  const [memberEditRole, setMemberEditRole] = React.useState<string>("");
  const [memberEditStatus, setMemberEditStatus] = React.useState<string>("");
  const [memberEditWeight, setMemberEditWeight] = React.useState<string>("");
  const [memberEditCapabilities, setMemberEditCapabilities] = React.useState<string>("");
  const [memberEditMetaJson, setMemberEditMetaJson] = React.useState<string>("");
  const [memberEditBackendLabel, setMemberEditBackendLabel] = React.useState<string>("");
  const [memberEditModel, setMemberEditModel] = React.useState<string>("");
  const [memberEditBaseUrl, setMemberEditBaseUrl] = React.useState<string>("");
  const [memberEditSummaryModel, setMemberEditSummaryModel] = React.useState<string>("");
  const [memberEditTools, setMemberEditTools] = React.useState<string>("");
  const [memberEditTimeoutMs, setMemberEditTimeoutMs] = React.useState<string>("");
  const [memberEditAgentId, setMemberEditAgentId] = React.useState<string>("");
  const [memberEditDeploymentId, setMemberEditDeploymentId] = React.useState<string>("");
  const [memberEditBusy, setMemberEditBusy] = React.useState<boolean>(false);
  const [memberEditError, setMemberEditError] = React.useState<string | null>(null);

  const [rulesBusy, setRulesBusy] = React.useState<boolean>(false);
  const [rulesError, setRulesError] = React.useState<string | null>(null);
  const [rules, setRules] = React.useState<TeamQuorumRuleRow[] | null>(null);
  const [ruleAction, setRuleAction] = React.useState<string>("team_run");
  const [ruleMinApprovals, setRuleMinApprovals] = React.useState<string>("1");
  const [ruleMode, setRuleMode] = React.useState<string>("strict");

  const membersList = Array.isArray(members) ? members : [];
  const rolePlanOptions = React.useMemo(() => {
    const set = new Set<string>();
    for (const member of membersList) {
      const role = String(member?.role || "").trim().toLowerCase();
      if (role) set.add(role);
    }
    try {
      const parsed = JSON.parse(String(teamEditRoleOverridesJson || "").trim() || "{}");
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        for (const key of Object.keys(parsed)) {
          const role = String(key || "").trim().toLowerCase();
          if (role) set.add(role);
        }
      }
    } catch {
      // ignored; surfaced elsewhere
    }
    for (const role of Object.keys(teamRoleInstructions)) {
      const normalized = String(role || "").trim().toLowerCase();
      if (normalized) set.add(normalized);
    }
    for (const edge of teamRoleGraphEdges) {
      const from = String(edge?.from_role || "").trim().toLowerCase();
      const to = String(edge?.to_role || "").trim().toLowerCase();
      if (from) set.add(from);
      if (to) set.add(to);
    }
    return Array.from(set).filter(Boolean).sort();
  }, [membersList, teamEditRoleOverridesJson, teamRoleGraphEdges, teamRoleInstructions]);

  const rulesList = Array.isArray(rules) ? rules : [];
  const memberAgentOptions = Array.isArray(memberAgents) ? memberAgents : [];
  const memberSelectedAgent = memberAgentOptions.find(
    (agent) => String(agent?.agent_id || "") === String(memberAgentId || "").trim(),
  );
  const memberAgentDeployments = Array.isArray(memberSelectedAgent?.deployments)
    ? (memberSelectedAgent.deployments as BrokerDeploymentInfo[])
    : [];
  const memberEditSelectedAgent = memberAgentOptions.find(
    (agent) => String(agent?.agent_id || "") === String(memberEditAgentId || "").trim(),
  );
  const memberEditAgentDeployments = Array.isArray(memberEditSelectedAgent?.deployments)
    ? (memberEditSelectedAgent.deployments as BrokerDeploymentInfo[])
    : [];

  const refreshTeamDetails = React.useCallback(async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    try {
      const resp = await apiBrokerTeamGet(base, tid, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "team get failed");
      setTeamDetails(resp?.team ?? null);
    } catch {
      setTeamDetails(null);
    }
  }, [auth, base, canQuery]);

  const refreshMembers = React.useCallback(async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      const resp = await apiBrokerTeamMembersList(base, tid, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "member list failed");
      setMembers(Array.isArray(resp?.members) ? resp.members : []);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [auth, base, canQuery]);

  const refreshRules = React.useCallback(async (id: string) => {
    const tid = String(id || "").trim();
    if (!canQuery || !tid) return;
    setRulesError(null);
    setRulesBusy(true);
    try {
      const resp = await apiBrokerTeamQuorumList(base, tid, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "quorum list failed");
      setRules(Array.isArray(resp?.rules) ? resp.rules : []);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  }, [auth, base, canQuery]);

  const loadTeamEditsFromDetails = React.useCallback((details: TeamRow | null) => {
    if (!details) {
      setTeamEditName("");
      setTeamEditTags("");
      setTeamEditPolicyRef("");
      setTeamEditSharedScope("");
      setTeamEditSharedMode("read_write");
      setTeamEditMetaJson("");
      setTeamEditRoleOverridesJson("");
      setTeamRoleInstructions({});
      setTeamRolePromptMode("prepend");
      setTeamRoleGraphEdges([]);
      setTeamRolePlanTouched(false);
      return;
    }
    setTeamEditName(String(details?.display_name || ""));
    const tags = asStringList(details?.tags);
    setTeamEditTags(tags.join(", "));
    setTeamEditPolicyRef(String(details?.policy_ref || ""));
    setTeamEditSharedScope(String(details?.shared_memory_scope_id || ""));
    if (details?.meta && typeof details.meta === "object") {
      try {
        setTeamEditMetaJson(JSON.stringify(details.meta, null, 2));
      } catch {
        setTeamEditMetaJson("");
      }
    } else {
      setTeamEditMetaJson("");
    }
    const metaObj = asUnknownRecord(details?.meta);
    setTeamEditSharedMode(
      normalizeSharedMemoryMode(metaObj?.shared_memory_mode ?? metaObj?.memory_scope_mode ?? "read_write"),
    );
    const roleOverrides = asUnknownRecord(metaObj?.role_overrides);
    if (roleOverrides) {
      try {
        setTeamEditRoleOverridesJson(JSON.stringify(roleOverrides, null, 2));
      } catch {
        setTeamEditRoleOverridesJson("");
      }
    } else {
      setTeamEditRoleOverridesJson("");
    }
    if (metaObj) {
      setTeamRoleInstructions(normalizeRoleInstructionMap(metaObj.role_instructions));
      setTeamRolePromptMode(normalizeRolePromptMode(metaObj.role_prompt_mode));
      setTeamRoleGraphEdges(normalizeRoleGraphEdges(metaObj.role_graph));
    } else {
      setTeamRoleInstructions({});
      setTeamRolePromptMode("prepend");
      setTeamRoleGraphEdges([]);
    }
    setTeamRolePlanTouched(false);
  }, []);

  const handleRoleInstructionsChange = React.useCallback((next: Record<string, string>) => {
    setTeamRolePlanTouched(true);
    setTeamRoleInstructions(next);
  }, []);

  const handleRolePromptModeChange = React.useCallback((next: string) => {
    setTeamRolePlanTouched(true);
    setTeamRolePromptMode(next);
  }, []);

  const handleRoleGraphEdgesChange = React.useCallback((next: RoleGraphEdge[]) => {
    setTeamRolePlanTouched(true);
    setTeamRoleGraphEdges(next);
  }, []);

  const handleUpdateTeam = React.useCallback(async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const displayName = String(teamEditName || "").trim();
    if (!displayName) {
      setTeamEditError("display_name required");
      return;
    }
    const tagsRaw = String(teamEditTags || "").trim();
    const tags =
      tagsRaw.length === 0
        ? []
        : tagsRaw
            .split(",")
            .map((t) => t.trim())
            .filter(Boolean);
    const policyRef = String(teamEditPolicyRef || "").trim();
    const sharedScope = String(teamEditSharedScope || "").trim();
    const sharedModeRaw = String(teamEditSharedMode || "").trim().toLowerCase();
    if (
      sharedModeRaw &&
      sharedModeRaw !== "read_only" &&
      sharedModeRaw !== "read_write" &&
      sharedModeRaw !== "readonly" &&
      sharedModeRaw !== "readwrite"
    ) {
      setTeamEditError("shared memory mode must be read_only or read_write");
      return;
    }
    const sharedMode = normalizeSharedMemoryMode(sharedModeRaw);
    const metaRaw = String(teamEditMetaJson || "").trim();
    let meta: UnknownRecord = {};
    if (metaRaw) {
      try {
        meta = parseUnknownRecordJson(metaRaw, "meta");
      } catch (err) {
        setTeamEditError(`invalid meta json: ${String(err)}`);
        return;
      }
    }
    const roleOverridesRaw = String(teamEditRoleOverridesJson || "").trim();
    if (roleOverridesRaw) {
      try {
        meta.role_overrides = parseUnknownRecordJson(roleOverridesRaw, "role overrides");
      } catch (err) {
        setTeamEditError(`invalid role overrides json: ${String(err)}`);
        return;
      }
    }
    if (teamRolePlanTouched) {
      if (Object.keys(teamRoleInstructions).length > 0) {
        meta.role_instructions = teamRoleInstructions;
        meta.role_prompt_mode = normalizeRolePromptMode(teamRolePromptMode);
      } else {
        delete meta.role_instructions;
        delete meta.role_prompt_mode;
      }
      if (teamRoleGraphEdges.length > 0) {
        meta.role_graph = { edges: teamRoleGraphEdges };
      } else {
        delete meta.role_graph;
      }
    }
    if (sharedScope) meta.shared_memory_mode = sharedMode || "read_write";
    else delete meta.shared_memory_mode;
    setTeamEditError(null);
    setTeamEditBusy(true);
    try {
      const body: BrokerTeamUpdateRequest = {
        display_name: displayName,
        tags,
        policy_ref: policyRef,
        shared_memory_scope_id: sharedScope,
        meta,
      };
      const resp = await apiBrokerTeamUpdate(
        base,
        tid,
        body,
        auth,
      );
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "update team failed");
      setTeamDetails(resp?.team ?? null);
      loadTeamEditsFromDetails(resp?.team ?? null);
    } catch (err) {
      setTeamEditError(String(err));
    } finally {
      setTeamEditBusy(false);
    }
  }, [
    auth,
    base,
    loadTeamEditsFromDetails,
    teamEditMetaJson,
    teamEditName,
    teamEditPolicyRef,
    teamEditRoleOverridesJson,
    teamEditSharedMode,
    teamEditSharedScope,
    teamEditTags,
    teamIdTrimmed,
    teamRoleGraphEdges,
    teamRoleInstructions,
    teamRolePlanTouched,
    teamRolePromptMode,
  ]);

  const handleAddMember = React.useCallback(async () => {
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
      const payload: BrokerTeamMemberUpsertRequest = { role };
      const mid = String(memberId || "").trim();
      if (mid) payload.member_id = mid;
      const aid = String(memberAgentId || "").trim();
      if (aid) payload.agent_id = aid;
      const dep = String(memberDeploymentId || "").trim();
      if (dep) payload.deployment_id = dep;
      const status = String(memberStatus || "").trim();
      if (status) payload.status = status;
      const weightRaw = String(memberWeight || "").trim();
      if (weightRaw) {
        const weight = Number.parseInt(weightRaw, 10);
        if (!Number.isFinite(weight)) {
          setMembersError("weight must be a number");
          return;
        }
        payload.weight = weight;
      }
      const caps = String(memberCapabilities || "")
        .split(",")
        .map((c) => c.trim())
        .filter(Boolean);
      if (caps.length > 0) payload.capabilities = caps;
      const meta: UnknownRecord = {};
      const backendLabel = String(memberBackendLabel || "").trim();
      if (backendLabel) meta.backend_label = backendLabel;
      const runOverrides: UnknownRecord = {};
      const model = String(memberModel || "").trim();
      if (model) runOverrides.model = model;
      const baseUrl = String(memberBaseUrl || "").trim();
      if (baseUrl) runOverrides.base_url = baseUrl;
      const summaryModel = String(memberSummaryModel || "").trim();
      if (summaryModel) runOverrides.summary_model = summaryModel;
      const tools = String(memberTools || "").trim();
      if (tools) runOverrides.tools = tools;
      const timeoutMsRaw = String(memberTimeoutMs || "").trim();
      if (timeoutMsRaw) {
        const timeoutMs = Number.parseInt(timeoutMsRaw, 10);
        if (!Number.isFinite(timeoutMs)) {
          setMembersError("timeout_ms must be a number");
          return;
        }
        runOverrides.timeout_ms = timeoutMs;
      }
      if (Object.keys(runOverrides).length > 0) meta.run_overrides = runOverrides;
      if (Object.keys(meta).length > 0) payload.meta = meta;
      const resp = await apiBrokerTeamMembersUpsert(base, tid, payload, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "member create failed");
      setMemberId("");
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [
    auth,
    base,
    memberAgentId,
    memberBackendLabel,
    memberBaseUrl,
    memberCapabilities,
    memberDeploymentId,
    memberId,
    memberModel,
    memberRole,
    memberStatus,
    memberSummaryModel,
    memberTimeoutMs,
    memberTools,
    memberWeight,
    refreshMembers,
    teamIdTrimmed,
  ]);

  const handleToggleMemberStatus = React.useCallback(async (member: TeamMemberRow) => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const mid = String(member?.member_id || "").trim();
    if (!mid) return;
    const current = String(member?.status || "active").toLowerCase();
    const next = current === "paused" ? "active" : "paused";
    setMembersError(null);
    setMembersBusy(true);
    try {
      const resp = await apiBrokerTeamMemberUpdate(base, tid, mid, { status: next }, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "update member failed");
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [auth, base, refreshMembers, teamIdTrimmed]);

  const handleSetAllMemberStatus = React.useCallback(async (status: "active" | "paused") => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    if (membersList.length === 0) {
      setMembersError("no team members loaded");
      return;
    }
    if (!window.confirm(`Set ${membersList.length} member(s) to ${status}?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      for (const member of membersList) {
        const mid = String(member?.member_id || "").trim();
        if (!mid) continue;
        const current = String(member?.status || "active").toLowerCase();
        if (current === status) continue;
        const resp = await apiBrokerTeamMemberUpdate(base, tid, mid, { status }, auth);
        if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "update member failed");
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [auth, base, membersList, refreshMembers, teamIdTrimmed]);

  const handleRemovePausedMembers = React.useCallback(async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    const paused = membersList.filter((member) => String(member?.status || "").trim().toLowerCase() === "paused");
    if (paused.length === 0) {
      setMembersError("no paused members to remove");
      return;
    }
    if (!window.confirm(`Remove ${paused.length} paused member(s) from the team?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      for (const member of paused) {
        const mid = String(member?.member_id || "").trim();
        if (!mid) continue;
        await apiBrokerTeamMembersDelete(base, tid, mid, auth);
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [auth, base, membersList, refreshMembers, teamIdTrimmed]);

  const handleEditMember = React.useCallback((member: TeamMemberRow) => {
    const mid = String(member?.member_id || "").trim();
    if (!mid) return;
    setMemberEditId(mid);
    setMemberEditRole(String(member?.role || ""));
    setMemberEditStatus(String(member?.status || "active"));
    setMemberEditAgentId(String(member?.agent_id || ""));
    setMemberEditDeploymentId(String(member?.deployment_id || ""));
    setMemberEditWeight(typeof member?.weight === "number" && Number.isFinite(member.weight) ? String(member.weight) : "");
    const caps = Array.isArray(member?.capabilities) ? member.capabilities.map((c) => String(c).trim()).filter(Boolean) : [];
    setMemberEditCapabilities(caps.join(", "));
    const metaObj = asUnknownRecord(member?.meta);
    if (metaObj) {
      try {
        setMemberEditMetaJson(JSON.stringify(member.meta, null, 2));
      } catch {
        setMemberEditMetaJson("");
      }
    } else {
      setMemberEditMetaJson("");
    }
    setMemberEditBackendLabel(metaObj?.backend_label ? String(metaObj.backend_label) : "");
    const overridesRaw = asUnknownRecord(metaObj?.run_overrides);
    setMemberEditModel(overridesRaw?.model ? String(overridesRaw.model) : "");
    setMemberEditBaseUrl(overridesRaw?.base_url ? String(overridesRaw.base_url) : "");
    setMemberEditSummaryModel(overridesRaw?.summary_model ? String(overridesRaw.summary_model) : "");
    setMemberEditTools(overridesRaw?.tools ? String(overridesRaw.tools) : "");
    const timeoutRaw = overridesRaw?.timeout_ms;
    if (typeof timeoutRaw === "number" && Number.isFinite(timeoutRaw)) setMemberEditTimeoutMs(String(timeoutRaw));
    else if (typeof timeoutRaw === "string" && timeoutRaw.trim().length > 0) setMemberEditTimeoutMs(timeoutRaw.trim());
    else setMemberEditTimeoutMs("");
    setMemberEditError(null);
  }, []);

  const handleCancelMemberEdit = React.useCallback(() => {
    setMemberEditId("");
    setMemberEditRole("");
    setMemberEditStatus("");
    setMemberEditAgentId("");
    setMemberEditDeploymentId("");
    setMemberEditWeight("");
    setMemberEditCapabilities("");
    setMemberEditMetaJson("");
    setMemberEditBackendLabel("");
    setMemberEditModel("");
    setMemberEditBaseUrl("");
    setMemberEditSummaryModel("");
    setMemberEditTools("");
    setMemberEditTimeoutMs("");
    setMemberEditError(null);
  }, []);

  const handleSaveMemberEdit = React.useCallback(async () => {
    const tid = teamIdTrimmed;
    const mid = String(memberEditId || "").trim();
    if (!tid || !mid) return;
    const role = String(memberEditRole || "").trim();
    if (!role) {
      setMemberEditError("role required");
      return;
    }
    const status = String(memberEditStatus || "").trim() || "active";
    const agentId = String(memberEditAgentId || "").trim();
    const deploymentId = String(memberEditDeploymentId || "").trim();
    const caps = String(memberEditCapabilities || "")
      .split(",")
      .map((c) => c.trim())
      .filter(Boolean);
    const weightRaw = String(memberEditWeight || "").trim();
    let weightValue: number | undefined;
    if (weightRaw.length > 0) {
      const parsed = Number.parseInt(weightRaw, 10);
      if (!Number.isFinite(parsed)) {
        setMemberEditError("weight must be a number");
        return;
      }
      weightValue = parsed;
    }
    let meta: UnknownRecord = {};
    let hasMeta = false;
    const metaRaw = String(memberEditMetaJson || "").trim();
    if (metaRaw) {
      try {
        meta = parseUnknownRecordJson(metaRaw, "meta");
        hasMeta = true;
      } catch (err) {
        setMemberEditError(`invalid meta json: ${String(err)}`);
        return;
      }
    }
    const backendLabel = String(memberEditBackendLabel || "").trim();
    if (backendLabel) {
      meta.backend_label = backendLabel;
      hasMeta = true;
    } else if (Object.prototype.hasOwnProperty.call(meta, "backend_label")) {
      delete meta.backend_label;
      hasMeta = true;
    }
    const runOverrides: UnknownRecord = asUnknownRecord(meta.run_overrides)
      ? { ...asUnknownRecord(meta.run_overrides)! }
      : {};
    const model = String(memberEditModel || "").trim();
    if (model) runOverrides.model = model;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "model")) delete runOverrides.model;
    const baseUrl = String(memberEditBaseUrl || "").trim();
    if (baseUrl) runOverrides.base_url = baseUrl;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "base_url")) delete runOverrides.base_url;
    const summaryModel = String(memberEditSummaryModel || "").trim();
    if (summaryModel) runOverrides.summary_model = summaryModel;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "summary_model")) delete runOverrides.summary_model;
    const tools = String(memberEditTools || "").trim();
    if (tools) runOverrides.tools = tools;
    else if (Object.prototype.hasOwnProperty.call(runOverrides, "tools")) delete runOverrides.tools;
    const timeoutMsRaw = String(memberEditTimeoutMs || "").trim();
    if (timeoutMsRaw.length > 0) {
      const parsed = Number.parseInt(timeoutMsRaw, 10);
      if (!Number.isFinite(parsed)) {
        setMemberEditError("timeout_ms must be a number");
        return;
      }
      runOverrides.timeout_ms = parsed;
    } else if (Object.prototype.hasOwnProperty.call(runOverrides, "timeout_ms")) {
      delete runOverrides.timeout_ms;
    }
    if (Object.keys(runOverrides).length > 0) {
      meta.run_overrides = runOverrides;
      hasMeta = true;
    } else if (Object.prototype.hasOwnProperty.call(meta, "run_overrides")) {
      delete meta.run_overrides;
      hasMeta = true;
    }
    setMemberEditError(null);
    setMemberEditBusy(true);
    try {
      const payload: BrokerTeamMemberUpdateRequest = {
        role,
        status,
        capabilities: caps,
        agent_id: agentId,
        deployment_id: deploymentId,
      };
      if (hasMeta) payload.meta = meta;
      if (weightValue !== undefined) payload.weight = weightValue;
      const resp = await apiBrokerTeamMemberUpdate(base, tid, mid, payload, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "update member failed");
      await refreshMembers(tid);
      handleCancelMemberEdit();
    } catch (err) {
      setMemberEditError(String(err));
    } finally {
      setMemberEditBusy(false);
    }
  }, [
    auth,
    base,
    handleCancelMemberEdit,
    memberEditAgentId,
    memberEditBackendLabel,
    memberEditBaseUrl,
    memberEditCapabilities,
    memberEditDeploymentId,
    memberEditId,
    memberEditMetaJson,
    memberEditModel,
    memberEditRole,
    memberEditStatus,
    memberEditSummaryModel,
    memberEditTimeoutMs,
    memberEditTools,
    memberEditWeight,
    refreshMembers,
    teamIdTrimmed,
  ]);

  const handleAddConnectedAgentsToTeam = React.useCallback(async () => {
    const tid = teamIdTrimmed;
    if (!tid) return;
    if (!canQuery) return;
    if (!memberRole.trim()) {
      setMembersError("role required");
      return;
    }
    if (memberAgentOptions.length === 0) {
      setMembersError("load agents before bulk add");
      return;
    }
    const role = String(memberRole || "").trim() || "executor";
    const existingAgentIds = new Set<string>();
    for (const member of membersList) {
      const aid = String(member?.agent_id || "").trim();
      if (aid) existingAgentIds.add(aid);
    }
    const payloads: BrokerTeamMemberUpsertRequest[] = [];
    for (const agent of memberAgentOptions) {
      const aid = String(agent?.agent_id || "").trim();
      if (!aid || existingAgentIds.has(aid)) continue;
      const deployments = Array.isArray(agent?.deployments) ? agent.deployments : [];
      const connected = agent?.connected === true || deployments.length > 0;
      if (!connected) continue;
      const payload: BrokerTeamMemberUpsertRequest = { role, agent_id: aid };
      if (deployments.length > 0) {
        const depId = deployments[0]?.deployment_id ? String(deployments[0].deployment_id) : "";
        if (depId) payload.deployment_id = depId;
      }
      payloads.push(payload);
      existingAgentIds.add(aid);
    }
    if (payloads.length === 0) {
      setMembersError("no connected agents to add (already in team)");
      return;
    }
    if (!window.confirm(`Add ${payloads.length} connected agent(s) to team?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      for (const payload of payloads) {
        const resp = await apiBrokerTeamMembersUpsert(base, tid, payload, auth);
        if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "member create failed");
      }
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [auth, base, canQuery, memberAgentOptions, memberRole, membersList, refreshMembers, teamIdTrimmed]);

  const handleDeleteMember = React.useCallback(async (memberIdRaw: string) => {
    const tid = teamIdTrimmed;
    const mid = String(memberIdRaw || "").trim();
    if (!tid || !mid) return;
    if (!window.confirm(`Remove member "${mid}"?`)) return;
    setMembersError(null);
    setMembersBusy(true);
    try {
      const resp = await apiBrokerTeamMembersDelete(base, tid, mid, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "delete member failed");
      await refreshMembers(tid);
    } catch (err) {
      setMembersError(String(err));
    } finally {
      setMembersBusy(false);
    }
  }, [auth, base, refreshMembers, teamIdTrimmed]);

  const handleAddRule = React.useCallback(async () => {
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
      const body: BrokerTeamQuorumRuleUpsertRequest = { action, min_approvals: min, quorum_mode: mode };
      const resp = await apiBrokerTeamQuorumUpsert(base, tid, body, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "upsert quorum rule failed");
      await refreshRules(tid);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  }, [auth, base, refreshRules, ruleAction, ruleMinApprovals, ruleMode, teamIdTrimmed]);

  const handleDeleteRule = React.useCallback(async (ruleIdRaw: string) => {
    const tid = teamIdTrimmed;
    const rid = String(ruleIdRaw || "").trim();
    if (!tid || !rid) return;
    if (!window.confirm(`Delete quorum rule "${rid}"?`)) return;
    setRulesError(null);
    setRulesBusy(true);
    try {
      const resp = await apiBrokerTeamQuorumDelete(base, tid, rid, auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "delete quorum rule failed");
      await refreshRules(tid);
    } catch (err) {
      setRulesError(String(err));
    } finally {
      setRulesBusy(false);
    }
  }, [auth, base, refreshRules, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setTeamDetails(null);
      setMembers(null);
      setRules(null);
      return;
    }
  }, [teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      loadTeamEditsFromDetails(null);
      return;
    }
    loadTeamEditsFromDetails(teamDetails);
  }, [loadTeamEditsFromDetails, teamDetails, teamIdTrimmed]);

  React.useEffect(() => {
    if (!canQuery || !teamIdTrimmed) return;
    void refreshTeamDetails(teamIdTrimmed);
    void refreshMembers(teamIdTrimmed);
    void refreshRules(teamIdTrimmed);
  }, [canQuery, refreshMembers, refreshRules, refreshTeamDetails, teamIdTrimmed]);

  React.useEffect(() => {
    if (!memberAgentId) {
      if (memberDeploymentId) setMemberDeploymentId("");
      return;
    }
    if (memberDeploymentId || memberAgentDeployments.length === 0) return;
    const first = memberAgentDeployments[0];
    const depId = first?.deployment_id ? String(first.deployment_id) : "";
    if (depId) setMemberDeploymentId(depId);
  }, [memberAgentDeployments, memberAgentId, memberDeploymentId]);

  React.useEffect(() => {
    if (!memberEditAgentId) {
      if (memberEditDeploymentId) setMemberEditDeploymentId("");
      return;
    }
    if (memberEditDeploymentId || memberEditAgentDeployments.length === 0) return;
    const first = memberEditAgentDeployments[0];
    const depId = first?.deployment_id ? String(first.deployment_id) : "";
    if (depId) setMemberEditDeploymentId(depId);
  }, [memberEditAgentDeployments, memberEditAgentId, memberEditDeploymentId]);

  return {
    handleAddConnectedAgentsToTeam,
    handleAddMember,
    handleAddRule,
    handleCancelMemberEdit,
    handleDeleteMember,
    handleDeleteRule,
    handleEditMember,
    handleRoleGraphEdgesChange,
    handleRoleInstructionsChange,
    handleRolePromptModeChange,
    handleRemovePausedMembers,
    handleSaveMemberEdit,
    handleSetAllMemberStatus,
    handleToggleMemberStatus,
    handleUpdateTeam,
    loadTeamEditsFromDetails,
    memberAgentDeployments,
    memberAgentId,
    memberAgentOptions,
    memberAgents,
    memberBaseUrl,
    memberBackendLabel,
    memberCapabilities,
    memberDeploymentId,
    memberEditAgentDeployments,
    memberEditAgentId,
    memberEditBackendLabel,
    memberEditBaseUrl,
    memberEditBusy,
    memberEditCapabilities,
    memberEditDeploymentId,
    memberEditError,
    memberEditId,
    memberEditMetaJson,
    memberEditModel,
    memberEditRole,
    memberEditStatus,
    memberEditSummaryModel,
    memberEditTimeoutMs,
    memberEditTools,
    memberEditWeight,
    memberId,
    memberModel,
    memberRole,
    memberStatus,
    memberSummaryModel,
    memberTimeoutMs,
    memberTools,
    memberWeight,
    members,
    membersBusy,
    membersError,
    membersList,
    refreshMembers,
    refreshTeamDetails,
    refreshRules,
    rolePlanOptions,
    ruleAction,
    ruleMinApprovals,
    ruleMode,
    rules,
    rulesBusy,
    rulesError,
    rulesList,
    setMemberAgentId,
    setMemberBackendLabel,
    setMemberBaseUrl,
    setMemberCapabilities,
    setMemberDeploymentId,
    setMemberEditAgentId,
    setMemberEditBackendLabel,
    setMemberEditBaseUrl,
    setMemberEditCapabilities,
    setMemberEditDeploymentId,
    setMemberEditMetaJson,
    setMemberEditModel,
    setMemberEditRole,
    setMemberEditStatus,
    setMemberEditSummaryModel,
    setMemberEditTimeoutMs,
    setMemberEditTools,
    setMemberEditWeight,
    setMemberId,
    setMemberModel,
    setMemberRole,
    setMemberStatus,
    setMemberSummaryModel,
    setMemberTimeoutMs,
    setMemberTools,
    setMemberWeight,
    setRuleAction,
    setRuleMinApprovals,
    setRuleMode,
    setTeamEditMetaJson,
    setTeamEditName,
    setTeamEditPolicyRef,
    setTeamEditRoleOverridesJson,
    setTeamEditSharedMode,
    setTeamEditSharedScope,
    setTeamEditTags,
    teamDetails,
    teamEditBusy,
    teamEditError,
    teamEditMetaJson,
    teamEditName,
    teamEditPolicyRef,
    teamEditRoleOverridesJson,
    teamEditSharedMode,
    teamEditSharedScope,
    teamEditTags,
    teamRoleGraphEdges,
    teamRoleInstructions,
    teamRolePromptMode,
  };
}
