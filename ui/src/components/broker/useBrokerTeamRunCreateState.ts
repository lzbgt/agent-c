import React from "react";
import {
  apiBrokerListAgents,
  apiBrokerTeamMembersUpsert,
  apiBrokerTeamRunCreate,
  type ApiAuth,
} from "../../api";
import type { InlineApproval } from "./TeamRunCreatePanel";
import {
  buildRoleAllocatedRuntimeMembers,
  buildRuntimeAgentAdditions,
  normalizeRoleInstructionMap,
  normalizeRolePromptMode,
  normalizeSharedMemoryMode,
} from "./teamRunUtils";
import type {
  QuorumEval,
  RuntimeMembersPreview,
  RuntimeSavePreview,
  RuntimeTeamDiff,
} from "./teamRunPanelTypes";
import type { TeamMemberRow } from "./types";

type UseBrokerTeamRunCreateStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  membersList: TeamMemberRow[];
  teamMeta?: Record<string, any> | null;
  onMembersRefresh?: (teamId: string) => Promise<void> | void;
};

export default function useBrokerTeamRunCreateState({
  base,
  auth,
  canQuery,
  teamIdTrimmed,
  membersList,
  teamMeta,
  onMembersRefresh,
}: UseBrokerTeamRunCreateStateArgs) {
  const [runPrompt, setRunPrompt] = React.useState<string>("");
  const [runModel, setRunModel] = React.useState<string>("");
  const [runTools, setRunTools] = React.useState<string>("host");
  const [runMode, setRunMode] = React.useState<string>("async");
  const [runRole, setRunRole] = React.useState<string>("");
  const [runRoles, setRunRoles] = React.useState<string>("");
  const [runConcurrency, setRunConcurrency] = React.useState<string>("1");
  const [runTimeoutMs, setRunTimeoutMs] = React.useState<string>("60000");
  const [runSharedMemoryScope, setRunSharedMemoryScope] = React.useState<string>("");
  const [runSharedMemoryMode, setRunSharedMemoryMode] = React.useState<string>("read_write");
  const [runAutoAllocateRoles, setRunAutoAllocateRoles] = React.useState<boolean>(false);
  const [runAutoAllocateMaxMembers, setRunAutoAllocateMaxMembers] = React.useState<string>("");
  const [runQuorumMode, setRunQuorumMode] = React.useState<string>("auto");
  const [runOverridesMode, setRunOverridesMode] = React.useState<string>("member_meta");
  const [runMemberOverridesJson, setRunMemberOverridesJson] = React.useState<string>("");
  const [runRoleOverridesJson, setRunRoleOverridesJson] = React.useState<string>("");
  const [runRoleInstructionsOverride, setRunRoleInstructionsOverride] = React.useState<boolean>(false);
  const [runRoleInstructions, setRunRoleInstructions] = React.useState<Record<string, string>>({});
  const [runRolePromptMode, setRunRolePromptMode] = React.useState<string>("prepend");
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
  const [runApprovals, setRunApprovals] = React.useState<InlineApproval[]>([]);
  const [runApprovalMemberId, setRunApprovalMemberId] = React.useState<string>("");
  const [runApprovalRuleId, setRunApprovalRuleId] = React.useState<string>("");
  const [runApprovalDecision, setRunApprovalDecision] = React.useState<"approve" | "deny">("approve");
  const [runApprovalReason, setRunApprovalReason] = React.useState<string>("");
  const [runBusy, setRunBusy] = React.useState<boolean>(false);
  const [runError, setRunError] = React.useState<string | null>(null);
  const [runResult, setRunResult] = React.useState<any | null>(null);
  const [runQuorum, setRunQuorum] = React.useState<QuorumEval | null>(null);

  const teamRoleOverridesDefaults =
    teamMeta?.role_overrides && typeof teamMeta.role_overrides === "object"
      ? (teamMeta.role_overrides as Record<string, any>)
      : null;
  const teamRoleOverrideKeys = teamRoleOverridesDefaults
    ? Object.keys(teamRoleOverridesDefaults).map((key) => String(key)).filter(Boolean)
    : [];
  const teamRoleInstructionsDefaults = normalizeRoleInstructionMap(teamMeta?.role_instructions);
  const teamRoleInstructionKeys = Object.keys(teamRoleInstructionsDefaults);
  const teamRolePromptModeDefault = normalizeRolePromptMode(teamMeta?.role_prompt_mode);
  const teamSharedMemoryScopeDefault = teamMeta?.shared_memory_scope_id
    ? String(teamMeta.shared_memory_scope_id).trim()
    : "";
  const teamSharedMemoryModeDefault = normalizeSharedMemoryMode(
    teamMeta?.shared_memory_mode ?? teamMeta?.memory_scope_mode ?? "read_write",
  );

  const runRolePlanOptions = React.useMemo(() => {
    const set = new Set<string>();
    for (const role of teamRoleOverrideKeys) {
      const value = String(role || "").trim().toLowerCase();
      if (value) set.add(value);
    }
    for (const role of teamRoleInstructionKeys) {
      const value = String(role || "").trim().toLowerCase();
      if (value) set.add(value);
    }
    for (const member of membersList) {
      const value = String(member?.role || "").trim().toLowerCase();
      if (value) set.add(value);
    }
    return Array.from(set).filter(Boolean).sort();
  }, [membersList, teamRoleInstructionKeys, teamRoleOverrideKeys]);

  const runtimeMembersPreview = React.useMemo<RuntimeMembersPreview>(() => {
    const raw = String(runRuntimeMembersJson || "").trim();
    if (!raw) return { items: [], error: "" };
    try {
      const parsed = JSON.parse(raw);
      if (!Array.isArray(parsed)) {
        return { items: [], error: "runtime_members must be a JSON array" };
      }
      return { items: parsed, error: "" };
    } catch (err) {
      return { items: [], error: `invalid runtime_members json: ${String(err)}` };
    }
  }, [runRuntimeMembersJson]);

  const runtimeAgentOptions = Array.isArray(runtimeAgents) ? runtimeAgents : [];
  const runtimeSelectedAgent = runtimeAgentOptions.find(
    (agent) => String(agent?.agent_id || "") === String(runtimeMemberAgentId || "").trim(),
  );
  const runtimeAgentDeployments = Array.isArray(runtimeSelectedAgent?.deployments)
    ? (runtimeSelectedAgent.deployments as any[])
    : [];

  const runtimeSavePreview = React.useMemo<RuntimeSavePreview>(() => {
    const existingIDs = new Set<string>();
    for (const member of membersList) {
      const id = String(member?.member_id || "").trim();
      if (id) existingIDs.add(id);
    }
    const items = runtimeMembersPreview.items;
    if (!Array.isArray(items) || items.length === 0) {
      return { newMembers: [], skipped: [], invalid: [] };
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

  const runtimeTeamDiff = React.useMemo<RuntimeTeamDiff>(() => {
    const runtimeItems = runtimeMembersPreview.items;
    if (!Array.isArray(runtimeItems)) {
      return { runtimeOnly: [], teamOnly: [], mismatched: [] };
    }
    const teamByMemberId = new Map<string, TeamMemberRow>();
    const teamByAgentId = new Map<string, TeamMemberRow>();
    for (const member of membersList) {
      const memberId = String(member?.member_id || "").trim();
      const agentId = String(member?.agent_id || "").trim();
      if (memberId) teamByMemberId.set(memberId, member);
      if (agentId) teamByAgentId.set(agentId, member);
    }
    const matched = new Set<string>();
    const runtimeOnly: any[] = [];
    const mismatched: any[] = [];
    for (const item of runtimeItems) {
      const memberId = item?.member_id ? String(item.member_id).trim() : "";
      const agentId = item?.agent_id ? String(item.agent_id).trim() : "";
      const teamMatch = (memberId && teamByMemberId.get(memberId)) || (agentId && teamByAgentId.get(agentId));
      if (!teamMatch) {
        runtimeOnly.push(item);
        continue;
      }
      if (teamMatch?.member_id) matched.add(String(teamMatch.member_id));
      if (!teamMatch?.member_id && teamMatch?.agent_id) matched.add(String(teamMatch.agent_id));
      const diffs: string[] = [];
      const runtimeRole = item?.role ? String(item.role) : "";
      const runtimeDep = item?.deployment_id ? String(item.deployment_id) : "";
      const runtimeStatus = item?.status ? String(item.status) : "";
      if (runtimeRole && teamMatch?.role && runtimeRole !== teamMatch.role) diffs.push("role");
      if (agentId && teamMatch?.agent_id && agentId !== teamMatch.agent_id) diffs.push("agent_id");
      if (runtimeDep && teamMatch?.deployment_id && runtimeDep !== teamMatch.deployment_id) diffs.push("deployment_id");
      if (runtimeStatus && teamMatch?.status && runtimeStatus !== teamMatch.status) diffs.push("status");
      if (diffs.length > 0) {
        mismatched.push({ item, team: teamMatch, diffs });
      }
    }
    const teamOnly = membersList.filter((member) => {
      const memberId = String(member?.member_id || "").trim();
      const agentId = String(member?.agent_id || "").trim();
      if (memberId && matched.has(memberId)) return false;
      if (!memberId && agentId && matched.has(agentId)) return false;
      if (!memberId && !agentId) return false;
      return true;
    });
    return { runtimeOnly, teamOnly, mismatched };
  }, [membersList, runtimeMembersPreview.items]);

  const handleAddRunApproval = React.useCallback(() => {
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
  }, [runApprovalDecision, runApprovalMemberId, runApprovalReason, runApprovalRuleId]);

  const handleCreateRun = React.useCallback(
    async (setRunLookupId: (next: string) => void) => {
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
        const sharedScope = String(runSharedMemoryScope || "").trim();
        if (sharedScope) {
          runPayload.memory_scope_id = sharedScope;
          runPayload.memory_scope_mode = normalizeSharedMemoryMode(runSharedMemoryMode);
        }
        const teamPayload: Record<string, any> = {};
        const role = String(runRole || "").trim();
        if (role) teamPayload.role = role;
        const rolesCsv = String(runRoles || "").trim();
        if (rolesCsv) teamPayload.roles = rolesCsv.split(",").map((item) => item.trim()).filter(Boolean);
        const concurrency = Number.parseInt(String(runConcurrency || ""), 10);
        if (Number.isFinite(concurrency)) teamPayload.max_concurrency = concurrency;
        const timeout = Number.parseInt(String(runTimeoutMs || ""), 10);
        if (Number.isFinite(timeout)) teamPayload.timeout_ms = timeout;
        const mode = String(runMode || "").trim();
        if (mode) teamPayload.mode = mode;
        const quorumMode = String(runQuorumMode || "").trim();
        if (quorumMode) teamPayload.quorum_policy = { mode: quorumMode };
        if (runAutoAllocateRoles) {
          teamPayload.auto_allocate_roles = true;
          const maxMembers = Number.parseInt(String(runAutoAllocateMaxMembers || "").trim(), 10);
          if (Number.isFinite(maxMembers)) {
            teamPayload.auto_allocate_max_members = maxMembers;
          }
        }
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
        if (runRoleInstructionsOverride) {
          teamPayload.role_instructions = runRoleInstructions;
          const promptMode = String(runRolePromptMode || "").trim();
          if (promptMode) teamPayload.role_prompt_mode = promptMode;
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
        const resp = await apiBrokerTeamRunCreate(base, teamIdTrimmed, { run: runPayload, team: teamPayload }, auth);
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
    },
    [
      auth,
      base,
      runApprovals,
      runAutoAllocateMaxMembers,
      runAutoAllocateRoles,
      runConcurrency,
      runMemberOverridesJson,
      runMode,
      runModel,
      runOverridesMode,
      runPrompt,
      runQuorumMode,
      runRole,
      runRoleInstructions,
      runRoleInstructionsOverride,
      runRoleOverridesJson,
      runRolePromptMode,
      runRoles,
      runRuntimeMembersJson,
      runSharedMemoryMode,
      runSharedMemoryScope,
      runTimeoutMs,
      runTools,
      teamIdTrimmed,
    ],
  );

  const handleSeedRoleOverrides = React.useCallback(() => {
    if (!teamRoleOverridesDefaults) return;
    try {
      setRunRoleOverridesJson(JSON.stringify(teamRoleOverridesDefaults, null, 2));
    } catch {
      setRunRoleOverridesJson("");
    }
  }, [teamRoleOverridesDefaults]);

  const handleSeedRoleInstructions = React.useCallback(() => {
    if (teamRoleInstructionKeys.length === 0) return;
    setRunRoleInstructionsOverride(true);
    setRunRoleInstructions({ ...teamRoleInstructionsDefaults });
    setRunRolePromptMode(teamRolePromptModeDefault || "prepend");
  }, [teamRoleInstructionKeys.length, teamRoleInstructionsDefaults, teamRolePromptModeDefault]);

  const refreshRuntimeAgents = React.useCallback(async () => {
    if (!canQuery) return;
    setRuntimeAgentsError(null);
    setRuntimeAgentsBusy(true);
    try {
      const resp = await apiBrokerListAgents(base, auth);
      const rows = Array.isArray(resp?.agents) ? resp.agents : [];
      setRuntimeAgents(rows);
    } catch (err) {
      setRuntimeAgentsError(String(err));
    } finally {
      setRuntimeAgentsBusy(false);
    }
  }, [auth, base, canQuery]);

  const handleAddRuntimeMember = React.useCallback(() => {
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
    const capabilities = String(runtimeMemberCapabilities || "")
      .split(",")
      .map((item) => item.trim())
      .filter(Boolean);
    if (capabilities.length > 0) entry.capabilities = capabilities;
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
  }, [
    runRuntimeMembersJson,
    runtimeMemberAgentId,
    runtimeMemberBackendLabel,
    runtimeMemberBaseUrl,
    runtimeMemberCapabilities,
    runtimeMemberDeploymentId,
    runtimeMemberId,
    runtimeMemberModel,
    runtimeMemberRole,
    runtimeMemberSummaryModel,
    runtimeMemberTimeoutMs,
    runtimeMemberTools,
  ]);

  const handleRemoveRuntimeMember = React.useCallback(
    (idx: number) => {
      const rawItems = runtimeMembersPreview.items;
      if (!Array.isArray(rawItems) || idx < 0 || idx >= rawItems.length) return;
      const next = rawItems.filter((_, index) => index !== idx);
      setRunRuntimeMembersJson(next.length > 0 ? JSON.stringify(next, null, 2) : "");
    },
    [runtimeMembersPreview.items],
  );

  const handleToggleRuntimeMemberStatus = React.useCallback(
    (idx: number) => {
      const rawItems = runtimeMembersPreview.items;
      if (!Array.isArray(rawItems) || idx < 0 || idx >= rawItems.length) return;
      const next = rawItems.map((item, index) => {
        if (index !== idx) return item;
        const statusRaw = item?.status ? String(item.status).toLowerCase() : "";
        const nextStatus = statusRaw === "paused" ? "active" : "paused";
        return { ...item, status: nextStatus };
      });
      setRunRuntimeMembersJson(JSON.stringify(next, null, 2));
    },
    [runtimeMembersPreview.items],
  );

  const handleSetAllRuntimeStatus = React.useCallback(
    (status: "active" | "paused") => {
      const rawItems = runtimeMembersPreview.items;
      if (!Array.isArray(rawItems) || rawItems.length === 0) return;
      const next = rawItems.map((item) => ({ ...item, status }));
      setRunRuntimeMembersJson(JSON.stringify(next, null, 2));
    },
    [runtimeMembersPreview.items],
  );

  const handleRemovePausedRuntimeMembers = React.useCallback(() => {
    const rawItems = runtimeMembersPreview.items;
    if (!Array.isArray(rawItems) || rawItems.length === 0) return;
    const filtered = rawItems.filter((item) => {
      const statusRaw = item?.status ? String(item.status).toLowerCase() : "active";
      return statusRaw !== "paused";
    });
    setRunRuntimeMembersJson(filtered.length > 0 ? JSON.stringify(filtered, null, 2) : "");
  }, [runtimeMembersPreview.items]);

  const handleCompactRuntimeMembers = React.useCallback(() => {
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
          const caps = item.capabilities.map((capability: any) => String(capability).trim()).filter(Boolean);
          if (caps.length > 0) out.capabilities = caps;
        }
        if (typeof item?.weight === "number") out.weight = item.weight;
        if (item?.meta && typeof item.meta === "object") {
          const meta: Record<string, any> = {};
          for (const [key, value] of Object.entries(item.meta as Record<string, any>)) {
            if (value === null || value === undefined) continue;
            if (typeof value === "string" && value.trim() === "") continue;
            if (Array.isArray(value) && value.length === 0) continue;
            if (typeof value === "object" && !Array.isArray(value) && Object.keys(value).length === 0) continue;
            meta[key] = value;
          }
          if (Object.keys(meta).length > 0) out.meta = meta;
        }
        return out;
      })
      .filter((item) => Object.keys(item).length > 0);
    setRunRuntimeMembersJson(compacted.length > 0 ? JSON.stringify(compacted, null, 2) : "");
  }, [runtimeMembersPreview.error, runtimeMembersPreview.items]);

  const handleCopyRuntimeMembers = React.useCallback(async () => {
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
  }, [runRuntimeMembersJson, runtimeMembersPreview.error]);

  const handleImportRuntimeMembers = React.useCallback(
    async (event: React.ChangeEvent<HTMLInputElement>) => {
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
            const exists = merged.some((member: any) => {
              const existingMemberId = member?.member_id ? String(member.member_id).trim() : "";
              const existingAgentId = member?.agent_id ? String(member.agent_id).trim() : "";
              if (memberId && existingMemberId) return memberId === existingMemberId;
              if (agentId && existingAgentId) return agentId === existingAgentId;
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
    },
    [runtimeImportMerge, runtimeMembersPreview.items],
  );

  const handleDownloadRuntimeMembers = React.useCallback(() => {
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
  }, [runRuntimeMembersJson]);

  const handleExportTeamMembers = React.useCallback(() => {
    if (membersList.length === 0) {
      setRunError("no team members to export");
      return;
    }
    const payload = membersList.map((member) => {
      const entry: Record<string, any> = {};
      const memberId = String(member?.member_id || "").trim();
      const agentId = String(member?.agent_id || "").trim();
      const deploymentId = String(member?.deployment_id || "").trim();
      const role = String(member?.role || "").trim();
      const status = String(member?.status || "").trim();
      if (memberId) entry.member_id = memberId;
      if (agentId) entry.agent_id = agentId;
      if (deploymentId) entry.deployment_id = deploymentId;
      if (role) entry.role = role;
      if (status && status !== "active") entry.status = status;
      if (typeof member?.weight === "number") entry.weight = member.weight;
      if (Array.isArray(member?.capabilities)) {
        const caps = member.capabilities.map((capability) => String(capability).trim()).filter(Boolean);
        if (caps.length > 0) entry.capabilities = caps;
      }
      if (member?.meta && typeof member.meta === "object") {
        const meta: Record<string, any> = {};
        for (const [key, value] of Object.entries(member.meta)) {
          if (value === null || value === undefined) continue;
          if (typeof value === "string" && value.trim() === "") continue;
          if (Array.isArray(value) && value.length === 0) continue;
          if (typeof value === "object" && !Array.isArray(value) && Object.keys(value).length === 0) continue;
          meta[key] = value;
        }
        if (Object.keys(meta).length > 0) entry.meta = meta;
      }
      return entry;
    });
    setRunRuntimeMembersJson(JSON.stringify(payload, null, 2));
    setRunError(null);
  }, [membersList]);

  const handleSeedExplicitOverrides = React.useCallback(() => {
    if (membersList.length === 0) {
      setRunError("no team members loaded");
      return;
    }
    const seed: Record<string, any> = {};
    for (const member of membersList) {
      const memberId = String(member?.member_id || "").trim();
      if (!memberId) continue;
      const meta = member?.meta && typeof member.meta === "object" ? (member.meta as Record<string, any>) : null;
      const overridesRaw =
        meta?.run_overrides && typeof meta.run_overrides === "object"
          ? (meta.run_overrides as Record<string, any>)
          : null;
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
        seed[memberId] = entry;
      }
    }
    setRunOverridesMode("explicit");
    setRunMemberOverridesJson(JSON.stringify(seed, null, 2));
    setRunError(null);
  }, [membersList]);

  const handleFixInvalidRuntimeMembers = React.useCallback(() => {
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
      if (!agentId) continue;
      fixed.push(role ? item : { ...item, role: roleDefault });
    }
    setRunRuntimeMembersJson(fixed.length > 0 ? JSON.stringify(fixed, null, 2) : "");
  }, [runtimeMemberRole, runtimeMembersPreview.error, runtimeMembersPreview.items]);

  const handleAddConnectedAgents = React.useCallback(() => {
    if (runtimeMembersPreview.error) {
      setRunError(runtimeMembersPreview.error);
      return;
    }
    const role = String(runtimeMemberRole || "").trim() || "executor";
    const { additions, error } = buildRuntimeAgentAdditions({
      runtimeAgentOptions,
      existingRuntime: runtimeMembersPreview.items,
      membersList,
      role,
    });
    if (error) {
      setRunError(error);
      return;
    }
    setRunRuntimeMembersJson(JSON.stringify([...runtimeMembersPreview.items, ...additions], null, 2));
    setRunError(null);
  }, [membersList, runRuntimeMembersJson, runtimeAgentOptions, runtimeMemberRole, runtimeMembersPreview.error, runtimeMembersPreview.items]);

  const handleAllocateRoleRuntimeMembers = React.useCallback(() => {
    if (runtimeMembersPreview.error) {
      setRunError(runtimeMembersPreview.error);
      return;
    }
    const { additions, warning, error } = buildRoleAllocatedRuntimeMembers({
      runtimeAgentOptions,
      existingRuntime: runtimeMembersPreview.items,
      membersList,
      roles: runRolePlanOptions,
    });
    if (error) {
      setRunError(error);
      return;
    }
    setRunRuntimeMembersJson(JSON.stringify([...runtimeMembersPreview.items, ...additions], null, 2));
    setRunError(warning ?? null);
  }, [membersList, runRolePlanOptions, runtimeAgentOptions, runtimeMembersPreview.error, runtimeMembersPreview.items]);

  const handleSaveRuntimeMembers = React.useCallback(async () => {
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
        await apiBrokerTeamMembersUpsert(base, teamIdTrimmed, payload, auth);
      }
      if (onMembersRefresh) {
        await onMembersRefresh(teamIdTrimmed);
      }
    } catch (err) {
      setRuntimeSaveError(String(err));
    } finally {
      setRuntimeSaveBusy(false);
    }
  }, [auth, base, onMembersRefresh, runtimeMembersPreview.error, runtimeMembersPreview.items, runtimeSavePreview.newMembers, teamIdTrimmed]);

  React.useEffect(() => {
    setRunRoleInstructionsOverride(false);
    setRunRoleInstructions({});
    setRunRolePromptMode("prepend");
    setRunSharedMemoryScope(teamSharedMemoryScopeDefault);
    setRunSharedMemoryMode(teamSharedMemoryModeDefault);
    setRunAutoAllocateRoles(false);
    setRunAutoAllocateMaxMembers("");
  }, [teamIdTrimmed, teamSharedMemoryModeDefault, teamSharedMemoryScopeDefault]);

  React.useEffect(() => {
    if (!canQuery) return;
    if (runtimeAgentsBusy || (runtimeAgents && runtimeAgents.length > 0)) return;
    void refreshRuntimeAgents();
  }, [canQuery, refreshRuntimeAgents, runtimeAgents, runtimeAgentsBusy]);

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
    const deploymentId = first?.deployment_id ? String(first.deployment_id) : "";
    if (deploymentId) {
      setRuntimeMemberDeploymentId(deploymentId);
    }
  }, [runtimeAgentDeployments, runtimeMemberAgentId, runtimeMemberDeploymentId]);

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    setRunResult(null);
    setRunError(null);
    setRunQuorum(null);
    setRunApprovals([]);
    setRunApprovalMemberId("");
    setRunApprovalRuleId("");
    setRunApprovalReason("");
  }, [teamIdTrimmed]);

  return {
    runPrompt,
    setRunPrompt,
    runModel,
    setRunModel,
    runTools,
    setRunTools,
    runMode,
    setRunMode,
    runRole,
    setRunRole,
    runRoles,
    setRunRoles,
    runConcurrency,
    setRunConcurrency,
    runTimeoutMs,
    setRunTimeoutMs,
    runSharedMemoryScope,
    setRunSharedMemoryScope,
    runSharedMemoryMode,
    setRunSharedMemoryMode,
    runAutoAllocateRoles,
    setRunAutoAllocateRoles,
    runAutoAllocateMaxMembers,
    setRunAutoAllocateMaxMembers,
    runQuorumMode,
    setRunQuorumMode,
    runOverridesMode,
    setRunOverridesMode,
    runMemberOverridesJson,
    setRunMemberOverridesJson,
    runRoleOverridesJson,
    setRunRoleOverridesJson,
    runRoleInstructionsOverride,
    setRunRoleInstructionsOverride,
    runRoleInstructions,
    setRunRoleInstructions,
    runRolePromptMode,
    setRunRolePromptMode,
    runRuntimeMembersJson,
    setRunRuntimeMembersJson,
    runtimeMemberId,
    setRuntimeMemberId,
    runtimeMemberAgentId,
    setRuntimeMemberAgentId,
    runtimeMemberDeploymentId,
    setRuntimeMemberDeploymentId,
    runtimeMemberRole,
    setRuntimeMemberRole,
    runtimeMemberCapabilities,
    setRuntimeMemberCapabilities,
    runtimeMemberBackendLabel,
    setRuntimeMemberBackendLabel,
    runtimeMemberModel,
    setRuntimeMemberModel,
    runtimeMemberBaseUrl,
    setRuntimeMemberBaseUrl,
    runtimeMemberSummaryModel,
    setRuntimeMemberSummaryModel,
    runtimeMemberTools,
    setRuntimeMemberTools,
    runtimeMemberTimeoutMs,
    setRuntimeMemberTimeoutMs,
    runtimeAgentsBusy,
    runtimeAgentsError,
    runtimeAgentOptions,
    runtimeAgentDeployments,
    runtimeSaveBusy,
    runtimeSaveError,
    runtimeImportRef,
    runtimeImportMerge,
    setRuntimeImportMerge,
    runApprovals,
    setRunApprovals,
    runApprovalMemberId,
    setRunApprovalMemberId,
    runApprovalRuleId,
    setRunApprovalRuleId,
    runApprovalDecision,
    setRunApprovalDecision,
    runApprovalReason,
    setRunApprovalReason,
    runBusy,
    runError,
    setRunError,
    runResult,
    setRunResult,
    runQuorum,
    teamRoleOverrideKeys,
    teamRoleInstructionKeys,
    runRolePlanOptions,
    runtimeMembersPreview,
    runtimeSavePreview,
    runtimeTeamDiff,
    handleAddRunApproval,
    handleCreateRun,
    handleSeedRoleOverrides,
    handleSeedRoleInstructions,
    refreshRuntimeAgents,
    handleAddRuntimeMember,
    handleRemoveRuntimeMember,
    handleToggleRuntimeMemberStatus,
    handleSetAllRuntimeStatus,
    handleRemovePausedRuntimeMembers,
    handleCompactRuntimeMembers,
    handleCopyRuntimeMembers,
    handleImportRuntimeMembers,
    handleDownloadRuntimeMembers,
    handleExportTeamMembers,
    handleSeedExplicitOverrides,
    handleFixInvalidRuntimeMembers,
    handleAddConnectedAgents,
    handleAllocateRoleRuntimeMembers,
    handleSaveRuntimeMembers,
  };
}
