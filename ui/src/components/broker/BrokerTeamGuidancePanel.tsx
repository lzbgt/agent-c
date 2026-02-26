import React from "react";
import {
  apiBrokerTeamGuidanceAck,
  apiBrokerTeamGuidanceCreate,
  apiBrokerTeamGuidanceList,
  apiBrokerTeamGuidanceReceiptsList,
  type ApiAuth,
  type BrokerGuidanceEvent,
  type BrokerGuidanceReceipt,
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

const formatDuration = (ms?: number | null) => {
  if (!ms || !Number.isFinite(ms)) return "";
  if (ms < 1000) return `${Math.round(ms)}ms`;
  const sec = ms / 1000;
  if (sec < 60) return `${sec.toFixed(1)}s`;
  const min = sec / 60;
  if (min < 60) return `${min.toFixed(1)}m`;
  const hr = min / 60;
  return `${hr.toFixed(1)}h`;
};

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
  const [receiptsByGuidanceId, setReceiptsByGuidanceId] = React.useState<Record<string, BrokerGuidanceReceipt[]>>({});
  const [receiptsBusyId, setReceiptsBusyId] = React.useState<string>("");
  const [receiptsErrorByGuidanceId, setReceiptsErrorByGuidanceId] = React.useState<Record<string, string>>({});
  const [receiptsOpenByGuidanceId, setReceiptsOpenByGuidanceId] = React.useState<Record<string, boolean>>({});
  const [briefingOpenByGuidanceId, setBriefingOpenByGuidanceId] = React.useState<Record<string, boolean>>({});

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
        if (receiptsOpenByGuidanceId[guidanceId]) {
          const receipts = await apiBrokerTeamGuidanceReceiptsList(
            props.base,
            teamIdTrimmed,
            guidanceId,
            { limit: 50 },
            props.auth,
          );
          if (receipts.ok) {
            setReceiptsByGuidanceId((prev) => ({
              ...prev,
              [guidanceId]: Array.isArray(receipts.receipts) ? receipts.receipts : [],
            }));
          }
        }
      } catch (err) {
        setAckError(String(err));
      } finally {
        setAckBusyId("");
      }
    },
    [canQuery, ackNote, props.base, props.auth, teamIdTrimmed, loadGuidance, receiptsOpenByGuidanceId],
  );

  const guidanceRows = normalizeGuidanceList(guidance);
  const loadReceipts = React.useCallback(
    async (guidanceId: string) => {
      if (!canQuery || !guidanceId) return;
      setReceiptsBusyId(guidanceId);
      setReceiptsErrorByGuidanceId((prev) => ({ ...prev, [guidanceId]: "" }));
      try {
        const resp = await apiBrokerTeamGuidanceReceiptsList(
          props.base,
          teamIdTrimmed,
          guidanceId,
          { limit: 50 },
          props.auth,
        );
        if (!resp.ok) throw new Error(resp.error || resp.err || "receipts failed");
        setReceiptsByGuidanceId((prev) => ({
          ...prev,
          [guidanceId]: Array.isArray(resp.receipts) ? resp.receipts : [],
        }));
      } catch (err) {
        setReceiptsErrorByGuidanceId((prev) => ({ ...prev, [guidanceId]: String(err) }));
        setReceiptsByGuidanceId((prev) => ({ ...prev, [guidanceId]: [] }));
      } finally {
        setReceiptsBusyId("");
      }
    },
    [canQuery, props.base, props.auth, teamIdTrimmed],
  );

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
            {guidanceRows.map((item, idx) => {
              const gid = String(item.guidance_id || "");
              const payload = item.payload && typeof item.payload === "object" ? item.payload : null;
              const briefing =
                payload && typeof payload === "object" && !Array.isArray(payload) ? (payload as any).briefing : null;
              const briefingOpen = !!briefingOpenByGuidanceId[gid];
              const receipts = receiptsByGuidanceId[gid] ?? [];
              const receiptsOpen = !!receiptsOpenByGuidanceId[gid];
              const receiptsError = receiptsErrorByGuidanceId[gid];
              const receiptsBusy = receiptsBusyId === gid;
              const receiptsLoaded = Object.prototype.hasOwnProperty.call(receiptsByGuidanceId, gid);
              return (
                <div key={gid || String(item.created_unix_ms || idx)} className="rounded-md border border-white/10 bg-black/20 p-2">
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
                    <div className="flex flex-wrap items-center gap-2">
                      {briefing && gid ? (
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                          type="button"
                          onClick={() =>
                            setBriefingOpenByGuidanceId((prev) => ({ ...prev, [gid]: !briefingOpen }))
                          }
                        >
                          {briefingOpen ? "Hide briefing" : "Briefing"}
                        </button>
                      ) : null}
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                        type="button"
                        disabled={!canQuery || !gid}
                        onClick={() => {
                          if (!gid) return;
                          const nextOpen = !receiptsOpen;
                          setReceiptsOpenByGuidanceId((prev) => ({ ...prev, [gid]: nextOpen }));
                          if (nextOpen && !receiptsLoaded) {
                            void loadReceipts(gid);
                          }
                        }}
                      >
                        {receiptsOpen ? "Hide receipts" : `Receipts${receipts.length ? ` (${receipts.length})` : ""}`}
                      </button>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                        type="button"
                        disabled={!canQuery || !gid || ackBusyId === gid}
                        onClick={() => void handleAck(gid)}
                      >
                        {ackBusyId === gid ? "Acking…" : "Ack"}
                      </button>
                    </div>
                  </div>
                  <div className="mt-1 text-[12px] text-white/80">{item.message}</div>
                  <div className="mt-1 text-[11px] text-white/50">
                    {Array.isArray(item.target_roles) && item.target_roles.length > 0
                      ? `roles: ${item.target_roles.join(", ")}`
                      : null}
                    {item.target_orchestrator_id ? ` · orch: ${item.target_orchestrator_id}` : null}
                    {item.status ? ` · status: ${item.status}` : null}
                    {item.acked_by ? ` · acked by ${item.acked_by}` : null}
                    {item.acked_unix_ms ? ` · ${fmtTs(item.acked_unix_ms)}` : null}
                  </div>
                  {briefing && briefingOpen ? (
                    <div className="mt-2 space-y-2 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
                      <div className="text-[11px] font-semibold text-white/70">Re-entry briefing</div>
                      {briefing.goal ? (
                        <div>
                          <div className="text-[10px] uppercase text-white/50">Goal</div>
                          <div className="text-[12px] text-white/80">{String(briefing.goal)}</div>
                        </div>
                      ) : null}
                      {briefing.proposed && typeof briefing.proposed === "object" ? (
                        <div>
                          <div className="text-[10px] uppercase text-white/50">Proposed</div>
                          <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/40 p-2 text-[11px] text-white/70">
                            {JSON.stringify(briefing.proposed, null, 2)}
                          </pre>
                        </div>
                      ) : null}
                      {briefing.drift && typeof briefing.drift === "object" ? (
                        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
                          {Number.isFinite(briefing.drift.elapsed_ms) ? (
                            <span>elapsed {formatDuration(Number(briefing.drift.elapsed_ms))}</span>
                          ) : null}
                          {Number.isFinite(briefing.drift.threshold_ms) ? (
                            <span>threshold {formatDuration(Number(briefing.drift.threshold_ms))}</span>
                          ) : null}
                          {briefing.drift.detected_unix_ms ? (
                            <span>{fmtTs(Number(briefing.drift.detected_unix_ms))}</span>
                          ) : null}
                        </div>
                      ) : null}
                      {briefing.team_run_status || briefing.team_run_id ? (
                        <div className="text-[11px] text-white/60">
                          {briefing.team_run_id ? `run ${briefing.team_run_id}` : null}
                          {briefing.team_run_status ? ` · ${briefing.team_run_status}` : null}
                          {Number.isFinite(briefing.team_run_elapsed_ms)
                            ? ` · elapsed ${formatDuration(Number(briefing.team_run_elapsed_ms))}`
                            : null}
                        </div>
                      ) : null}
                      {briefing.goal_contract && typeof briefing.goal_contract === "object" ? (
                        <div>
                          <div className="text-[10px] uppercase text-white/50">Goal contract</div>
                          <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/40 p-2 text-[11px] text-white/70">
                            {JSON.stringify(briefing.goal_contract, null, 2)}
                          </pre>
                        </div>
                      ) : null}
                      {briefing.role_plan_snapshot && typeof briefing.role_plan_snapshot === "object" ? (
                        <div>
                          <div className="text-[10px] uppercase text-white/50">Role plan</div>
                          <pre className="mt-1 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/40 p-2 text-[11px] text-white/70">
                            {JSON.stringify(briefing.role_plan_snapshot, null, 2)}
                          </pre>
                        </div>
                      ) : null}
                    </div>
                  ) : null}
                  {receiptsOpen ? (
                    <div className="mt-2 rounded-md border border-white/10 bg-black/30 p-2 text-[11px] text-white/70">
                      <div className="flex flex-wrap items-center justify-between gap-2">
                        <span className="font-semibold text-white/70">Receipts</span>
                        <button
                          className="rounded-md border border-white/10 bg-black/30 px-2 py-0.5 text-[10px] text-white/60 hover:bg-black/40 disabled:opacity-50"
                          type="button"
                          disabled={!canQuery || receiptsBusy}
                          onClick={() => void loadReceipts(gid)}
                        >
                          {receiptsBusy ? "Loading…" : "Refresh"}
                        </button>
                      </div>
                      {receiptsError ? <div className="mt-1 text-[11px] text-rose-200">{receiptsError}</div> : null}
                      {receiptsBusy && receipts.length === 0 ? (
                        <div className="mt-1 text-[11px] text-white/50">Loading receipts…</div>
                      ) : null}
                      {!receiptsBusy && receiptsLoaded && receipts.length === 0 ? (
                        <div className="mt-1 text-[11px] text-white/50">No receipts yet.</div>
                      ) : null}
                      {receipts.length > 0 ? (
                        <div className="mt-2 space-y-1">
                          {receipts.map((receipt, idx) => (
                            <div
                              key={`${receipt.id || receipt.acked_unix_ms || idx}`}
                              className="rounded-md border border-white/10 bg-black/20 px-2 py-1"
                            >
                              <div className="flex flex-wrap items-center gap-2 text-[10px] text-white/60">
                                {receipt.ack_source ? (
                                  <span className="rounded-full border border-white/10 px-2 py-0.5">
                                    {receipt.ack_source}
                                  </span>
                                ) : null}
                                {receipt.ack_by ? <span>{receipt.ack_by}</span> : null}
                                {receipt.ack_role ? <span>role {receipt.ack_role}</span> : null}
                                {receipt.acked_unix_ms ? <span>{fmtTs(receipt.acked_unix_ms)}</span> : null}
                              </div>
                              {receipt.ack_note ? (
                                <div className="mt-1 text-[11px] text-white/70">{receipt.ack_note}</div>
                              ) : null}
                            </div>
                          ))}
                        </div>
                      ) : null}
                    </div>
                  ) : null}
                </div>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}
