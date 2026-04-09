import React from "react";

import {
  apiBrokerTeamRunCancel,
  apiBrokerTeamRunGet,
  apiBrokerTeamRunList,
  apiBrokerTeamRunRuntimeMembersUpdate,
  type ApiAuth,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import { asUnknownRecord } from "./brokerObjectUtils";
import type { RuntimeMemberDraft } from "./teamRunPanelTypes";
import type {
  MemberSession,
  TeamRunCreateResult,
  TeamRunLookupResult,
  TeamRunRecentRun,
  TeamRunRuntimeMemberRow,
} from "./teamRunStatusTypes";
import { TEAM_RUN_EVENT_TYPES } from "./teamRunUtils";
import type { BrokerEventRow } from "./types";

type UseBrokerTeamRunLookupStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamIdTrimmed: string;
  quorumEvents?: BrokerEventRow[];
  runResult: TeamRunCreateResult | null;
  runRuntimeMembersJson: string;
  setRunRuntimeMembersJson: (value: string) => void;
};

export default function useBrokerTeamRunLookupState({
  base,
  auth,
  canQuery,
  teamIdTrimmed,
  quorumEvents,
  runResult,
  runRuntimeMembersJson,
  setRunRuntimeMembersJson,
}: UseBrokerTeamRunLookupStateArgs) {
  const [runLookupByTeam, setRunLookupByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.teamRunLookupByTeam",
    {},
  );
  const [runLookupId, setRunLookupIdState] = React.useState<string>("");
  const [runLookupBusy, setRunLookupBusy] = React.useState<boolean>(false);
  const [runLookupError, setRunLookupError] = React.useState<string | null>(null);
  const [runLookupResult, setRunLookupResult] = React.useState<TeamRunLookupResult | null>(null);
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
  const [runtimeUpdateMode, setRuntimeUpdateMode] = React.useState<string>("replace");
  const [runtimeUpdateBusy, setRuntimeUpdateBusy] = React.useState<boolean>(false);
  const [runtimeUpdateError, setRuntimeUpdateError] = React.useState<string | null>(null);
  const [runtimeUpdateNote, setRuntimeUpdateNote] = React.useState<string>("");
  const [recentRuns, setRecentRuns] = React.useState<TeamRunRecentRun[]>([]);
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

  const recentRunsItems = recentRuns;
  const runMemberSessions = React.useMemo(() => {
    const raw = runLookupResult?.member_sessions;
    if (!raw || typeof raw !== "object") return [] as MemberSession[];
    return Object.entries(raw)
      .map(([memberId, sessionId]) => ({
        memberId: String(memberId || ""),
        sessionId: String(sessionId || ""),
      }))
      .filter((row) => row.memberId && row.sessionId);
  }, [runLookupResult]);
  const runMemberOptions = React.useMemo(() => {
    const out = new Set<string>();
    const members = runLookupResult?.members ?? [];
    const runtime = runLookupResult?.runtime_members ?? [];
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
    const members = runLookupResult?.members ?? [];
    const runtime = runLookupResult?.runtime_members ?? [];
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
    const members = runLookupResult?.members ?? [];
    const runtime = runLookupResult?.runtime_members ?? [];
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
      setRecentRuns(resp.runs ?? []);
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
  }, [auth, base, resolveRunId, teamIdTrimmed, setRunLookupId]);

  const handleRuntimeMembersLoadFromRun = React.useCallback(() => {
    const runtimeMembers = runLookupResult?.runtime_members ?? [];
    if (runtimeMembers.length === 0) {
      setRuntimeUpdateError("no runtime members to load");
      return;
    }
    setRunRuntimeMembersJson(JSON.stringify(runtimeMembers, null, 2));
    setRuntimeUpdateError(null);
  }, [runLookupResult, setRunRuntimeMembersJson]);

  const applyRuntimeMembersUpdate = React.useCallback(
    async (members: RuntimeMemberDraft[], mode: "replace" | "merge") => {
      const runId = resolveRunId();
      if (!teamIdTrimmed || !runId) {
        setRuntimeUpdateError("missing team_id or run id");
        return;
      }
      const normalizedMembers: TeamRunRuntimeMemberRow[] = [];
      for (const member of members) {
        const agentId = String(member?.agent_id || "").trim();
        const role = String(member?.role || "").trim();
        if (!agentId || !role) {
          setRuntimeUpdateError("runtime members missing agent_id or role");
          return;
        }
        const normalized: TeamRunRuntimeMemberRow = { agent_id: agentId, role };
        const memberId = String(member?.member_id || "").trim();
        if (memberId) normalized.member_id = memberId;
        const deploymentId = String(member?.deployment_id || "").trim();
        if (deploymentId) normalized.deployment_id = deploymentId;
        if (Array.isArray(member?.capabilities)) {
          normalized.capabilities = member.capabilities.map((item) => String(item).trim()).filter(Boolean);
        }
        const status = String(member?.status || "").trim();
        if (status) normalized.status = status;
        if (typeof member?.weight === "number" && Number.isFinite(member.weight)) {
          normalized.weight = member.weight;
        }
        const meta = asUnknownRecord(member?.meta);
        if (meta) normalized.meta = meta;
        normalizedMembers.push(normalized);
      }
      setRuntimeUpdateError(null);
      setRuntimeUpdateNote("");
      setRuntimeUpdateBusy(true);
      try {
        const resp = await apiBrokerTeamRunRuntimeMembersUpdate(
          base,
          teamIdTrimmed,
          runId,
          { mode, runtime_members: normalizedMembers },
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
          setRunRuntimeMembersJson(JSON.stringify(normalizedMembers, null, 2));
          setRuntimeUpdateNote(`updated ${normalizedMembers.length} runtime members`);
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
    let parsed: unknown = null;
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
    const mode = String(runtimeUpdateMode || "").trim().toLowerCase() === "merge" ? "merge" : "replace";
    await applyRuntimeMembersUpdate(parsed as RuntimeMemberDraft[], mode);
  }, [applyRuntimeMembersUpdate, runRuntimeMembersJson, runtimeUpdateMode]);

  const handleRuntimeMemberToggle = React.useCallback(
    async (member: TeamRunRuntimeMemberRow) => {
      const runtimeMembers = runLookupResult?.runtime_members ?? [];
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
      const updated = runtimeMembers.map((item) => {
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
    async (member: TeamRunRuntimeMemberRow) => {
      const runtimeMembers = runLookupResult?.runtime_members ?? [];
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
      const updated = runtimeMembers.filter((item) => {
        const existingMemberId = String(item?.member_id || "").trim();
        const existingAgentId = String(item?.agent_id || "").trim();
        if (memberId) return existingMemberId !== memberId;
        return existingAgentId !== agentId;
      });
      await applyRuntimeMembersUpdate(updated, "replace");
    },
    [applyRuntimeMembersUpdate, runLookupResult],
  );

  React.useEffect(() => {
    if (!teamIdTrimmed) return;
    setRunLookupResult(null);
    setRunLookupError(null);
    setRunLookupId("");
  }, [setRunLookupId, teamIdTrimmed]);

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
    runtimeUpdateMode,
    setRuntimeUpdateMode,
    runtimeUpdateBusy,
    runtimeUpdateError,
    runtimeUpdateNote,
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
    handleRuntimeMembersLoadFromRun,
    handleRuntimeMembersUpdate,
    handleRuntimeMemberToggle,
    handleRuntimeMemberRemove,
  };
}
