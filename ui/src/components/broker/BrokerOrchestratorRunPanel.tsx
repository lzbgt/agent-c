import React from "react";
import {
  apiBrokerOrchestratorRunCreate,
  apiBrokerOrchestratorRunGet,
  apiBrokerOrchestratorRunHeartbeat,
  apiBrokerOrchestratorRunUpdate,
  apiBrokerOrchestratorRunsList,
  type ApiAuth,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import FieldLabel from "../FieldLabel";
import { fmtTs } from "./teamRunUtils";

type OrchestratorRunPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  teamMeta?: Record<string, any> | null;
  events?: Array<{ type?: string; ts_unix_ms?: number; event_id?: string; trace_id?: string; payload?: any }>;
};

type ParsedJson = { ok: true; value: any } | { ok: false; error: string };

const parseJsonField = (raw: string, label: string): ParsedJson => {
  const trimmed = String(raw || "").trim();
  if (!trimmed) return { ok: true, value: undefined };
  try {
    return { ok: true, value: JSON.parse(trimmed) };
  } catch (err) {
    return { ok: false, error: `${label} JSON invalid: ${String(err)}` };
  }
};

const fmtAge = (ms?: number | null) => {
  if (typeof ms !== "number" || !Number.isFinite(ms) || ms < 0) return "";
  if (ms < 1000) return `${Math.round(ms)}ms`;
  if (ms < 60000) return `${Math.round(ms / 100) / 10}s`;
  if (ms < 3600000) return `${Math.round(ms / 6000) / 10}m`;
  return `${Math.round(ms / 360000) / 10}h`;
};

const toNumber = (val: any): number | null => {
  if (typeof val === "number" && Number.isFinite(val)) return val;
  if (typeof val === "string" && val.trim()) {
    const parsed = Number.parseFloat(val);
    if (Number.isFinite(parsed)) return parsed;
  }
  return null;
};

const normalizeRevisionEntries = (raw: any): Array<Record<string, any>> => {
  if (!Array.isArray(raw)) return [];
  return raw.filter((item) => item && typeof item === "object") as Array<Record<string, any>>;
};

const revisionVersion = (entry?: Record<string, any> | null): number => {
  if (!entry) return 0;
  const val = toNumber(entry.version);
  return val ? val : 0;
};

const sortRevisions = (entries: Array<Record<string, any>>): Array<Record<string, any>> => {
  if (entries.length <= 1) return entries;
  const copy = [...entries];
  copy.sort((a, b) => revisionVersion(b) - revisionVersion(a));
  return copy;
};

const diffSummary = (diff: any): string => {
  if (!diff || typeof diff !== "object") return "";
  const parts: string[] = [];
  const added = Array.isArray(diff.added) ? diff.added.length : 0;
  const removed = Array.isArray(diff.removed) ? diff.removed.length : 0;
  const changed = Array.isArray(diff.changed) ? diff.changed.length : 0;
  if (added) parts.push(`+${added}`);
  if (removed) parts.push(`-${removed}`);
  if (changed) parts.push(`~${changed}`);
  return parts.join(" ");
};

const diffKeys = (diff: any): string[] => {
  if (!diff || typeof diff !== "object") return [];
  const out: string[] = [];
  if (Array.isArray(diff.added)) out.push(...diff.added.map((v: any) => String(v)));
  if (Array.isArray(diff.removed)) out.push(...diff.removed.map((v: any) => String(v)));
  if (Array.isArray(diff.changed)) out.push(...diff.changed.map((v: any) => String(v)));
  return out;
};

const formatJson = (value: any): string => {
  if (value === undefined) return "";
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return "";
  }
};

