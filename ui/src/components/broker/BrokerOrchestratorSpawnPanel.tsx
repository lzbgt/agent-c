import React from "react";
import {
  apiBrokerOrchestratorSpawnRequestCreate,
  apiBrokerOrchestratorSpawnRequestGet,
  apiBrokerOrchestratorSpawnRequestUpdate,
  apiBrokerOrchestratorSpawnRequestsList,
  type ApiAuth,
} from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import FieldLabel from "../FieldLabel";
import { fmtTs } from "./teamRunUtils";

type SpawnPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
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

export default function BrokerOrchestratorSpawnPanel(props: SpawnPanelProps) {
  const teamIdTrimmed = String(props.teamId || "").trim();
  const [listBusy, setListBusy] = React.useState(false);
  const [listError, setListError] = React.useState<string | null>(null);
  const [spawnRequests, setSpawnRequests] = React.useState<any[]>([]);
  const [statusFilter, setStatusFilter] = React.useState<string>("requested");
  const [runFilter, setRunFilter] = React.useState<string>("");

  const [createRole, setCreateRole] = React.useState<string>("planner");
  const [createCount, setCreateCount] = React.useState<string>("1");
  const [createStatus, setCreateStatus] = React.useState<string>("requested");
  const [createRunId, setCreateRunId] = React.useState<string>("");
  const [createRequirementsJson, setCreateRequirementsJson] = React.useState<string>("");
  const [createAssignedJson, setCreateAssignedJson] = React.useState<string>("");
  const [createMetaJson, setCreateMetaJson] = React.useState<string>("");
  const [createBusy, setCreateBusy] = React.useState<boolean>(false);
  const [createError, setCreateError] = React.useState<string | null>(null);
  const [createNote, setCreateNote] = React.useState<string | null>(null);

  const [spawnLookupByTeam, setSpawnLookupByTeam] = useLocalStorageState<Record<string, string>>(
    "agentui.orchestratorSpawnLookupByTeam",
    {},
  );
  const [spawnId, setSpawnIdState] = React.useState<string>("");
  const [spawnBusy, setSpawnBusy] = React.useState<boolean>(false);
  const [spawnError, setSpawnError] = React.useState<string | null>(null);
  const [spawnResult, setSpawnResult] = React.useState<any | null>(null);

  const [updateStatus, setUpdateStatus] = React.useState<string>("");
  const [updateRequirementsJson, setUpdateRequirementsJson] = React.useState<string>("");
  const [updateAssignedJson, setUpdateAssignedJson] = React.useState<string>("");
  const [updateErrorText, setUpdateErrorText] = React.useState<string>("");
  const [updateMetaJson, setUpdateMetaJson] = React.useState<string>("");
  const [updateBusy, setUpdateBusy] = React.useState<boolean>(false);
  const [updateError, setUpdateError] = React.useState<string | null>(null);
  const [updateNote, setUpdateNote] = React.useState<string | null>(null);

  const lastEventKeyRef = React.useRef<string>("");

  React.useEffect(() => {
    if (!teamIdTrimmed) {
      setSpawnIdState("");
      return;
    }
    const next = spawnLookupByTeam[teamIdTrimmed] || "";
    setSpawnIdState(next);
  }, [teamIdTrimmed, spawnLookupByTeam]);

  const setSpawnId = React.useCallback(
    (next: string) => {
      setSpawnIdState(next);
      if (!teamIdTrimmed) return;
      setSpawnLookupByTeam((prev) => ({ ...prev, [teamIdTrimmed]: next }));
    },
    [teamIdTrimmed, setSpawnLookupByTeam],
  );

  const loadSpawnRequests = React.useCallback(async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
    setListBusy(true);
    setListError(null);
    try {
      const resp = await apiBrokerOrchestratorSpawnRequestsList(props.base, teamIdTrimmed, props.auth, {
        limit: 50,
        status: statusFilter.trim() ? statusFilter.trim() : undefined,
        orchestratorRunId: runFilter.trim() ? runFilter.trim() : undefined,
      });
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "spawn requests failed");
      }
      setSpawnRequests(Array.isArray(resp.spawn_requests) ? resp.spawn_requests : []);
    } catch (err) {
      setListError(String(err));
    } finally {
      setListBusy(false);
    }
  }, [props.base, props.auth, props.canQuery, statusFilter, runFilter, teamIdTrimmed]);

  React.useEffect(() => {
    if (!teamIdTrimmed || !props.canQuery) {
      setSpawnRequests([]);
      return;
    }
    void loadSpawnRequests();
  }, [teamIdTrimmed, props.canQuery, loadSpawnRequests]);

  const loadSpawnRequest = React.useCallback(
    async (targetId?: string) => {
      const sid = String(targetId || spawnId || "").trim();
      if (!props.canQuery || !teamIdTrimmed || !sid) return;
      setSpawnBusy(true);
      setSpawnError(null);
      try {
        const resp = await apiBrokerOrchestratorSpawnRequestGet(props.base, teamIdTrimmed, sid, props.auth);
        if (!resp.ok) {
          throw new Error(resp.error || resp.err || resp.code || "spawn request lookup failed");
        }
        setSpawnResult(resp.spawn_request ?? null);
      } catch (err) {
        setSpawnError(String(err));
      } finally {
        setSpawnBusy(false);
      }
    },
    [props.base, props.auth, props.canQuery, spawnId, teamIdTrimmed],
  );

  const handleCreate = async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
    setCreateBusy(true);
    setCreateError(null);
    setCreateNote(null);
    try {
      const role = createRole.trim();
      if (!role) {
        setCreateError("role required");
        return;
      }
      const reqs = parseJsonField(createRequirementsJson, "requirements");
      if (!reqs.ok) {
        setCreateError(reqs.error);
        return;
      }
      const assigned = parseJsonField(createAssignedJson, "assigned_members");
      if (!assigned.ok) {
        setCreateError(assigned.error);
        return;
      }
      const meta = parseJsonField(createMetaJson, "meta");
      if (!meta.ok) {
        setCreateError(meta.error);
        return;
      }
      const body: Record<string, any> = { role };
      const count = Number(createCount);
      if (Number.isFinite(count) && count > 0) body.count = count;
      if (createStatus.trim()) body.status = createStatus.trim();
      if (createRunId.trim()) body.orchestrator_run_id = createRunId.trim();
      if (reqs.value !== undefined) body.requirements = reqs.value;
      if (assigned.value !== undefined) body.assigned_members = assigned.value;
      if (meta.value !== undefined) body.meta = meta.value;
      const resp = await apiBrokerOrchestratorSpawnRequestCreate(props.base, teamIdTrimmed, body, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "create spawn request failed");
      }
      const newReq = resp.spawn_request ?? null;
      if (newReq?.spawn_request_id) {
        setSpawnId(String(newReq.spawn_request_id));
        setSpawnResult(newReq);
        setCreateNote(`created ${newReq.spawn_request_id}`);
      }
      await loadSpawnRequests();
    } catch (err) {
      setCreateError(String(err));
    } finally {
      setCreateBusy(false);
    }
  };

  const handleUpdate = async () => {
    if (!props.canQuery || !teamIdTrimmed) return;
    const sid = String(spawnId || "").trim();
    if (!sid) {
      setUpdateError("spawn_request_id required");
      return;
    }
    setUpdateBusy(true);
    setUpdateError(null);
    setUpdateNote(null);
    try {
      const reqs = parseJsonField(updateRequirementsJson, "requirements");
      if (!reqs.ok) {
        setUpdateError(reqs.error);
        return;
      }
      const assigned = parseJsonField(updateAssignedJson, "assigned_members");
      if (!assigned.ok) {
        setUpdateError(assigned.error);
        return;
      }
      const meta = parseJsonField(updateMetaJson, "meta");
      if (!meta.ok) {
        setUpdateError(meta.error);
        return;
      }
      const body: Record<string, any> = {};
      if (updateStatus.trim()) body.status = updateStatus.trim();
      if (reqs.value !== undefined) body.requirements = reqs.value;
      if (assigned.value !== undefined) body.assigned_members = assigned.value;
      if (updateErrorText.trim()) body.error = updateErrorText.trim();
      if (meta.value !== undefined) body.meta = meta.value;
      if (Object.keys(body).length === 0) {
        setUpdateError("no updates provided");
        return;
      }
      const resp = await apiBrokerOrchestratorSpawnRequestUpdate(props.base, teamIdTrimmed, sid, body, props.auth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "update spawn request failed");
      }
      setSpawnResult(resp.spawn_request ?? null);
      setUpdateNote("updated");
      await loadSpawnRequests();
    } catch (err) {
      setUpdateError(String(err));
    } finally {
      setUpdateBusy(false);
    }
  };

  React.useEffect(() => {
    if (!props.canQuery || !teamIdTrimmed) return;
    const rows = Array.isArray(props.events) ? props.events : [];
    if (rows.length === 0) return;
    const last = rows[rows.length - 1];
    const key = last?.event_id || `${last?.type || ""}:${last?.ts_unix_ms || 0}:${last?.trace_id || ""}`;
    if (!key || lastEventKeyRef.current === key) return;
    lastEventKeyRef.current = key;
    void loadSpawnRequests();
    const sid = last?.payload?.spawn_request_id ? String(last.payload.spawn_request_id) : "";
    if (sid && sid === String(spawnId || "").trim()) {
      void loadSpawnRequest(sid);
    }
  }, [props.canQuery, props.events, teamIdTrimmed, spawnId, loadSpawnRequests, loadSpawnRequest]);

  const current = spawnResult;

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 text-xs font-semibold text-white/80">Orchestrator spawn requests</div>
      <div className="text-[11px] text-white/50">
        Spawn requests are fulfilled by external adapters. See docs/spec/agent_spawn_adapter_v0.md for the contract.
      </div>
      <div className="grid gap-3">
        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Create spawn request</div>
          <div className="grid gap-2 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Role</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={createRole}
                onChange={(e) => setCreateRole(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Count</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={createCount}
                onChange={(e) => setCreateCount(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Status</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={createStatus}
                onChange={(e) => setCreateStatus(e.target.value)}
              />
            </div>
          </div>
          <div className="grid gap-2 md:grid-cols-2">
            <div className="grid gap-1">
              <FieldLabel>Orchestrator run id (optional)</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={createRunId}
                onChange={(e) => setCreateRunId(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Requirements (JSON, optional)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                placeholder='{"capabilities":["host"]}'
                value={createRequirementsJson}
                onChange={(e) => setCreateRequirementsJson(e.target.value)}
              />
            </div>
          </div>
          <div className="grid gap-2 md:grid-cols-2">
            <div className="grid gap-1">
              <FieldLabel>Assigned members (JSON, optional)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                placeholder='[{"agent_id":"agent-1","role":"planner"}]'
                value={createAssignedJson}
                onChange={(e) => setCreateAssignedJson(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Meta (JSON, optional)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                placeholder='{"priority":"high"}'
                value={createMetaJson}
                onChange={(e) => setCreateMetaJson(e.target.value)}
              />
            </div>
          </div>
          <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void handleCreate()}
              disabled={!props.canQuery || !teamIdTrimmed || createBusy}
            >
              {createBusy ? "Creating…" : "Create"}
            </button>
            {createNote ? <span className="text-emerald-200">{createNote}</span> : null}
            {createError ? <span className="text-rose-200">{createError}</span> : null}
          </div>
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Lookup spawn request</div>
          <div className="flex flex-wrap items-center gap-2">
            <input
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
              placeholder="spawn_request_id"
              value={spawnId}
              onChange={(e) => setSpawnId(e.target.value)}
            />
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={!props.canQuery || !teamIdTrimmed || spawnBusy}
              onClick={() => void loadSpawnRequest()}
            >
              {spawnBusy ? "Loading…" : "Load"}
            </button>
          </div>
          {spawnError ? <div className="text-[11px] text-rose-200">{spawnError}</div> : null}
          {current ? (
            <div className="grid gap-1 text-[11px] text-white/70">
              <div>
                <span className="text-white/60">status:</span> {String(current.status || "")}
              </div>
              <div>
                <span className="text-white/60">role:</span> {String(current.role || "")}{" "}
                <span className="text-white/60">count:</span> {String(current.count ?? "")}
              </div>
              <div>
                <span className="text-white/60">created:</span> {fmtTs(current.created_unix_ms)}{" "}
                <span className="text-white/60">updated:</span> {fmtTs(current.updated_unix_ms)}
              </div>
              {current.orchestrator_run_id ? (
                <div>
                  <span className="text-white/60">orchestrator_run_id:</span>{" "}
                  {String(current.orchestrator_run_id)}
                </div>
              ) : null}
              {current.error ? (
                <div className="text-rose-200">
                  <span className="text-white/60">error:</span> {String(current.error)}
                </div>
              ) : null}
              {current.requirements ? (
                <div className="text-white/60">
                  requirements:
                  <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/40 p-2 text-[10px] text-white/70">
                    {JSON.stringify(current.requirements, null, 2)}
                  </pre>
                </div>
              ) : null}
              {Array.isArray(current.assigned_members) && current.assigned_members.length > 0 ? (
                <div className="text-white/60">
                  assigned_members:
                  <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/40 p-2 text-[10px] text-white/70">
                    {JSON.stringify(current.assigned_members, null, 2)}
                  </pre>
                </div>
              ) : null}
              {current.meta && Object.keys(current.meta).length > 0 ? (
                <div className="text-white/60">
                  meta:
                  <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/40 p-2 text-[10px] text-white/70">
                    {JSON.stringify(current.meta, null, 2)}
                  </pre>
                </div>
              ) : null}
            </div>
          ) : null}
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Update spawn request</div>
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
              <FieldLabel>Error</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateErrorText}
                onChange={(e) => setUpdateErrorText(e.target.value)}
              />
            </div>
            <div className="grid gap-1">
              <FieldLabel>Requirements (JSON)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateRequirementsJson}
                onChange={(e) => setUpdateRequirementsJson(e.target.value)}
              />
            </div>
          </div>
          <div className="grid gap-2 md:grid-cols-2">
            <div className="grid gap-1">
              <FieldLabel>Assigned members (JSON)</FieldLabel>
              <textarea
                className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={updateAssignedJson}
                onChange={(e) => setUpdateAssignedJson(e.target.value)}
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
          <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void handleUpdate()}
              disabled={!props.canQuery || !teamIdTrimmed || updateBusy}
            >
              {updateBusy ? "Updating…" : "Update"}
            </button>
            {updateNote ? <span className="text-emerald-200">{updateNote}</span> : null}
            {updateError ? <span className="text-rose-200">{updateError}</span> : null}
          </div>
        </div>

        <div className="grid gap-2 rounded-md border border-white/5 bg-black/30 p-2">
          <div className="text-[11px] text-white/60">Recent spawn requests</div>
          <div className="grid gap-2 md:grid-cols-3">
            <div className="grid gap-1">
              <FieldLabel>Status filter</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={statusFilter}
                onChange={(e) => setStatusFilter(e.target.value)}
              />
            </div>
            <div className="grid gap-1 md:col-span-2">
              <FieldLabel>Orchestrator run id filter</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/40 px-2 py-1 text-xs text-white/90"
                value={runFilter}
                onChange={(e) => setRunFilter(e.target.value)}
              />
            </div>
          </div>
          <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void loadSpawnRequests()}
              disabled={!props.canQuery || !teamIdTrimmed || listBusy}
            >
              {listBusy ? "Loading…" : "Refresh"}
            </button>
            {listError ? <span className="text-rose-200">{listError}</span> : null}
          </div>
          {spawnRequests.length > 0 ? (
            <div className="grid gap-2 text-[11px] text-white/70">
              {spawnRequests.map((req) => (
                <button
                  key={String(req.spawn_request_id || "")}
                  className="rounded-md border border-white/10 bg-black/40 px-2 py-1 text-left hover:bg-black/50"
                  type="button"
                  onClick={() => {
                    const nextId = String(req.spawn_request_id || "");
                    setSpawnId(nextId);
                    setSpawnResult(req);
                  }}
                >
                  <div className="flex flex-wrap items-center justify-between gap-2">
                    <div className="text-white/80">
                      {String(req.spawn_request_id || "")} · {String(req.role || "")} · {String(req.status || "")}
                    </div>
                    <div className="text-white/50">{fmtTs(req.updated_unix_ms || req.created_unix_ms)}</div>
                  </div>
                  <div className="text-white/50">
                    count {String(req.count ?? "")}
                    {req.orchestrator_run_id ? ` · run ${String(req.orchestrator_run_id)}` : ""}
                  </div>
                </button>
              ))}
            </div>
          ) : (
            <div className="text-[11px] text-white/50">No spawn requests yet.</div>
          )}
        </div>
      </div>
    </section>
  );
}
