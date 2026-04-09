import React from "react";

import { apiBrokerTeamRunCreate, type ApiAuth } from "../../api";
import type { InlineApproval } from "./TeamRunCreatePanel";
import type { QuorumEval, RuntimeMemberDraft } from "./teamRunPanelTypes";
import type { TeamRunCreateResult } from "./teamRunStatusTypes";
import {
  normalizeRoleInstructionMap,
  normalizeRolePromptMode,
  normalizeSharedMemoryMode,
} from "./teamRunUtils";
import type { TeamMemberRow } from "./types";

type UseBrokerTeamRunCreateRequestStateArgs = {
  base: string;
  auth: ApiAuth;
  teamIdTrimmed: string;
  membersList: TeamMemberRow[];
  teamMeta?: Record<string, unknown> | null;
  runRuntimeMembersJson: string;
  setRunError: React.Dispatch<React.SetStateAction<string | null>>;
};

export default function useBrokerTeamRunCreateRequestState({
  base,
  auth,
  teamIdTrimmed,
  membersList,
  teamMeta,
  runRuntimeMembersJson,
  setRunError,
}: UseBrokerTeamRunCreateRequestStateArgs) {
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
  const [runApprovals, setRunApprovals] = React.useState<InlineApproval[]>([]);
  const [runApprovalMemberId, setRunApprovalMemberId] = React.useState<string>("");
  const [runApprovalRuleId, setRunApprovalRuleId] = React.useState<string>("");
  const [runApprovalDecision, setRunApprovalDecision] = React.useState<"approve" | "deny">("approve");
  const [runApprovalReason, setRunApprovalReason] = React.useState<string>("");
  const [runBusy, setRunBusy] = React.useState<boolean>(false);
  const [runResult, setRunResult] = React.useState<TeamRunCreateResult | null>(null);
  const [runQuorum, setRunQuorum] = React.useState<QuorumEval | null>(null);

  const teamRoleOverridesDefaults =
    teamMeta?.role_overrides && typeof teamMeta.role_overrides === "object"
      ? (teamMeta.role_overrides as Record<string, unknown>)
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
  }, [runApprovalDecision, runApprovalMemberId, runApprovalReason, runApprovalRuleId, setRunError]);

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
        const runPayload: Record<string, unknown> = { prompt };
        const model = String(runModel || "").trim();
        if (model) runPayload.model = model;
        const tools = String(runTools || "").trim();
        if (tools) runPayload.tools = tools;
        const sharedScope = String(runSharedMemoryScope || "").trim();
        if (sharedScope) {
          runPayload.memory_scope_id = sharedScope;
          runPayload.memory_scope_mode = normalizeSharedMemoryMode(runSharedMemoryMode);
        }
        const teamPayload: Record<string, unknown> = {};
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
            let parsed: unknown = null;
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
            teamPayload.member_overrides = parsed as Record<string, unknown>;
          }
        }
        const roleOverridesRaw = String(runRoleOverridesJson || "").trim();
        if (roleOverridesRaw) {
          let parsed: unknown = null;
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
          teamPayload.role_overrides = parsed as Record<string, unknown>;
        }
        if (runRoleInstructionsOverride) {
          teamPayload.role_instructions = runRoleInstructions;
          const promptMode = String(runRolePromptMode || "").trim();
          if (promptMode) teamPayload.role_prompt_mode = promptMode;
        }
        const runtimeMembersRaw = String(runRuntimeMembersJson || "").trim();
        if (runtimeMembersRaw) {
          let parsed: unknown = null;
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
          teamPayload.runtime_members = parsed as RuntimeMemberDraft[];
        }
        if (runApprovals.length > 0) {
          teamPayload.approvals = runApprovals;
        }
        const resp = await apiBrokerTeamRunCreate(base, teamIdTrimmed, { run: runPayload, team: teamPayload }, auth);
        if (!resp.ok) {
          setRunResult(null);
          setRunError(resp.error || resp.err || resp.code || "team run failed");
          const quorumRaw = (resp as unknown as { quorum?: unknown }).quorum;
          if (quorumRaw && typeof quorumRaw === "object" && !Array.isArray(quorumRaw)) {
            setRunQuorum(quorumRaw as QuorumEval);
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
      setRunError,
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

  const handleSeedExplicitOverrides = React.useCallback(() => {
    if (membersList.length === 0) {
      setRunError("no team members loaded");
      return;
    }
    const seed: Record<string, unknown> = {};
    for (const member of membersList) {
      const memberId = String(member?.member_id || "").trim();
      if (!memberId) continue;
      const meta = member?.meta && typeof member.meta === "object" ? (member.meta as Record<string, unknown>) : null;
      const overridesRaw =
        meta?.run_overrides && typeof meta.run_overrides === "object"
          ? (meta.run_overrides as Record<string, unknown>)
          : null;
      const entry: Record<string, unknown> = {};
      if (overridesRaw) {
        const model = typeof overridesRaw.model === "string" ? overridesRaw.model : "";
        const baseUrl = typeof overridesRaw.base_url === "string" ? overridesRaw.base_url : "";
        const summaryModel = typeof overridesRaw.summary_model === "string" ? overridesRaw.summary_model : "";
        const tools = typeof overridesRaw.tools === "string" ? overridesRaw.tools : "";
        const timeoutMs = typeof overridesRaw.timeout_ms === "number" ? overridesRaw.timeout_ms : null;
        const maxSteps = typeof overridesRaw.max_steps === "number" ? overridesRaw.max_steps : null;
        const streamAssistant =
          typeof overridesRaw.stream_assistant === "boolean" ? overridesRaw.stream_assistant : null;
        if (model) entry.model = model;
        if (baseUrl) entry.base_url = baseUrl;
        if (summaryModel) entry.summary_model = summaryModel;
        if (tools) entry.tools = tools;
        if (timeoutMs !== null) entry.timeout_ms = timeoutMs;
        if (maxSteps !== null) entry.max_steps = maxSteps;
        if (streamAssistant !== null) entry.stream_assistant = streamAssistant;
      }
      if (Object.keys(entry).length > 0) {
        seed[memberId] = entry;
      }
    }
    setRunOverridesMode("explicit");
    setRunMemberOverridesJson(JSON.stringify(seed, null, 2));
    setRunError(null);
  }, [membersList, setRunError]);

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
    if (!teamIdTrimmed) return;
    setRunResult(null);
    setRunError(null);
    setRunQuorum(null);
    setRunApprovals([]);
    setRunApprovalMemberId("");
    setRunApprovalRuleId("");
    setRunApprovalReason("");
  }, [teamIdTrimmed, setRunError]);

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
    runResult,
    setRunResult,
    runQuorum,
    teamRoleOverrideKeys,
    teamRoleInstructionKeys,
    runRolePlanOptions,
    handleAddRunApproval,
    handleCreateRun,
    handleSeedRoleOverrides,
    handleSeedRoleInstructions,
    handleSeedExplicitOverrides,
  };
}
