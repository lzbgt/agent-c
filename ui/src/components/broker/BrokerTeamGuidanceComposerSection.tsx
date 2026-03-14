import React from "react";

import FieldLabel from "../FieldLabel";
import { fmtTs } from "./teamRunUtils";
import type { BrokerTeamGuidanceState } from "./brokerTeamGuidanceTypes";

type BrokerTeamGuidanceComposerSectionProps = Pick<
  BrokerTeamGuidanceState,
  | "canQuery"
  | "guidanceEvents"
  | "listBusy"
  | "listError"
  | "statusFilter"
  | "teamRunFilter"
  | "kind"
  | "priority"
  | "message"
  | "payloadJson"
  | "targetRoles"
  | "targetOrchestrator"
  | "expiresUnixMs"
  | "createBusy"
  | "createError"
  | "ackNote"
  | "ackError"
  | "setStatusFilter"
  | "setTeamRunFilter"
  | "setKind"
  | "setPriority"
  | "setMessage"
  | "setPayloadJson"
  | "setTargetRoles"
  | "setTargetOrchestrator"
  | "setExpiresUnixMs"
  | "setAckNote"
  | "loadGuidance"
  | "handleCreate"
>;

export default function BrokerTeamGuidanceComposerSection({
  canQuery,
  guidanceEvents,
  listBusy,
  listError,
  statusFilter,
  teamRunFilter,
  kind,
  priority,
  message,
  payloadJson,
  targetRoles,
  targetOrchestrator,
  expiresUnixMs,
  createBusy,
  createError,
  ackNote,
  ackError,
  setStatusFilter,
  setTeamRunFilter,
  setKind,
  setPriority,
  setMessage,
  setPayloadJson,
  setTargetRoles,
  setTargetOrchestrator,
  setExpiresUnixMs,
  setAckNote,
  loadGuidance,
  handleCreate,
}: BrokerTeamGuidanceComposerSectionProps) {
  return (
    <>
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
      </div>
    </>
  );
}