export default function BrokerOrchestratorRunPanel(props: OrchestratorRunPanelProps) {
  const teamIdTrimmed = String(props.teamId || "").trim();
  const teamMetaObj = props.teamMeta && typeof props.teamMeta === "object" ? props.teamMeta : null;
  const [listBusy, setListBusy] = React.useState(false);
  const [listError, setListError] = React.useState<string | null>(null);
  const [runs, setRuns] = React.useState<any[]>([]);
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
  const [runResult, setRunResult] = React.useState<any | null>(null);
  const [showGoalRevisions, setShowGoalRevisions] = React.useState<boolean>(false);
  const [showRoleRevisions, setShowRoleRevisions] = React.useState<boolean>(false);
  const [showGoalContractJson, setShowGoalContractJson] = React.useState<boolean>(false);
  const [showRolePlanJson, setShowRolePlanJson] = React.useState<boolean>(false);
  const [copyNote, setCopyNote] = React.useState<string | null>(null);
  const [revisionFilterByTeam, setRevisionFilterByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.orchestratorRevisionFilterByTeam",
    {},
  );
  const [revisionFilter, setRevisionFilterState] = React.useState<string>("");

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
    const next = runLookupByTeam[teamIdTrimmed] || "";
    setRunIdState(next);
  }, [teamIdTrimmed, runLookupByTeam]);

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setRevisionFilterState("");
      return;
    }
    const next = revisionFilterByTeam[teamIdTrimmed] || "";
    setRevisionFilterState(next);
  }, [revisionFilterByTeam, teamIdTrimmed]);

  const setRevisionFilter = React.useCallback(
    (next: string) => {
      setRevisionFilterState(next);
      if (!teamIdTrimmed) return;
      setRevisionFilterByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [setRevisionFilterByTeam, teamIdTrimmed],
  );

  const setRunId = React.useCallback(
    (next: string) => {
      setRunIdState(next);
      if (!teamIdTrimmed) return;
      setRunLookupByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [teamIdTrimmed, setRunLookupByTeam],
  );

  const loadRuns = React.useCallback(async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
    setListBusy(true);
    setListError(null);
    try {
      const resp = await apiBrokerOrchestratorRunsList(props.base, teamIdTrimmed, props.auth, {
        limit: 25,
        status: statusFilter.trim() ? statusFilter.trim() : undefined,
      });
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "orchestrator runs failed");
      }
      setRuns(Array.isArray(resp.runs) ? resp.runs : []);
    } catch (err) {
      setListError(String(err));
    } finally {
      setListBusy(false);
    }
  }, [props.base, props.auth, props.canQuery, statusFilter, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed || !props.canQuery) {
      setRuns([]);
      return;
    }
    void loadRuns();
  }, [teamIdTrimmed, props.canQuery, loadRuns]);

  const loadRun = React.useCallback(
    async (targetId?: string) => {
      const rid = String(targetId || runId || "").trim();
      if (!props.canQuery || !teamIdTrimmed || !rid) return;
      setRunBusy(true);
      setRunError(null);
      try {
        const resp = await apiBrokerOrchestratorRunGet(props.base, teamIdTrimmed, rid, props.auth);
        if (!resp.ok) {
          throw new Error(resp.error || resp.err || resp.code || "orchestrator run lookup failed");
        }
        setRunResult(resp.run ?? null);
      } catch (err) {
        setRunError(String(err));
      } finally {
        setRunBusy(false);
      }
    },
    [props.base, props.auth, props.canQuery, runId, teamIdTrimmed],
  );

  const handleCreate = async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
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
      const resp = await apiBrokerOrchestratorRunCreate(props.base, teamIdTrimmed, body, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "create orchestrator run failed");
      }
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
  };

  const handleUpdate = async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
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
      if (updateExpectedOwnerEmpty) {
        body.expected_owner = "";
      } else if (updateExpectedOwner.trim()) {
        body.expected_owner = updateExpectedOwner.trim();
      }
      if (updateExpectedStatus.trim()) body.expected_status = updateExpectedStatus.trim();
      if (Object.keys(body).length === 0) {
        setUpdateError("no update fields set");
        return;
      }
      const resp = await apiBrokerOrchestratorRunUpdate(props.base, teamIdTrimmed, rid, body, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "update orchestrator run failed");
      }
      setRunResult(resp.run ?? null);
      setUpdateNote("updated");
      await loadRuns();
    } catch (err) {
      setUpdateError(String(err));
    } finally {
      setUpdateBusy(false);
    }
  };

  const handleHeartbeat = async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
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
      const resp = await apiBrokerOrchestratorRunHeartbeat(props.base, teamIdTrimmed, rid, body, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "heartbeat failed");
      }
      setRunResult(resp.run ?? null);
      setHeartbeatNote("heartbeat ok");
      await loadRuns();
    } catch (err) {
      setHeartbeatError(String(err));
    } finally {
      setHeartbeatBusy(false);
    }
  };

  React.useEffect(() => {
    if (!heartbeatAuto || !props.canQuery) return;
    const rid = String(runId || "").trim();
    if (!rid) return;
    const interval = Math.max(5000, Number(heartbeatIntervalMs) || 30000);
    const timer = setInterval(() => {
      void handleHeartbeat();
    }, interval);
    return () => clearInterval(timer);
  }, [heartbeatAuto, heartbeatIntervalMs, props.canQuery, runId]);

  React.useEffect(() => {
    if (!props.canQuery || !teamIdTrimmed) return;
    const rows = Array.isArray(props.events) ? props.events : [];
    if (rows.length === 0) return;
    const last = rows[rows.length - 1];
    const key = last?.event_id || `${last?.type || ""}:${last?.ts_unix_ms || 0}:${last?.trace_id || ""}`;
    if (!key || lastEventKeyRef.current === key) return;
    lastEventKeyRef.current = key;
    void loadRuns();
    const rid = last?.payload?.orchestrator_run_id ? String(last.payload.orchestrator_run_id) : "";
    if (rid && rid === String(runId || "").trim()) {
      void loadRun(rid);
    }
  }, [props.canQuery, props.events, teamIdTrimmed, runId, loadRuns, loadRun]);

  const handleCopyJson = async (payload: any, label: string) => {
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
  };

  const currentRun = runResult;
  const currentMeta = currentRun && typeof currentRun.meta === "object" ? currentRun.meta : null;
  const goalRevisions = sortRevisions(normalizeRevisionEntries(currentMeta?.goal_versions));
  const rolePlanRevisions = sortRevisions(normalizeRevisionEntries(currentMeta?.role_plan_versions));
  const latestGoalRevision = goalRevisions.length > 0 ? goalRevisions[0] : null;
  const latestRoleRevision = rolePlanRevisions.length > 0 ? rolePlanRevisions[0] : null;
  const latestGoalContract =
    latestGoalRevision?.goal_contract !== undefined
      ? latestGoalRevision.goal_contract
      : currentRun?.goal_contract ?? undefined;
  const latestRolePlanSnapshot =
    latestRoleRevision?.role_plan_snapshot !== undefined
      ? latestRoleRevision.role_plan_snapshot
      : currentRun?.role_plan_snapshot ?? undefined;
  const revisionFilterLower = revisionFilter.trim().toLowerCase();
  const revisionMatches = React.useCallback(
    (entry?: Record<string, any> | null) => {
      if (!revisionFilterLower) return true;
      if (!entry) return false;
      const version = entry.version !== undefined ? String(entry.version).toLowerCase() : "";
      const updatedBy = entry.updated_by ? String(entry.updated_by).toLowerCase() : "";
      const goal = entry.goal ? String(entry.goal).toLowerCase() : "";
      const contractDiffKeys = diffKeys(entry.goal_contract_diff).map((v) => v.toLowerCase());
      const roleDiffKeys = diffKeys(entry.role_plan_diff).map((v) => v.toLowerCase());
      const matchesDiffKey =
        contractDiffKeys.some((key) => key.includes(revisionFilterLower)) ||
        roleDiffKeys.some((key) => key.includes(revisionFilterLower));
      return (
        (version && version.includes(revisionFilterLower)) ||
        (updatedBy && updatedBy.includes(revisionFilterLower)) ||
        (goal && goal.includes(revisionFilterLower)) ||
        matchesDiffKey
      );
    },
    [revisionFilterLower],
  );
  const filteredGoalRevisions = revisionFilterLower
    ? goalRevisions.filter((entry) => revisionMatches(entry))
    : goalRevisions;
  const filteredRoleRevisions = revisionFilterLower
    ? rolePlanRevisions.filter((entry) => revisionMatches(entry))
    : rolePlanRevisions;
  const revisionEvents = React.useMemo(() => {
    const rows = Array.isArray(props.events) ? props.events : [];
    if (rows.length === 0) return [];
    const rid = String(runId || "").trim();
    const out = rows.filter((row) => {
      const type = String(row?.type || "");
      if (type !== "orchestrator_goal_revision" && type !== "orchestrator_role_plan_revision") return false;
      const payload = row?.payload;
      if (!payload || typeof payload !== "object") return false;
      if (teamIdTrimmed && String(payload.team_id || "") !== teamIdTrimmed) return false;
      if (rid && String(payload.orchestrator_run_id || "") !== rid) return false;
      return true;
    });
    out.sort((a, b) => (b.ts_unix_ms || 0) - (a.ts_unix_ms || 0));
    return out.slice(0, 8);
  }, [props.events, runId, teamIdTrimmed]);
  const currentOwner = currentMeta?.orchestrator_owner ? String(currentMeta.orchestrator_owner) : "";
  const prevOwner = currentMeta?.orchestrator_owner_prev ? String(currentMeta.orchestrator_owner_prev) : "";
  const allowTakeover =
    currentMeta?.allow_takeover === undefined ? "default" : currentMeta.allow_takeover ? "true" : "false";
  const rolePlanDefaults = teamMetaObj?.role_graph || teamMetaObj?.role_instructions ? "team meta" : "none";

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 text-xs font-semibold text-white/80">Orchestrator runs</div>
      <div className="grid gap-3">
        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Create orchestrator run</div>
          <div className="grid gap-1">
            <FieldLabel>Goal</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={createGoal}
              onChange={(e) => setCreateGoal(e.target.value)}
            />
          </div>
          <div className="grid gap-2 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Status</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={createStatus}
                onChange={(e) => setCreateStatus(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Role plan snapshot (JSON, optional)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                placeholder={rolePlanDefaults === "none" ? "{}" : "// default from team meta"}
                value={createRolePlanJson}
                onChange={(e) => setCreateRolePlanJson(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Goal contract (JSON, optional)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                placeholder='{"success_criteria":["..."]}'
                value={createGoalContractJson}
                onChange={(e) => setCreateGoalContractJson(e.target.value)}
              />
            </div>
          </div>
          <div className="grid gap-1">
            <FieldLabel>Meta (JSON, optional)</FieldLabel>
            <textarea
              className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={createMetaJson}
              onChange={(e) => setCreateMetaJson(e.target.value)}
            />
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || createBusy}
              onClick={() => void handleCreate()}
            >
              {createBusy ? "Creating…" : "Create run"}
            </button>
            {createNote ? <div className="text-[11px] text-emerald-200">{createNote}</div> : null}
          </div>
          {createError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {createError}
            </div>
          ) : null}
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div className="text-[11px] text-white/60">Recent runs</div>
            <div className="flex items-center gap-2">
              <input
                className="w-32 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/90"
                placeholder="status filter"
                value={statusFilter}
                onChange={(e) => setStatusFilter(e.target.value)}
              />
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                disabled={!props.canQuery || listBusy}
                onClick={() => void loadRuns()}
              >
                {listBusy ? "Loading…" : "Refresh"}
              </button>
            </div>
          </div>
          {listError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {listError}
            </div>
          ) : null}
          {runs.length === 0 ? (
            <div className="text-[11px] text-white/50">No orchestrator runs.</div>
          ) : (
            <div className="grid gap-2">
              {runs.map((run) => {
                const rid = String(run?.orchestrator_run_id || "");
                return (
                  <button
                    key={rid}
                    className={`flex flex-wrap items-center justify-between gap-2 rounded-md border px-2 py-1 text-[11px] ${
                      rid === runId
                        ? "border-emerald-400/40 bg-emerald-500/10 text-emerald-100"
                        : "border-white/10 bg-black/40 text-white/70 hover:bg-black/30"
                    }`}
                    type="button"
                    onClick={() => {
                      setRunId(rid);
                      void loadRun(rid);
                    }}
                  >
                    <span>{rid || "run"}</span>
                    <span className="text-[11px] text-white/60">
                      {run?.status ? run.status : "unknown"}
                      {run?.created_unix_ms ? ` · ${fmtTs(run.created_unix_ms)}` : ""}
                    </span>
                  </button>
                );
              })}
            </div>
          )}
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div className="text-[11px] text-white/60">Run details</div>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              disabled={!props.canQuery || runBusy || !runId}
              onClick={() => void loadRun()}
            >
              {runBusy ? "Loading…" : "Load"}
            </button>
          </div>
          <div className="grid gap-1">
            <FieldLabel>Run id</FieldLabel>
            <input
              className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={runId}
              onChange={(e) => setRunId(e.target.value)}
            />
          </div>
          {runError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {runError}
            </div>
          ) : null}
          {currentRun ? (
            <div className="rounded-md border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/70">
              <div className="text-white/80">{currentRun.goal || "goal"}</div>
              <div className="text-[11px] text-white/50">
                {currentRun.status || "status"}
                {currentRun.updated_unix_ms ? ` · updated ${fmtTs(currentRun.updated_unix_ms)}` : ""}
                {currentRun.last_heartbeat_unix_ms ? ` · heartbeat ${fmtTs(currentRun.last_heartbeat_unix_ms)}` : ""}
                {currentRun.lease_status ? ` · lease ${currentRun.lease_status}` : ""}
                {currentRun.heartbeat_age_ms !== undefined && currentRun.heartbeat_age_ms !== null
                  ? ` · hb age ${fmtAge(Number(currentRun.heartbeat_age_ms))}`
                  : ""}
                {currentRun.lease_timeout_ms !== undefined && currentRun.lease_timeout_ms !== null
                  ? ` · lease timeout ${fmtAge(Number(currentRun.lease_timeout_ms))}`
                  : ""}
              </div>
              <div className="text-[11px] text-white/50">
                {currentOwner ? `owner ${currentOwner}` : "owner unclaimed"}
                {prevOwner ? ` · prev ${prevOwner}` : ""}
                {allowTakeover ? ` · allow_takeover ${allowTakeover}` : ""}
              </div>
            </div>
          ) : (
            <div className="text-[11px] text-white/50">No run loaded.</div>
          )}
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Revision history</div>
          <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <span>Filter</span>
            <input
              className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-[11px] text-white/90"
              placeholder="version / updated_by / goal / diff key"
              value={revisionFilter}
              onChange={(e) => setRevisionFilter(e.target.value)}
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => setRevisionFilter("")}
              disabled={!revisionFilter.trim()}
            >
              Clear
            </button>
          </div>
          <div className="grid gap-3 md:grid-cols-2">
            <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
              <div className="flex items-center justify-between gap-2">
                <div className="text-[11px] text-white/70">Goal revisions</div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowGoalRevisions((prev) => !prev)}
                >
                  {showGoalRevisions ? "Hide" : "Show"}
                </button>
              </div>
              {filteredGoalRevisions.length === 0 ? (
                <div className="text-[11px] text-white/50">No goal revisions.</div>
              ) : showGoalRevisions ? (
                <div className="grid gap-2">
                  {filteredGoalRevisions.map((entry, idx) => {
                    const version = revisionVersion(entry);
                    const updated = toNumber(entry.updated_unix_ms);
                    const diff = entry.goal_contract_diff || null;
                    const summary = diffSummary(diff);
                    return (
                      <div
                        key={`goal-rev-${version}-${idx}`}
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                      >
                        <details className="group">
                          <summary className="cursor-pointer text-white/80">
                            v{version || "?"}
                            {updated ? ` · ${fmtTs(updated)}` : ""}
                            {entry.updated_by ? ` · ${String(entry.updated_by)}` : ""}
                          </summary>
                          <div className="mt-1 grid gap-2">
                            {entry.goal ? <div className="text-white/70">{String(entry.goal)}</div> : null}
                            {summary ? <div className="text-white/50">diff {summary}</div> : null}
                            <div className="flex flex-wrap items-center gap-2">
                              {entry.goal ? (
                                <button
                                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                                  type="button"
                                  onClick={() => void handleCopyJson(entry.goal, "goal")}
                                >
                                  Copy goal
                                </button>
                              ) : null}
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopyJson(entry.goal_contract, "goal contract")}
                              >
                                Copy contract
                              </button>
                              {diff ? (
                                <button
                                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                                  type="button"
                                  onClick={() => void handleCopyJson(diff, "goal contract diff")}
                                >
                                  Copy diff
                                </button>
                              ) : null}
                            </div>
                            <pre className="max-h-52 overflow-auto rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[10px] text-white/70">
                              {formatJson(entry.goal_contract) || "// empty"}
                            </pre>
                          </div>
                        </details>
                      </div>
                    );
                  })}
                </div>
              ) : (
                <div className="text-[11px] text-white/50">
                  Latest v{revisionVersion(latestGoalRevision) || "?"}
                  {latestGoalRevision && toNumber(latestGoalRevision.updated_unix_ms)
                    ? ` · ${fmtTs(Number(latestGoalRevision.updated_unix_ms))}`
                    : ""}
                </div>
              )}
            </div>

            <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
              <div className="flex items-center justify-between gap-2">
                <div className="text-[11px] text-white/70">Role plan revisions</div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowRoleRevisions((prev) => !prev)}
                >
                  {showRoleRevisions ? "Hide" : "Show"}
                </button>
              </div>
              {filteredRoleRevisions.length === 0 ? (
                <div className="text-[11px] text-white/50">No role plan revisions.</div>
              ) : showRoleRevisions ? (
                <div className="grid gap-2">
                  {filteredRoleRevisions.map((entry, idx) => {
                    const version = revisionVersion(entry);
                    const updated = toNumber(entry.updated_unix_ms);
                    const summary = diffSummary(entry.role_plan_diff);
                    return (
                      <div
                        key={`role-rev-${version}-${idx}`}
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                      >
                        <details className="group">
                          <summary className="cursor-pointer text-white/80">
                            v{version || "?"}
                            {updated ? ` · ${fmtTs(updated)}` : ""}
                            {entry.updated_by ? ` · ${String(entry.updated_by)}` : ""}
                          </summary>
                          <div className="mt-1 grid gap-2">
                            {summary ? <div className="text-white/50">diff {summary}</div> : null}
                            <div className="flex flex-wrap items-center gap-2">
                              <button
                                className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                                type="button"
                                onClick={() => void handleCopyJson(entry.role_plan_snapshot, "role plan snapshot")}
                              >
                                Copy snapshot
                              </button>
                              {entry.role_plan_diff ? (
                                <button
                                  className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                                  type="button"
                                  onClick={() => void handleCopyJson(entry.role_plan_diff, "role plan diff")}
                                >
                                  Copy diff
                                </button>
                              ) : null}
                            </div>
                            <pre className="max-h-52 overflow-auto rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[10px] text-white/70">
                              {formatJson(entry.role_plan_snapshot) || "// empty"}
                            </pre>
                          </div>
                        </details>
                      </div>
                    );
                  })}
                </div>
              ) : (
                <div className="text-[11px] text-white/50">
                  Latest v{revisionVersion(latestRoleRevision) || "?"}
                  {latestRoleRevision && toNumber(latestRoleRevision.updated_unix_ms)
                    ? ` · ${fmtTs(Number(latestRoleRevision.updated_unix_ms))}`
                    : ""}
                </div>
              )}
            </div>
          </div>
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <div className="text-[11px] text-white/70">Latest JSON</div>
              {copyNote ? <div className="text-[11px] text-emerald-200">{copyNote}</div> : null}
            </div>
            <div className="grid gap-2 md:grid-cols-2">
              <div className="grid gap-2">
                <div className="flex items-center justify-between gap-2">
                  <div className="text-[11px] text-white/60">Goal contract</div>
                  <div className="flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => setShowGoalContractJson((prev) => !prev)}
                    >
                      {showGoalContractJson ? "Hide" : "Show"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => void handleCopyJson(latestGoalContract, "goal contract")}
                    >
                      Copy
                    </button>
                  </div>
                </div>
                {showGoalContractJson ? (
                  <pre className="max-h-60 overflow-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70">
                    {formatJson(latestGoalContract) || "// empty"}
                  </pre>
                ) : (
                  <div className="text-[11px] text-white/50">
                    {latestGoalContract ? "Available" : "Empty"}
                  </div>
                )}
              </div>

              <div className="grid gap-2">
                <div className="flex items-center justify-between gap-2">
                  <div className="text-[11px] text-white/60">Role plan snapshot</div>
                  <div className="flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => setShowRolePlanJson((prev) => !prev)}
                    >
                      {showRolePlanJson ? "Hide" : "Show"}
                    </button>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => void handleCopyJson(latestRolePlanSnapshot, "role plan")}
                    >
                      Copy
                    </button>
                  </div>
                </div>
                {showRolePlanJson ? (
                  <pre className="max-h-60 overflow-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[10px] text-white/70">
                    {formatJson(latestRolePlanSnapshot) || "// empty"}
                  </pre>
                ) : (
                  <div className="text-[11px] text-white/50">
                    {latestRolePlanSnapshot ? "Available" : "Empty"}
                  </div>
                )}
              </div>
            </div>
          </div>
          <div className="grid gap-2 rounded-md border border-white/10 bg-black/40 p-2">
            <div className="text-[11px] text-white/70">Recent revision events</div>
            {revisionEvents.length === 0 ? (
              <div className="text-[11px] text-white/50">No revision events yet.</div>
            ) : (
              <div className="grid gap-2">
                {revisionEvents.map((row, idx) => {
                  const payload = row.payload && typeof row.payload === "object" ? row.payload : {};
                  const type = String(row.type || "");
                  const isGoal = type === "orchestrator_goal_revision";
                  const label = isGoal ? "Goal revision" : "Role plan revision";
                  const version = toNumber((payload as any).version) || 0;
                  const diff = isGoal ? (payload as any).goal_contract_diff : (payload as any).role_plan_diff;
                  const summary = diffSummary(diff);
                  return (
                    <div
                      key={`${type}-${row.event_id || row.ts_unix_ms || idx}`}
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-white/80">
                        {label} · v{version || "?"}
                        {row.ts_unix_ms ? ` · ${fmtTs(row.ts_unix_ms)}` : ""}
                      </div>
                      {summary ? <div className="text-white/50">diff {summary}</div> : null}
                      {diff ? (
                        <button
                          className="mt-1 rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() => void handleCopyJson(diff, `${isGoal ? "goal" : "role plan"} diff`)}
                        >
                          Copy diff
                        </button>
                      ) : null}
                    </div>
                  );
                })}
              </div>
            )}
          </div>
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Update run</div>
          <div className="grid gap-2 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Status</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateStatus}
                onChange={(e) => setUpdateStatus(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Goal</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateGoal}
                onChange={(e) => setUpdateGoal(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Meta (JSON)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateMetaJson}
                onChange={(e) => setUpdateMetaJson(e.target.value)}
              />
            </div>
          </div>
          <div className="grid gap-2 md:grid-cols-2">
            <div className="grid gap-1">
              <FieldLabel>Goal contract (JSON)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateGoalContractJson}
                onChange={(e) => setUpdateGoalContractJson(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Role plan snapshot (JSON)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateRolePlanJson}
                onChange={(e) => setUpdateRolePlanJson(e.target.value)}
              />
            </div>
          </div>
          <div className="grid gap-2 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Expected owner</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateExpectedOwner}
                onChange={(e) => setUpdateExpectedOwner(e.target.value)}
                placeholder={currentOwner || ""}
              />
              <label className="flex items-center gap-2 text-[11px] text-white/60">
                <input
                  type="checkbox"
                  checked={updateExpectedOwnerEmpty}
                  onChange={(e) => setUpdateExpectedOwnerEmpty(e.target.checked)}
                />
                Expect owner empty
              </label>
            </div>
            <div className="grid gap-1">
              <FieldLabel>Expected status</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateExpectedStatus}
                onChange={(e) => setUpdateExpectedStatus(e.target.value)}
              />
            </div>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || updateBusy}
              onClick={() => void handleUpdate()}
            >
              {updateBusy ? "Updating…" : "Update"}
            </button>
            {updateNote ? <div className="text-[11px] text-emerald-200">{updateNote}</div> : null}
          </div>
          {updateError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {updateError}
            </div>
          ) : null}
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Heartbeat</div>
          <div className="grid gap-2 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Status (optional)</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={heartbeatStatus}
                onChange={(e) => setHeartbeatStatus(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Expected owner</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={heartbeatExpectedOwner}
                onChange={(e) => setHeartbeatExpectedOwner(e.target.value)}
                placeholder={currentOwner || ""}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Expected status</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={heartbeatExpectedStatus}
                onChange={(e) => setHeartbeatExpectedStatus(e.target.value)}
              />
            </div>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || heartbeatBusy}
              onClick={() => void handleHeartbeat()}
            >
              {heartbeatBusy ? "Pinging…" : "Send heartbeat"}
            </button>
            <label className="flex items-center gap-2 text-[11px] text-white/60">
              <input
                type="checkbox"
                checked={heartbeatAuto}
                onChange={(e) => setHeartbeatAuto(e.target.checked)}
              />
              Auto
            </label>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              value={heartbeatIntervalMs}
              onChange={(e) => setHeartbeatIntervalMs(Number.parseInt(e.target.value, 10) || 0)}
            />
            <span className="text-[11px] text-white/50">ms</span>
            {heartbeatNote ? <span className="text-[11px] text-emerald-200">{heartbeatNote}</span> : null}
          </div>
          {heartbeatError ? (
            <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
              {heartbeatError}
            </div>
          ) : null}
        </div>
      </div>
    </section>
  );
}
