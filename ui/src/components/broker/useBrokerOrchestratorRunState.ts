import React from "react";
import {
  apiBrokerOrchestratorRunCreate,
  apiBrokerOrchestratorRunGet,
  apiBrokerOrchestratorRunHeartbeat,
  apiBrokerOrchestratorRunUpdate,
  apiBrokerOrchestratorRunsList,
  type ApiAuth,
  type BrokerOrchestratorRun,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import { fmtTs } from "./teamRunUtils";
import {
  diffKeyLabel,
  diffSummary,
  formatJson,
  matchesRevisionFilter,
  normalizeRevisionEntries,
  parseJsonField,
  revisionChangeLabels,
  revisionVersion,
  sortRevisions,
  toNumber,
} from "./brokerOrchestratorRunUtils";
import type { BrokerEventRow } from "./types";

type UseBrokerOrchestratorRunStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  teamMeta?: Record<string, unknown> | null;
  events?: BrokerEventRow[];
};

export default function useBrokerOrchestratorRunState(args: UseBrokerOrchestratorRunStateArgs) {
  const teamIdTrimmed = String(args.teamId || "").trim();
  const teamMetaObj = args.teamMeta && typeof args.teamMeta === "object" ? args.teamMeta : null;

  const [listBusy, setListBusy] = React.useState(false);
  const [listError, setListError] = React.useState<string | null>(null);
  const [runs, setRuns] = React.useState<BrokerOrchestratorRun[]>([]);
  const [statusFilter, setStatusFilter] = React.useState<string>("");

  const [createGoal, setCreateGoal] = React.useState<string>("");
  const [createStatus, setCreateStatus] = React.useState<string>("running");
  const [createGoalContractJson, setCreateGoalContractJson] = React.useState<string>("");
  const [createRolePlanJson, setCreateRolePlanJson] = React.useState<string>("");
  const [createMetaJson, setCreateMetaJson] = React.useState<string>("");
  const [createBusy, setCreateBusy] = React.useState<boolean>(false);
  const [createError, setCreateError] = React.useState<string | null>(null);
  const [createNote, setCreateNote] = React.useState<string | null>(null);

  const [runLookupByTeam, setRunLookupByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.orchestratorRunLookupByTeam",
    {},
  );
  const [runId, setRunIdState] = React.useState<string>("");
  const [runBusy, setRunBusy] = React.useState<boolean>(false);
  const [runError, setRunError] = React.useState<string | null>(null);
  const [runResult, setRunResult] = React.useState<BrokerOrchestratorRun | null>(null);
  const [showGoalRevisions, setShowGoalRevisions] = React.useState<boolean>(false);
  const [showRoleRevisions, setShowRoleRevisions] = React.useState<boolean>(false);
  const [showGoalContractJson, setShowGoalContractJson] = React.useState<boolean>(false);
  const [showRolePlanJson, setShowRolePlanJson] = React.useState<boolean>(false);
  const [copyNote, setCopyNote] = React.useState<string | null>(null);
  const [revisionFilterByTeam, setRevisionFilterByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.orchestratorRevisionFilterByTeam",
    {},
  );
  const [revisionScopeByTeam, setRevisionScopeByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.orchestratorRevisionScopeByTeam",
    {},
  );
  const [revisionFilter, setRevisionFilterState] = React.useState<string>("");
  const [revisionFilterScope, setRevisionFilterScope] = React.useState<"all" | "goal" | "role">("all");

  const [updateGoal, setUpdateGoal] = React.useState<string>("");
  const [updateStatus, setUpdateStatus] = React.useState<string>("");
  const [updateGoalContractJson, setUpdateGoalContractJson] = React.useState<string>("");
  const [updateRolePlanJson, setUpdateRolePlanJson] = React.useState<string>("");
  const [updateMetaJson, setUpdateMetaJson] = React.useState<string>("");
  const [updateExpectedOwner, setUpdateExpectedOwner] = React.useState<string>("");
  const [updateExpectedOwnerEmpty, setUpdateExpectedOwnerEmpty] = React.useState<boolean>(false);
  const [updateExpectedStatus, setUpdateExpectedStatus] = React.useState<string>("");
  const [updateBusy, setUpdateBusy] = React.useState<boolean>(false);
  const [updateError, setUpdateError] = React.useState<string | null>(null);
  const [updateNote, setUpdateNote] = React.useState<string | null>(null);

  const [heartbeatStatus, setHeartbeatStatus] = React.useState<string>("");
  const [heartbeatExpectedOwner, setHeartbeatExpectedOwner] = React.useState<string>("");
  const [heartbeatExpectedStatus, setHeartbeatExpectedStatus] = React.useState<string>("");
  const [heartbeatBusy, setHeartbeatBusy] = React.useState<boolean>(false);
  const [heartbeatError, setHeartbeatError] = React.useState<string | null>(null);
  const [heartbeatNote, setHeartbeatNote] = React.useState<string | null>(null);
  const [heartbeatAuto, setHeartbeatAuto] = useLocalStorageState<boolean>("agentui.orchestratorHeartbeatAuto", false);
  const [heartbeatIntervalMs, setHeartbeatIntervalMs] = useLocalStorageState<number>(
    "agentui.orchestratorHeartbeatMs",
    30000,
  );
  const lastEventKeyRef = React.useRef<string>("");

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRunIdState("");
      return;
    }
    setRunIdState(runLookupByTeam[teamIdTrimmed] || "");
  }, [runLookupByTeam, teamIdTrimmed]);

  React.useEffect(() => {
    setRunResult(null);
    setRunError(null);
  }, [teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRevisionFilterState("");
      return;
    }
    setRevisionFilterState(revisionFilterByTeam[teamIdTrimmed] || "");
  }, [revisionFilterByTeam, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRevisionFilterScope("all");
      return;
    }
    const raw = revisionScopeByTeam[teamIdTrimmed] || "all";
    if (raw === "goal" || raw === "role" || raw === "all") setRevisionFilterScope(raw);
    else setRevisionFilterScope("all");
  }, [revisionScopeByTeam, teamIdTrimmed]);

  const setRevisionFilter = React.useCallback(
    (next: string) => {
      setRevisionFilterState(next);
      if (!teamIdTrimmed) return;
      setRevisionFilterByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [setRevisionFilterByTeam, teamIdTrimmed],
  );

  const setRevisionScope = React.useCallback(
    (next: "all" | "goal" | "role") => {
      setRevisionFilterScope(next);
      if (!teamIdTrimmed) return;
      setRevisionScopeByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [setRevisionScopeByTeam, teamIdTrimmed],
  );

  const setRunId = React.useCallback(
    (next: string) => {
      setRunIdState(next);
      if (!teamIdTrimmed) return;
      setRunLookupByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [setRunLookupByTeam, teamIdTrimmed],
  );

  const loadRuns = React.useCallback(async () => {
    if (!args.canQuery || !teamIdTrimmed) return;
    setListBusy(true);
    setListError(null);
    try {
      const resp = await apiBrokerOrchestratorRunsList(args.base, teamIdTrimmed, args.auth, {
        limit: 25,
        status: statusFilter.trim() ? statusFilter.trim() : undefined,
      });
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "orchestrator runs failed");
      setRuns(Array.isArray(resp.runs) ? resp.runs : []);
    } catch (err) {
      setListError(String(err));
    } finally {
      setListBusy(false);
    }
  }, [args.auth, args.base, args.canQuery, statusFilter, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed || !args.canQuery) {
      setRuns([]);
      return;
    }
    void loadRuns();
  }, [args.canQuery, loadRuns, teamIdTrimmed]);

  const loadRun = React.useCallback(
    async (targetId?: string) => {
      const rid = String(targetId || runId || "").trim();
      if (!args.canQuery || !teamIdTrimmed || !rid) return;
      setRunBusy(true);
      setRunError(null);
      try {
        const resp = await apiBrokerOrchestratorRunGet(args.base, teamIdTrimmed, rid, args.auth);
        if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "orchestrator run lookup failed");
        setRunResult(resp.run ?? null);
      } catch (err) {
        setRunError(String(err));
      } finally {
        setRunBusy(false);
      }
    },
    [args.auth, args.base, args.canQuery, runId, teamIdTrimmed],
  );

  const handleCreate = React.useCallback(async () => {
    if (!args.canQuery || !teamIdTrimmed) return;
    setCreateBusy(true);
    setCreateError(null);
    setCreateNote(null);
    try {
      const goal = createGoal.trim();
      if (!goal) {
        setCreateError("goal required");
        return;
      }
      const goalContract = parseJsonField(createGoalContractJson, "goal_contract");
      if (!goalContract.ok) {
        setCreateError(goalContract.error);
        return;
      }
      const rolePlan = parseJsonField(createRolePlanJson, "role_plan_snapshot");
      if (!rolePlan.ok) {
        setCreateError(rolePlan.error);
        return;
      }
      const meta = parseJsonField(createMetaJson, "meta");
      if (!meta.ok) {
        setCreateError(meta.error);
        return;
      }
      const body: Record<string, any> = { goal };
      if (createStatus.trim()) body.status = createStatus.trim();
      if (goalContract.value !== undefined) body.goal_contract = goalContract.value;
      if (rolePlan.value !== undefined) body.role_plan_snapshot = rolePlan.value;
      if (meta.value !== undefined) body.meta = meta.value;
      const resp = await apiBrokerOrchestratorRunCreate(args.base, teamIdTrimmed, body, args.auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "create orchestrator run failed");
      const newRun = resp.run ?? null;
      if (newRun?.orchestrator_run_id) {
        setRunId(String(newRun.orchestrator_run_id));
        setRunResult(newRun);
        setCreateNote(`created ${newRun.orchestrator_run_id}`);
      }
      await loadRuns();
    } catch (err) {
      setCreateError(String(err));
    } finally {
      setCreateBusy(false);
    }
  }, [
    args.auth,
    args.base,
    args.canQuery,
    createGoal,
    createGoalContractJson,
    createMetaJson,
    createRolePlanJson,
    createStatus,
    loadRuns,
    teamIdTrimmed,
    setRunId,
  ]);

  const handleUpdate = React.useCallback(async () => {
    if (!args.canQuery || !teamIdTrimmed) return;
    const rid = String(runId || "").trim();
    if (!rid) {
      setUpdateError("run id required");
      return;
    }
    setUpdateBusy(true);
    setUpdateError(null);
    setUpdateNote(null);
    try {
      const goalContract = parseJsonField(updateGoalContractJson, "goal_contract");
      if (!goalContract.ok) {
        setUpdateError(goalContract.error);
        return;
      }
      const rolePlan = parseJsonField(updateRolePlanJson, "role_plan_snapshot");
      if (!rolePlan.ok) {
        setUpdateError(rolePlan.error);
        return;
      }
      const meta = parseJsonField(updateMetaJson, "meta");
      if (!meta.ok) {
        setUpdateError(meta.error);
        return;
      }
      const body: Record<string, any> = {};
      if (updateStatus.trim()) body.status = updateStatus.trim();
      if (updateGoal.trim()) body.goal = updateGoal.trim();
      if (goalContract.value !== undefined) body.goal_contract = goalContract.value;
      if (rolePlan.value !== undefined) body.role_plan_snapshot = rolePlan.value;
      if (meta.value !== undefined) body.meta = meta.value;
      if (updateExpectedOwnerEmpty) body.expected_owner = "";
      else if (updateExpectedOwner.trim()) body.expected_owner = updateExpectedOwner.trim();
      if (updateExpectedStatus.trim()) body.expected_status = updateExpectedStatus.trim();
      if (Object.keys(body).length === 0) {
        setUpdateError("no update fields set");
        return;
      }
      const resp = await apiBrokerOrchestratorRunUpdate(args.base, teamIdTrimmed, rid, body, args.auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "update orchestrator run failed");
      setRunResult(resp.run ?? null);
      setUpdateNote("updated");
      await loadRuns();
    } catch (err) {
      setUpdateError(String(err));
    } finally {
      setUpdateBusy(false);
    }
  }, [
    args.auth,
    args.base,
    args.canQuery,
    loadRuns,
    runId,
    teamIdTrimmed,
    updateExpectedOwner,
    updateExpectedOwnerEmpty,
    updateExpectedStatus,
    updateGoal,
    updateGoalContractJson,
    updateMetaJson,
    updateRolePlanJson,
    updateStatus,
  ]);

  const handleHeartbeat = React.useCallback(async () => {
    if (!args.canQuery || !teamIdTrimmed) return;
    const rid = String(runId || "").trim();
    if (!rid) {
      setHeartbeatError("run id required");
      return;
    }
    setHeartbeatBusy(true);
    setHeartbeatError(null);
    setHeartbeatNote(null);
    try {
      const body: Record<string, any> = {};
      if (heartbeatStatus.trim()) body.status = heartbeatStatus.trim();
      if (heartbeatExpectedOwner.trim()) body.expected_owner = heartbeatExpectedOwner.trim();
      if (heartbeatExpectedStatus.trim()) body.expected_status = heartbeatExpectedStatus.trim();
      const resp = await apiBrokerOrchestratorRunHeartbeat(args.base, teamIdTrimmed, rid, body, args.auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "heartbeat failed");
      setRunResult(resp.run ?? null);
      setHeartbeatNote("heartbeat ok");
      await loadRuns();
    } catch (err) {
      setHeartbeatError(String(err));
    } finally {
      setHeartbeatBusy(false);
    }
  }, [
    args.auth,
    args.base,
    args.canQuery,
    heartbeatExpectedOwner,
    heartbeatExpectedStatus,
    heartbeatStatus,
    loadRuns,
    runId,
    teamIdTrimmed,
  ]);

  React.useEffect(() => {
    if (!heartbeatAuto || !args.canQuery) return;
    const rid = String(runId || "").trim();
    if (!rid) return;
    const interval = Math.max(5000, Number(heartbeatIntervalMs) || 30000);
    const timer = setInterval(() => {
      void handleHeartbeat();
    }, interval);
    return () => clearInterval(timer);
  }, [args.canQuery, handleHeartbeat, heartbeatAuto, heartbeatIntervalMs, runId]);

  React.useEffect(() => {
    if (!args.canQuery || !teamIdTrimmed) return;
    const rows = Array.isArray(args.events) ? args.events : [];
    if (rows.length === 0) return;
    const last = rows[rows.length - 1];
    const key = last?.event_id || `${last?.type || ""}:${last?.ts_unix_ms || 0}:${last?.trace_id || ""}`;
    if (!key || lastEventKeyRef.current === key) return;
    lastEventKeyRef.current = key;
    void loadRuns();
    const rid = last?.payload?.orchestrator_run_id ? String(last.payload.orchestrator_run_id) : "";
    if (rid && rid === String(runId || "").trim()) void loadRun(rid);
  }, [args.canQuery, args.events, loadRun, loadRuns, runId, teamIdTrimmed]);

  React.useEffect(() => {
    if (!copyNote) return;
    const timer = setTimeout(() => setCopyNote(null), 2500);
    return () => clearTimeout(timer);
  }, [copyNote]);

  const handleCopyJson = React.useCallback(async (payload: any, label: string) => {
    const text = formatJson(payload);
    if (!text) {
      setCopyNote(`${label} empty`);
      return;
    }
    try {
      if (navigator?.clipboard?.writeText) {
        await navigator.clipboard.writeText(text);
        setCopyNote(`${label} copied`);
      } else {
        setCopyNote("clipboard unavailable");
      }
    } catch (err) {
      setCopyNote(`copy failed: ${String(err)}`);
    }
  }, []);

  const currentRun = runResult;
  const currentMeta = currentRun && typeof currentRun.meta === "object" ? currentRun.meta : null;
  const goalRevisions = sortRevisions(normalizeRevisionEntries(currentMeta?.goal_versions));
  const rolePlanRevisions = sortRevisions(normalizeRevisionEntries(currentMeta?.role_plan_versions));
  const latestGoalRevision = goalRevisions.length > 0 ? goalRevisions[0] : null;
  const latestRoleRevision = rolePlanRevisions.length > 0 ? rolePlanRevisions[0] : null;
  const goalVersion = toNumber(currentMeta?.goal_version);
  const rolePlanVersion = toNumber(currentMeta?.role_plan_version);
  const goalUpdatedAt = toNumber(currentMeta?.goal_updated_unix_ms);
  const rolePlanUpdatedAt = toNumber(currentMeta?.role_plan_updated_unix_ms);
  const revisionSummary = React.useMemo(() => {
    const parts: string[] = [];
    if (goalVersion) parts.push(`goal v${goalVersion}`);
    if (goalUpdatedAt) parts.push(`goal updated ${fmtTs(goalUpdatedAt)}`);
    if (rolePlanVersion) parts.push(`role plan v${rolePlanVersion}`);
    if (rolePlanUpdatedAt) parts.push(`role updated ${fmtTs(rolePlanUpdatedAt)}`);
    return parts.join(" · ");
  }, [goalUpdatedAt, goalVersion, rolePlanUpdatedAt, rolePlanVersion]);
  const latestGoalContract =
    latestGoalRevision?.goal_contract !== undefined ? latestGoalRevision.goal_contract : currentRun?.goal_contract ?? undefined;
  const latestRolePlanSnapshot =
    latestRoleRevision?.role_plan_snapshot !== undefined
      ? latestRoleRevision.role_plan_snapshot
      : currentRun?.role_plan_snapshot ?? undefined;
  const latestGoalChangeSummary = React.useMemo(() => {
    if (!latestGoalRevision) return "";
    const parts: string[] = [];
    if (latestGoalRevision.goal_changed === true) parts.push("goal changed");
    if (latestGoalRevision.goal_contract_changed === true) parts.push("goal contract changed");
    return parts.join(" · ");
  }, [latestGoalRevision]);

  const revisionFilterLower = revisionFilter.trim().toLowerCase();
  const revisionMatches = React.useCallback(
    (entry?: Record<string, any> | null) => {
      if (!entry) return false;
      return matchesRevisionFilter(revisionFilterLower, {
        version: entry.version,
        updatedBy: entry.updated_by,
        goal: entry.goal,
        diffs: [entry.goal_contract_diff, entry.role_plan_diff],
        changeLabels: revisionChangeLabels(entry.goal_changed, entry.goal_contract_changed),
      });
    },
    [revisionFilterLower],
  );
  const scopedGoalRevisions = revisionFilterScope === "role" ? [] : goalRevisions;
  const scopedRoleRevisions = revisionFilterScope === "goal" ? [] : rolePlanRevisions;
  const filteredGoalRevisions = revisionFilterLower
    ? scopedGoalRevisions.filter((entry) => revisionMatches(entry))
    : scopedGoalRevisions;
  const filteredRoleRevisions = revisionFilterLower
    ? scopedRoleRevisions.filter((entry) => revisionMatches(entry))
    : scopedRoleRevisions;
  const totalFiltered = filteredGoalRevisions.length + filteredRoleRevisions.length;
  const totalRevisions = scopedGoalRevisions.length + scopedRoleRevisions.length;
  const revisionEvents = React.useMemo(() => {
    const rows = Array.isArray(args.events) ? args.events : [];
    if (rows.length === 0) return [];
    const rid = String(runId || "").trim();
    const out = rows.filter((row) => {
      const type = String(row?.type || "");
      if (type !== "orchestrator_goal_revision" && type !== "orchestrator_role_plan_revision") return false;
      if (revisionFilterScope === "goal" && type !== "orchestrator_goal_revision") return false;
      if (revisionFilterScope === "role" && type !== "orchestrator_role_plan_revision") return false;
      const payload = row?.payload;
      if (!payload || typeof payload !== "object") return false;
      if (teamIdTrimmed && String(payload.team_id || "") !== teamIdTrimmed) return false;
      if (rid && String(payload.orchestrator_run_id || "") !== rid) return false;
      const isGoal = type === "orchestrator_goal_revision";
      const diff = isGoal ? payload.goal_contract_diff : payload.role_plan_diff;
      return matchesRevisionFilter(revisionFilterLower, {
        version: payload.version,
        updatedBy: payload.updated_by,
        goal: payload.goal,
        diffs: [diff],
        changeLabels: isGoal ? revisionChangeLabels(payload.goal_changed, payload.goal_contract_changed) : [],
      });
    });
    out.sort((a, b) => (b.ts_unix_ms || 0) - (a.ts_unix_ms || 0));
    return out;
  }, [args.events, revisionFilterLower, revisionFilterScope, runId, teamIdTrimmed]);
  const revisionEventsTotal = revisionEvents.length;
  const revisionEventsDisplay = revisionEvents.slice(0, 8);
  const revisionEventsOverflow = revisionEventsTotal > revisionEventsDisplay.length;
  const goalRevisionEmptyLabel =
    revisionFilterScope === "role"
      ? "Hidden by scope filter."
      : revisionFilterLower
        ? goalRevisions.length > 0
          ? "No goal revisions match filter."
          : "No goal revisions."
        : "No goal revisions.";
  const roleRevisionEmptyLabel =
    revisionFilterScope === "goal"
      ? "Hidden by scope filter."
      : revisionFilterLower
        ? rolePlanRevisions.length > 0
          ? "No role plan revisions match filter."
          : "No role plan revisions."
        : "No role plan revisions.";
  const revisionEventsEmptyLabel = revisionFilterLower
    ? "No revision events match filter."
    : revisionFilterScope === "goal"
      ? "No goal revision events."
      : revisionFilterScope === "role"
        ? "No role plan revision events."
        : "No revision events yet.";

  const currentOwner = typeof currentMeta?.orchestrator_owner === "string" ? currentMeta.orchestrator_owner : "";
  const prevOwner =
    typeof currentMeta?.orchestrator_owner_prev === "string" ? currentMeta.orchestrator_owner_prev : "";
  const allowTakeover =
    currentMeta?.allow_takeover === undefined
      ? "default"
      : currentMeta.allow_takeover === true
        ? "true"
        : "false";
  const rolePlanDefaults =
    teamMetaObj && ("role_graph" in teamMetaObj || "role_instructions" in teamMetaObj) ? "team meta" : "none";

  return {
    teamIdTrimmed,
    listBusy,
    listError,
    runs,
    statusFilter,
    setStatusFilter,
    createGoal,
    setCreateGoal,
    createStatus,
    setCreateStatus,
    createGoalContractJson,
    setCreateGoalContractJson,
    createRolePlanJson,
    setCreateRolePlanJson,
    createMetaJson,
    setCreateMetaJson,
    createBusy,
    createError,
    createNote,
    runId,
    setRunId,
    runBusy,
    runError,
    currentRun,
    revisionSummary,
    currentOwner,
    prevOwner,
    allowTakeover,
    rolePlanDefaults,
    showGoalRevisions,
    setShowGoalRevisions,
    showRoleRevisions,
    setShowRoleRevisions,
    showGoalContractJson,
    setShowGoalContractJson,
    showRolePlanJson,
    setShowRolePlanJson,
    copyNote,
    revisionFilter,
    setRevisionFilter,
    revisionFilterScope,
    setRevisionScope,
    filteredGoalRevisions,
    filteredRoleRevisions,
    scopedGoalRevisions,
    scopedRoleRevisions,
    totalFiltered,
    totalRevisions,
    latestGoalRevision,
    latestRoleRevision,
    latestGoalContract,
    latestRolePlanSnapshot,
    latestGoalChangeSummary,
    revisionEventsDisplay,
    revisionEventsTotal,
    revisionEventsOverflow,
    goalRevisionEmptyLabel,
    roleRevisionEmptyLabel,
    revisionEventsEmptyLabel,
    updateGoal,
    setUpdateGoal,
    updateStatus,
    setUpdateStatus,
    updateGoalContractJson,
    setUpdateGoalContractJson,
    updateRolePlanJson,
    setUpdateRolePlanJson,
    updateMetaJson,
    setUpdateMetaJson,
    updateExpectedOwner,
    setUpdateExpectedOwner,
    updateExpectedOwnerEmpty,
    setUpdateExpectedOwnerEmpty,
    updateExpectedStatus,
    setUpdateExpectedStatus,
    updateBusy,
    updateError,
    updateNote,
    heartbeatStatus,
    setHeartbeatStatus,
    heartbeatExpectedOwner,
    setHeartbeatExpectedOwner,
    heartbeatExpectedStatus,
    setHeartbeatExpectedStatus,
    heartbeatBusy,
    heartbeatError,
    heartbeatNote,
    heartbeatAuto,
    setHeartbeatAuto,
    heartbeatIntervalMs,
    setHeartbeatIntervalMs,
    loadRuns,
    loadRun,
    handleCreate,
    handleUpdate,
    handleHeartbeat,
    handleCopyJson,
  };
}

export type BrokerOrchestratorRunState = ReturnType<typeof useBrokerOrchestratorRunState>;
