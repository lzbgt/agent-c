import React from "react";
import {
  apiBrokerTeamRunApprovalsCreate,
  apiBrokerTeamRunApprovalsList,
  apiBrokerTeamRunCancel,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunList,
  apiBrokerTeamRunModeratorDirective,
  apiBrokerTeamRunModeratorEvents,
  apiBrokerTeamRunModeratorTask,
  apiBrokerTeamRunRuntimeMembersUpdate,
  type ApiAuth,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import { TEAM_RUN_EVENT_TYPES, parseCsvList } from "./teamRunUtils";
import type { TeamRunApprovalRow } from "./teamRunPanelTypes";
import type { BrokerEventRow } from "./types";

type UseBrokerTeamRunControlStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  quorumEvents?: BrokerEventRow[];
  runResult: any | null;
  runRuntimeMembersJson: string;
  setRunRuntimeMembersJson: (value: string) => void;
};

export default function useBrokerTeamRunControlState({
  base,
  auth,
  canQuery,
  teamIdTrimmed,
  quorumEvents,
  runResult,
  runRuntimeMembersJson,
  setRunRuntimeMembersJson,
}: UseBrokerTeamRunControlStateArgs) {
  const [runLookupByTeam, setRunLookupByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.teamRunLookupByTeam",
    {},
  );
  const [runLookupId, setRunLookupIdState] = React.useState<string>("");
  const [runLookupBusy, setRunLookupBusy] = React.useState<boolean>(false);
  const [runLookupError, setRunLookupError] = React.useState<string | null>(null);
  const [runLookupResult, setRunLookupResult] = React.useState<any | null>(null);
  const [runCancelBusy, setRunCancelBusy] = React.useState<boolean>(false);
  const [runCancelError, setRunCancelError] = React.useState<string | null>(null);
  const [runCancelNote, setRunCancelNote] = React.useState<string>("");
  const [autoRefreshRunLookup, setAutoRefreshRunLookup] = useLocalStorageState<boolean>(
    "agentui.teamRunLookupAuto",
    true,
  );
  const [autoResumeRunLookup, setAutoResumeRunLookup] = useLocalStorageState<boolean>(
    "agentui.teamRunLookupResume",
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
  const [runtimeUpdateMode, setRuntimeUpdateMode] = React.useState<string>("replace");
  const [runtimeUpdateBusy, setRuntimeUpdateBusy] = React.useState<boolean>(false);
  const [runtimeUpdateError, setRuntimeUpdateError] = React.useState<string | null>(null);
  const [runtimeUpdateNote, setRuntimeUpdateNote] = React.useState<string>("");
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
  const autoResumeKeyRef = React.useRef<string>("");

  const quorumRequestRows = React.useMemo(() => {
    const rows = Array.isArray(quorumEvents) ? quorumEvents : [];
    const filtered = rows.filter((ev) => ev?.type === "team_quorum_request");
    if (!teamIdTrimmed) return filtered.slice(0, 6);
    return filtered.filter((ev) => String(ev?.payload?.team_id || "") === teamIdTrimmed).slice(0, 6);
  }, [quorumEvents, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRunLookupIdState("");
      return;
    }
    const next = runLookupByTeam[teamIdTrimmed] || "";
    setRunLookupIdState(next);
  }, [runLookupByTeam, teamIdTrimmed]);

  const setRunLookupId = React.useCallback(
    (next: string) => {
      setRunLookupIdState(next);
      if (!teamIdTrimmed) return;
      setRunLookupByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [setRunLookupByTeam, teamIdTrimmed],
  );

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
    for (const member of members) {
      const memberId = String(member?.member_id || "").trim();
      if (memberId) out.add(memberId);
    }
    for (const member of runtime) {
      const memberId = String(member?.member_id || "").trim();
      if (memberId) out.add(memberId);
    }
    return Array.from(out);
  }, [runLookupResult]);
  const runAgentOptions = React.useMemo(() => {
    const out = new Set<string>();
    const members = Array.isArray(runLookupResult?.members) ? runLookupResult.members : [];
    const runtime = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    for (const member of members) {
      const agentId = String(member?.agent_id || "").trim();
      if (agentId) out.add(agentId);
    }
    for (const member of runtime) {
      const agentId = String(member?.agent_id || "").trim();
      if (agentId) out.add(agentId);
    }
    return Array.from(out);
  }, [runLookupResult]);
  const runRoleOptions = React.useMemo(() => {
    const out = new Set<string>();
    const members = Array.isArray(runLookupResult?.members) ? runLookupResult.members : [];
    const runtime = Array.isArray(runLookupResult?.runtime_members) ? runLookupResult.runtime_members : [];
    for (const member of members) {
      const role = String(member?.role || "").trim();
      if (role) out.add(role);
    }
    for (const member of runtime) {
      const role = String(member?.role || "").trim();
      if (role) out.add(role);
    }
    return Array.from(out);
  }, [runLookupResult]);

  const fetchRunStatus = React.useCallback(
    async (runId: string) => {
      if (!teamIdTrimmed || !runId) {
        setRunLookupError("missing team_id or run id");
        return;
      }
      setRunLookupError(null);
      setRunLookupBusy(true);
      try {
        const resp = await apiBrokerTeamRunGet(base, teamIdTrimmed, runId, auth);
        setRunLookupResult(resp);
      } catch (err) {
        setRunLookupError(String(err));
      } finally {
        setRunLookupBusy(false);
      }
    },
    [auth, base, teamIdTrimmed],
  );

  const handleRunLookup = React.useCallback(async () => {
    const runId = String(runLookupId || "").trim();
    await fetchRunStatus(runId);
  }, [fetchRunStatus, runLookupId]);

  React.useEffect(() => {
    if (!autoResumeRunLookup || !canQuery) return;
    const runId = String(runLookupId || "").trim();
    if (!teamIdTrimmed || !runId) return;
    const key = `${teamIdTrimmed}:${runId}`;
    if (autoResumeKeyRef.current === key) return;
    autoResumeKeyRef.current = key;
    void fetchRunStatus(runId);
  }, [autoResumeRunLookup, canQuery, fetchRunStatus, runLookupId, teamIdTrimmed]);

  const loadRecentRuns = React.useCallback(async () => {
    if (!teamIdTrimmed) {
      setRecentRuns([]);
      return;
    }
    setRecentRunsBusy(true);
    setRecentRunsError(null);
    try {
      const resp = await apiBrokerTeamRunList(base, teamIdTrimmed, auth, {
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
  }, [auth, base, recentRunsLimit, recentRunsStatus, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRecentRuns([]);
      return;
    }
    void loadRecentRuns();
  }, [loadRecentRuns, teamIdTrimmed]);

  React.useEffect(() => {
    if (!recentRunsLive || !canQuery) return;
    const rows = Array.isArray(quorumEvents) ? quorumEvents : [];
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
  }, [canQuery, loadRecentRuns, quorumEvents, recentRunsLive, teamIdTrimmed]);

  const resolveRunId = React.useCallback(
    () => String(runLookupResult?.team_run_id || runLookupId || runResult?.team_run_id || "").trim(),
    [runLookupId, runLookupResult, runResult],
  );

  React.useEffect(() => {
    if (!autoRefreshRunLookup || !canQuery) return;
    const runId = resolveRunId();
    if (!runId) return;
    const rows = Array.isArray(quorumEvents) ? quorumEvents : [];
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
  }, [autoRefreshRunLookup, canQuery, fetchRunStatus, quorumEvents, resolveRunId, runLookupLastEvent, teamIdTrimmed]);

  const handleRunCancel = React.useCallback(async () => {
    const runId = resolveRunId();
    if (!teamIdTrimmed || !runId) {
      setRunCancelError("missing team_id or run id");
      return;
    }
    setRunCancelError(null);
    setRunCancelNote("");
    setRunCancelBusy(true);
    try {
      const resp = await apiBrokerTeamRunCancel(base, teamIdTrimmed, runId, auth);
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
  }, [auth, base, resolveRunId, teamIdTrimmed]);

  const buildModeratorTargets = React.useCallback(() => {
    const roles = parseCsvList(moderatorTargetRoles).map((role) => role.toLowerCase());
    const members = parseCsvList(moderatorTargetMembers);
    const agents = parseCsvList(moderatorTargetAgents);
    if (roles.length === 0 && members.length === 0 && agents.length === 0) return undefined;
    return { roles, member_ids: members, agent_ids: agents };
  }, [moderatorTargetAgents, moderatorTargetMembers, moderatorTargetRoles]);

  const handleModeratorDirectivePublish = React.useCallback(async () => {
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
      const resp = await apiBrokerTeamRunModeratorDirective(base, teamIdTrimmed, runId, payload, auth);
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
  }, [
    auth,
    base,
    buildModeratorTargets,
    moderatorAppendToSession,
    moderatorAssignees,
    moderatorDirective,
    moderatorDirectiveScope,
    resolveRunId,
    teamIdTrimmed,
  ]);

  const handleModeratorTaskPublish = React.useCallback(async () => {
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
      const resp = await apiBrokerTeamRunModeratorTask(base, teamIdTrimmed, runId, payload, auth);
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
  }, [
    auth,
    base,
    buildModeratorTargets,
    moderatorAppendToSession,
    moderatorAssignees,
    moderatorTaskDetail,
    moderatorTaskStatus,
    moderatorTaskTitle,
    resolveRunId,
    teamIdTrimmed,
  ]);

  const handleModeratorEventsLoad = React.useCallback(async () => {
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
        base,
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
        auth,
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
  }, [
    auth,
    base,
    moderatorEventsLimit,
    moderatorEventsMaxBytes,
    moderatorEventsTypes,
    moderatorTargetAgents,
    moderatorTargetMembers,
    moderatorTargetRoles,
    resolveRunId,
    teamIdTrimmed,
  ]);

  const handleRuntimeMembersLoadFromRun = React.useCallback(() => {
    const runtimeMembers = runLookupResult?.runtime_members;
    if (!Array.isArray(runtimeMembers) || runtimeMembers.length === 0) {
      setRuntimeUpdateError("no runtime members to load");
      return;
    }
    setRunRuntimeMembersJson(JSON.stringify(runtimeMembers, null, 2));
    setRuntimeUpdateError(null);
  }, [runLookupResult, setRunRuntimeMembersJson]);

  const applyRuntimeMembersUpdate = React.useCallback(
    async (members: any[], mode: string) => {
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
          base,
          teamIdTrimmed,
          runId,
          { mode, runtime_members: members },
          auth,
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
    },
    [auth, base, resolveRunId, setRunRuntimeMembersJson, teamIdTrimmed],
  );

  const handleRuntimeMembersUpdate = React.useCallback(async () => {
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
  }, [applyRuntimeMembersUpdate, runRuntimeMembersJson, runtimeUpdateMode]);

  const handleRuntimeMemberToggle = React.useCallback(
    async (member: any) => {
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
        const existingMemberId = String(item?.member_id || "").trim();
        const existingAgentId = String(item?.agent_id || "").trim();
        const match = memberId ? existingMemberId === memberId : existingAgentId === agentId;
        if (!match) return item;
        const current = String(item?.status || "active").toLowerCase();
        const next = current === "paused" ? "active" : "paused";
        return { ...item, status: next };
      });
      await applyRuntimeMembersUpdate(updated, "replace");
    },
    [applyRuntimeMembersUpdate, runLookupResult],
  );

  const handleRuntimeMemberRemove = React.useCallback(
    async (member: any) => {
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
        const existingMemberId = String(item?.member_id || "").trim();
        const existingAgentId = String(item?.agent_id || "").trim();
        if (memberId) return existingMemberId !== memberId;
        return existingAgentId !== agentId;
      });
      await applyRuntimeMembersUpdate(updated, "replace");
    },
    [applyRuntimeMembersUpdate, runLookupResult],
  );

  const handleApprovalsRefresh = React.useCallback(async () => {
    const runId = approvalRunIdTrimmed;
    if (!teamIdTrimmed || !runId) {
      setApprovalsError("missing team_id or run id");
      return;
    }
    setApprovalsError(null);
    setApprovalsBusy(true);
    try {
      const resp = await apiBrokerTeamRunApprovalsList(base, teamIdTrimmed, runId, auth);
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
  }, [approvalRunIdTrimmed, auth, base, teamIdTrimmed]);

  const handleApprovalSubmit = React.useCallback(async () => {
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
      const resp = await apiBrokerTeamRunApprovalsCreate(base, teamIdTrimmed, runId, payload, auth);
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
  }, [approvalDecision, approvalMemberId, approvalReason, approvalRuleId, approvalRunIdTrimmed, auth, base, teamIdTrimmed]);

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

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    setRunLookupResult(null);
    setRunLookupError(null);
    setRunLookupId("");
    setApprovalRunId("");
    setApprovals(null);
    setApprovalsError(null);
    lastAutoApprovalRunIdRef.current = "";
  }, [setRunLookupId, teamIdTrimmed]);

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

  return {
    quorumRequestRows,
    runLookupId,
    setRunLookupId,
    runLookupBusy,
    runLookupError,
    runLookupResult,
    runCancelBusy,
    runCancelError,
    runCancelNote,
    autoRefreshRunLookup,
    setAutoRefreshRunLookup,
    autoResumeRunLookup,
    setAutoResumeRunLookup,
    moderatorDirective,
    setModeratorDirective,
    moderatorDirectiveScope,
    setModeratorDirectiveScope,
    moderatorTaskTitle,
    setModeratorTaskTitle,
    moderatorTaskDetail,
    setModeratorTaskDetail,
    moderatorTaskStatus,
    setModeratorTaskStatus,
    moderatorTargetRoles,
    setModeratorTargetRoles,
    moderatorTargetMembers,
    setModeratorTargetMembers,
    moderatorTargetAgents,
    setModeratorTargetAgents,
    moderatorAssignees,
    setModeratorAssignees,
    moderatorAppendToSession,
    setModeratorAppendToSession,
    moderatorBusy,
    moderatorError,
    moderatorSuccess,
    moderatorEvents,
    moderatorEventsBusy,
    moderatorEventsError,
    moderatorEventsTypes,
    setModeratorEventsTypes,
    moderatorEventsMaxBytes,
    setModeratorEventsMaxBytes,
    moderatorEventsLimit,
    setModeratorEventsLimit,
    moderatorEventsExpanded,
    setModeratorEventsExpanded,
    runtimeUpdateMode,
    setRuntimeUpdateMode,
    runtimeUpdateBusy,
    runtimeUpdateError,
    runtimeUpdateNote,
    approvalsLastSyncMs,
    approvalRunId,
    setApprovalRunId,
    approvalsBusy,
    approvalMemberId,
    setApprovalMemberId,
    approvalDecision,
    setApprovalDecision,
    approvalRuleId,
    setApprovalRuleId,
    approvalReason,
    setApprovalReason,
    approvalsError,
    approvals,
    recentRunsLimit,
    setRecentRunsLimit,
    recentRunsStatus,
    setRecentRunsStatus,
    recentRunsLive,
    setRecentRunsLive,
    recentRunsBusy,
    recentRunsError,
    recentRunsItems,
    runMemberSessions,
    runMemberOptions,
    runAgentOptions,
    runRoleOptions,
    resolveRunId,
    fetchRunStatus,
    handleRunLookup,
    loadRecentRuns,
    handleRunCancel,
    handleModeratorDirectivePublish,
    handleModeratorTaskPublish,
    handleModeratorEventsLoad,
    handleRuntimeMembersLoadFromRun,
    handleRuntimeMembersUpdate,
    handleRuntimeMemberToggle,
    handleRuntimeMemberRemove,
    handleApprovalsRefresh,
    handleApprovalSubmit,
  };
}
