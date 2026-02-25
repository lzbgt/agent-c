import React from "react";
import {
  apiBrokerTeamGuidanceAck,
  apiBrokerTeamGuidanceCreate,
  apiBrokerTeamGuidanceList,
  type ApiAuth,
  type BrokerGuidanceEvent,
} from "../../api";
import FieldLabel from "../FieldLabel";
import { fmtTs, parseCsvList, GUIDANCE_EVENT_TYPES } from "./teamRunUtils";
import type { BrokerEventRow } from "./types";

export type BrokerTeamGuidancePanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  events?: BrokerEventRow[];
};

const normalizeGuidanceList = (rows?: BrokerGuidanceEvent[]) =>
  Array.isArray(rows) ? rows.filter((row) => row && typeof row === "object") : [];

export default function BrokerTeamGuidancePanel(props: BrokerTeamGuidancePanelProps) {
  const teamIdTrimmed = String(props.teamId || "").trim();
  const canQuery = props.canQuery && teamIdTrimmed.length > 0;

  const [guidance, setGuidance] = React.useState<BrokerGuidanceEvent[]>([]);
  const [listBusy, setListBusy] = React.useState(false);
  const [listError, setListError] = React.useState<string | null>(null);
  const [statusFilter, setStatusFilter] = React.useState<string>("open");
  const [teamRunFilter, setTeamRunFilter] = React.useState<string>("");

  const [kind, setKind] = React.useState<string>("directive");
  const [priority, setPriority] = React.useState<string>("normal");
  const [message, setMessage] = React.useState<string>("");
  const [payloadJson, setPayloadJson] = React.useState<string>("");
  const [targetRoles, setTargetRoles] = React.useState<string>("");
  const [targetOrchestrator, setTargetOrchestrator] = React.useState<string>("");
  const [expiresUnixMs, setExpiresUnixMs] = React.useState<string>("");
  const [createBusy, setCreateBusy] = React.useState(false);
  const [createError, setCreateError] = React.useState<string | null>(null);

  const [ackNote, setAckNote] = React.useState<string>("");
  const [ackBusyId, setAckBusyId] = React.useState<string>("");
  const [ackError, setAckError] = React.useState<string | null>(null);
  const lastEventTsRef = React.useRef<number>(0);

  const guidanceEvents = React.useMemo(() => {
    const rows = Array.isArray(props.events) ? props.events : [];
    return rows.filter((row) => GUIDANCE_EVENT_TYPES.has(String(row?.type || "")));
  }, [props.events]);

  const loadGuidance = React.useCallback(async () => {
    if (!canQuery) return;
    setListBusy(true);
    setListError(null);
    try {
      const resp = await apiBrokerTeamGuidanceList(
        props.base,
        teamIdTrimmed,
        {
          status: statusFilter,
          teamRunId: teamRunFilter.trim() || undefined,
          limit: 200,
        },
        props.auth,
      );
      if (!resp.ok) throw new Error(resp.error || resp.err || "guidance list failed");
      setGuidance(normalizeGuidanceList(resp.guidance));
    } catch (err) {
      setListError(String(err));
      setGuidance([]);
    } finally {
      setListBusy(false);
    }
  }, [canQuery, props.base, props.auth, teamIdTrimmed, statusFilter, teamRunFilter]);

  React.useEffect(() => {
    void loadGuidance();
  }, [loadGuidance]);

  React.useEffect(() => {
    if (!canQuery) return;
    if (guidanceEvents.length === 0) return;
    const maxTs = guidanceEvents.reduce((acc, row) => Math.max(acc, row.ts_unix_ms || 0), 0);
    if (maxTs <= 0 || maxTs <= lastEventTsRef.current) return;
    lastEventTsRef.current = maxTs;
    void loadGuidance();
  }, [canQuery, guidanceEvents, loadGuidance]);

  const handleCreate = React.useCallback(async () => {
    if (!canQuery) return;
    const msg = message.trim();
    if (!msg) {
      setCreateError("message required");
      return;
    }
    setCreateBusy(true);
    setCreateError(null);
    try {
      let payload: Record<string, any> | undefined;
      const rawPayload = payloadJson.trim();
      if (rawPayload) {
        payload = JSON.parse(rawPayload);
      }
      const expires = expiresUnixMs.trim();
      const body: Record<string, any> = {
        kind,
        priority,
        message: msg,
      };
      const teamRun = teamRunFilter.trim();
      if (teamRun) body.team_run_id = teamRun;
      if (payload) body.payload = payload;
      const roles = parseCsvList(targetRoles);
      if (roles.length > 0) body.target_roles = roles;
      const orch = targetOrchestrator.trim();
      if (orch) body.target_orchestrator_id = orch;
      if (expires) body.expires_unix_ms = Number(expires);
      const resp = await apiBrokerTeamGuidanceCreate(props.base, teamIdTrimmed, body, props.auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || "create guidance failed");
      setMessage("");
      setPayloadJson("");
      setExpiresUnixMs("");
      await loadGuidance();
    } catch (err) {
      setCreateError(String(err));
    } finally {
      setCreateBusy(false);
    }
  }, [
    canQuery,
    message,
    kind,
    priority,
    payloadJson,
    targetRoles,
    targetOrchestrator,
    expiresUnixMs,
    teamRunFilter,
    props.base,
    props.auth,
    teamIdTrimmed,
    loadGuidance,
  ]);

  const handleAck = React.useCallback(
    async (guidanceId: string) => {
      if (!canQuery || !guidanceId) return;
      setAckBusyId(guidanceId);
      setAckError(null);
      try {
        const resp = await apiBrokerTeamGuidanceAck(
          props.base,
          teamIdTrimmed,
          guidanceId,
          { status: "acked", note: ackNote.trim(), ack_source: "human" },
          props.auth,
        );
        if (!resp.ok) throw new Error(resp.error || resp.err || "ack failed");
        await loadGuidance();
      } catch (err) {
        setAckError(String(err));
      } finally {
        setAckBusyId("");
      }
    },
    [canQuery, ackNote, props.base, props.auth, teamIdTrimmed, loadGuidance],
  );

  const guidanceRows = normalizeGuidanceList(guidance);

  return (
    <div className="space-y-3 rounded-lg border border-white/10 bg-white/5 p-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="text-sm font-semibold text-white/80">Guidance lane</div>
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <label className="flex items-center gap-2">
            <span>Status</span>
            <input
              className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
              value={statusFilter}
              onChange={(e) => setStatusFilter(e.target.value)}
            />
          </label>
          <label className="flex items-center gap-2">
            <span>Team run</span>
            <input
              className="w-40 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
              value={teamRunFilter}
              onChange={(e) => setTeamRunFilter(e.target.value)}
              placeholder="optional team_run_id"
            />
          </label>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || listBusy}
            onClick={() => void loadGuidance()}
          >
            {listBusy ? "Refreshing…" : "Refresh"}
          </button>
          {listError ? <span className="text-rose-200">{listError}</span> : null}
        </div>
      </div>

      <div className="grid gap-2 md:grid-cols-2">
        <div className="space-y-2">
          <FieldLabel>Kind</FieldLabel>
          <select
            className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={kind}
            onChange={(e) => setKind(e.target.value)}
          >
            <option value="directive">directive</option>
            <option value="context">context</option>
            <option value="warning">warning</option>
            <option value="constraint">constraint</option>
          </select>
          <FieldLabel>Priority</FieldLabel>
          <select
            className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={priority}
            onChange={(e) => setPriority(e.target.value)}
          >
            <option value="low">low</option>
            <option value="normal">normal</option>
            <option value="high">high</option>
            <option value="urgent">urgent</option>
          </select>
          <FieldLabel>Message</FieldLabel>
          <textarea
            className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={message}
            onChange={(e) => setMessage(e.target.value)}
            placeholder="guidance message"
          />
        </div>
        <div className="space-y-2">
          <FieldLabel>Target roles (csv)</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={targetRoles}
            onChange={(e) => setTargetRoles(e.target.value)}
            placeholder="lead,reviewer"
          />
          <FieldLabel>Target orchestrator id</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={targetOrchestrator}
            onChange={(e) => setTargetOrchestrator(e.target.value)}
          />
          <FieldLabel>Expires unix ms (optional)</FieldLabel>
          <input
            className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={expiresUnixMs}
            onChange={(e) => setExpiresUnixMs(e.target.value)}
            placeholder="e.g. 1761177600000"
          />
          <FieldLabel>Payload JSON (optional)</FieldLabel>
          <textarea
            className="min-h-[84px] w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={payloadJson}
            onChange={(e) => setPayloadJson(e.target.value)}
            placeholder='{"reason":"budget"}'
          />
        </div>
      </div>

      <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
        <button
          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || createBusy}
          onClick={() => void handleCreate()}
        >
          {createBusy ? "Creating…" : "Send guidance"}
        </button>
        {createError ? <span className="text-rose-200">{createError}</span> : null}
      </div>

      <div className="space-y-2">
        {guidanceEvents.length > 0 ? (
          <div className="rounded-md border border-white/10 bg-black/20 p-2 text-[11px] text-white/60">
            <div className="text-[11px] font-semibold text-white/70">Recent guidance events</div>
            <div className="mt-1 space-y-1">
              {guidanceEvents.slice(-4).map((ev, idx) => (
                <div key={`${ev.event_id || ev.ts_unix_ms || idx}`} className="flex flex-wrap gap-2">
                  <span className="rounded-full border border-white/10 px-2 py-0.5 text-[10px] text-white/60">
                    {ev.type}
                  </span>
                  {ev.ts_unix_ms ? <span>{fmtTs(ev.ts_unix_ms)}</span> : null}
                  {ev.payload?.guidance_id ? <span>{String(ev.payload.guidance_id)}</span> : null}
                </div>
              ))}
            </div>
          </div>
        ) : null}
        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
          <FieldLabel>Ack note</FieldLabel>
          <input
            className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80"
            value={ackNote}
            onChange={(e) => setAckNote(e.target.value)}
            placeholder="ack note"
          />
          {ackError ? <span className="text-rose-200">{ackError}</span> : null}
        </div>
        {guidanceRows.length === 0 ? (
          <div className="text-[11px] text-white/50">No guidance items.</div>
        ) : (
          <div className="space-y-2">
            {guidanceRows.map((item) => {
              const gid = String(item.guidance_id || "");
              return (
                <div key={gid || Math.random()} className="rounded-md border border-white/10 bg-black/20 p-2">
                  <div className="flex flex-wrap items-center justify-between gap-2 text-[11px] text-white/70">
                    <div className="flex flex-wrap items-center gap-2">
                      <span className="rounded-full border border-white/10 px-2 py-0.5 text-[10px] text-white/60">
                        {item.kind || "guidance"}
                      </span>
                      <span className="rounded-full border border-white/10 px-2 py-0.5 text-[10px] text-white/60">
                        {item.priority || "normal"}
                      </span>
                      {item.team_run_id ? <span>run {item.team_run_id}</span> : null}
                      {item.created_unix_ms ? <span>{fmtTs(item.created_unix_ms)}</span> : null}
                    </div>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      disabled={!canQuery || !gid || ackBusyId === gid}
                      onClick={() => void handleAck(gid)}
                    >
                      {ackBusyId === gid ? "Acking…" : "Ack"}
                    </button>
                  </div>
                  <div className="mt-1 text-[12px] text-white/80">{item.message}</div>
                  <div className="mt-1 text-[11px] text-white/50">
                    {Array.isArray(item.target_roles) && item.target_roles.length > 0
                      ? `roles: ${item.target_roles.join(", ")}`
                      : null}
                    {item.target_orchestrator_id ? ` · orch: ${item.target_orchestrator_id}` : null}
                  </div>
                </div>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}
