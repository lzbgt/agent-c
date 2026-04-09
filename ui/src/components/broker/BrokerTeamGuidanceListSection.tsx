import React from "react";

import { fmtTs } from "./teamRunUtils";
import { formatDuration, getGuidanceBriefing } from "./brokerTeamGuidanceUtils";
import type { BrokerTeamGuidanceState } from "./brokerTeamGuidanceTypes";

type BrokerTeamGuidanceListSectionProps = Pick<
  BrokerTeamGuidanceState,
  | "canQuery"
  | "guidanceRows"
  | "ackBusyId"
  | "receiptsByGuidanceId"
  | "receiptsBusyId"
  | "receiptsErrorByGuidanceId"
  | "receiptsOpenByGuidanceId"
  | "briefingOpenByGuidanceId"
  | "handleAck"
  | "loadReceipts"
  | "toggleReceipts"
  | "toggleBriefing"
>;

export default function BrokerTeamGuidanceListSection({
  canQuery,
  guidanceRows,
  ackBusyId,
  receiptsByGuidanceId,
  receiptsBusyId,
  receiptsErrorByGuidanceId,
  receiptsOpenByGuidanceId,
  briefingOpenByGuidanceId,
  handleAck,
  loadReceipts,
  toggleReceipts,
  toggleBriefing,
}: BrokerTeamGuidanceListSectionProps) {
  if (guidanceRows.length === 0) {
    return <div className="text-[11px] text-white/50">No guidance items.</div>;
  }

  return (
    <div className="space-y-2">
      {guidanceRows.map((item, idx) => {
        const guidanceId = String(item.guidance_id || "");
        const briefing = getGuidanceBriefing(item.payload);
        const briefingOpen = !!briefingOpenByGuidanceId[guidanceId];
        const receipts = receiptsByGuidanceId[guidanceId] ?? [];
        const receiptsOpen = !!receiptsOpenByGuidanceId[guidanceId];
        const receiptsError = receiptsErrorByGuidanceId[guidanceId];
        const receiptsBusy = receiptsBusyId === guidanceId;
        const receiptsLoaded = Object.prototype.hasOwnProperty.call(receiptsByGuidanceId, guidanceId);
        return (
          <div key={guidanceId || String(item.created_unix_ms || idx)} className="rounded-md border border-white/10 bg-black/20 p-2">
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
                {briefing && guidanceId ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                    type="button"
                    onClick={() => toggleBriefing(guidanceId)}
                  >
                    {briefingOpen ? "Hide briefing" : "Briefing"}
                  </button>
                ) : null}
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  disabled={!canQuery || !guidanceId}
                  onClick={() => toggleReceipts(guidanceId)}
                >
                  {receiptsOpen ? "Hide receipts" : `Receipts${receipts.length ? ` (${receipts.length})` : ""}`}
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  disabled={!canQuery || !guidanceId || ackBusyId === guidanceId}
                  onClick={() => void handleAck(guidanceId)}
                >
                  {ackBusyId === guidanceId ? "Acking…" : "Ack"}
                </button>
              </div>
            </div>
            <div className="mt-1 text-[12px] text-white/80">{item.message}</div>
            <div className="mt-1 text-[11px] text-white/50">
              {Array.isArray(item.target_roles) && item.target_roles.length > 0 ? `roles: ${item.target_roles.join(", ")}` : null}
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
                    {briefing.drift.detected_unix_ms ? <span>{fmtTs(Number(briefing.drift.detected_unix_ms))}</span> : null}
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
                    onClick={() => void loadReceipts(guidanceId)}
                  >
                    {receiptsBusy ? "Loading…" : "Refresh"}
                  </button>
                </div>
                {receiptsError ? <div className="mt-1 text-[11px] text-rose-200">{receiptsError}</div> : null}
                {receiptsBusy && receipts.length === 0 ? <div className="mt-1 text-[11px] text-white/50">Loading receipts…</div> : null}
                {!receiptsBusy && receiptsLoaded && receipts.length === 0 ? (
                  <div className="mt-1 text-[11px] text-white/50">No receipts yet.</div>
                ) : null}
                {receipts.length > 0 ? (
                  <div className="mt-2 space-y-1">
                    {receipts.map((receipt, receiptIdx) => (
                      <div
                        key={`${receipt.id || receipt.acked_unix_ms || receiptIdx}`}
                        className="rounded-md border border-white/10 bg-black/20 px-2 py-1"
                      >
                        <div className="flex flex-wrap items-center gap-2 text-[10px] text-white/60">
                          {receipt.ack_source ? (
                            <span className="rounded-full border border-white/10 px-2 py-0.5">{receipt.ack_source}</span>
                          ) : null}
                          {receipt.ack_by ? <span>{receipt.ack_by}</span> : null}
                          {receipt.ack_role ? <span>role {receipt.ack_role}</span> : null}
                          {receipt.acked_unix_ms ? <span>{fmtTs(receipt.acked_unix_ms)}</span> : null}
                        </div>
                        {receipt.ack_note ? <div className="mt-1 text-[11px] text-white/70">{receipt.ack_note}</div> : null}
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
  );
}
