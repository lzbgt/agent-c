import React from "react";
import FieldLabel from "../FieldLabel";
import type { TeamRunHandoffEventRecord } from "./teamRunStatusTypes";
import { normalizeHandoffEventRecord } from "./teamRunStatusUtils";

type TeamRunStatusHandoffSectionProps = {
  canWrite: boolean;
  handoffEventRows: any[];
  handoffLatestById: Map<string, TeamRunHandoffEventRecord>;
  handoffKind: string;
  setHandoffKind: (value: string) => void;
  handoffFromRole: string;
  setHandoffFromRole: (value: string) => void;
  handoffToRole: string;
  setHandoffToRole: (value: string) => void;
  handoffReason: string;
  setHandoffReason: (value: string) => void;
  handoffMessage: string;
  setHandoffMessage: (value: string) => void;
  handoffSourceDeployment: string;
  setHandoffSourceDeployment: (value: string) => void;
  handoffSourceSession: string;
  setHandoffSourceSession: (value: string) => void;
  handoffTargetDeployment: string;
  setHandoffTargetDeployment: (value: string) => void;
  handoffTargetSession: string;
  setHandoffTargetSession: (value: string) => void;
  handoffData: string;
  setHandoffData: (value: string) => void;
  handoffBusy: boolean;
  handoffError: string | null;
  handoffNote: string | null;
  fmtTs: (ms?: number | null) => string;
  onHandoffEvent: () => Promise<void> | void;
  onHandoffTransition: (
    seed: TeamRunHandoffEventRecord,
    nextState: "accepted" | "declined" | "cancelled",
  ) => Promise<void> | void;
};

export default function TeamRunStatusHandoffSection(props: TeamRunStatusHandoffSectionProps) {
  return (
    <div className="mt-2 grid gap-2 rounded-md border border-white/10 bg-black/30 p-2">
      <div className="text-xs font-semibold text-white/80">Handoff events</div>
      {props.handoffEventRows.length > 0 ? (
        <div className="grid gap-1 text-[11px] text-white/60">
          {props.handoffEventRows.map((ev: any, idx: number) => {
            const record = normalizeHandoffEventRecord(ev);
            const fromRole = record.from_role || "";
            const toRole = record.to_role || "";
            const ts = typeof record.ts_unix_ms === "number" ? props.fmtTs(record.ts_unix_ms) : "";
            const reason = record.reason || "";
            const msg = record.message || "";
            const kind = record.kind || "role";
            const state = record.state || "proposed";
            const handoffId = record.handoff_id || "";
            const latest = handoffId ? props.handoffLatestById.get(handoffId) : undefined;
            const isLatest = !handoffId || latest?.event_index === record.event_index;
            const canResolve = kind === "cross_deployment" && state === "proposed" && isLatest;
            const sourceRef =
              record.source_deployment_id || record.source_session_id
                ? `${record.source_deployment_id || "deployment"} / ${record.source_session_id || "session"}`
                : "";
            const targetRef =
              record.target_deployment_id || record.target_session_id
                ? `${record.target_deployment_id || "deployment"} / ${record.target_session_id || "session"}`
                : "";
            return (
              <div
                key={`handoff-${record.event_index || idx}`}
                data-testid={
                  handoffId ? `team-run-handoff-row-${handoffId}-${record.event_index || idx}` : undefined
                }
                className="rounded-md border border-white/5 bg-black/20 px-2 py-1"
              >
                <div className="text-[11px] text-white/70">
                  {fromRole || "role"} {"->"} {toRole || "role"}
                  {ts ? ` · ${ts}` : ""}
                  {record.event_index ? ` · #${record.event_index}` : ""}
                </div>
                <div className="text-[10px] uppercase tracking-[0.18em] text-white/45">
                  {kind === "cross_deployment" ? "cross-deployment" : "role"} · {state}
                  {handoffId ? ` · ${handoffId}` : ""}
                </div>
                {kind === "cross_deployment" && sourceRef && targetRef ? (
                  <div className="text-[11px] text-sky-100/75">
                    {sourceRef} {"->"} {targetRef}
                  </div>
                ) : null}
                {reason ? <div className="text-[11px] text-white/60">reason: {reason}</div> : null}
                {msg ? <div className="text-[11px] text-white/60">{msg}</div> : null}
                {record.data ? (
                  <pre className="mt-1 whitespace-pre-wrap break-words text-[10px] text-white/50">
                    {JSON.stringify(record.data, null, 2)}
                  </pre>
                ) : null}
                {canResolve ? (
                  <div className="mt-2 flex flex-wrap gap-2">
                    <button
                      className="rounded-md border border-emerald-400/30 bg-emerald-500/10 px-2 py-1 text-[10px] text-emerald-100 hover:bg-emerald-500/20 disabled:opacity-50"
                      type="button"
                      disabled={!props.canWrite || props.handoffBusy}
                      onClick={() => void props.onHandoffTransition(record, "accepted")}
                    >
                      Accept
                    </button>
                    <button
                      className="rounded-md border border-rose-400/30 bg-rose-500/10 px-2 py-1 text-[10px] text-rose-100 hover:bg-rose-500/20 disabled:opacity-50"
                      type="button"
                      disabled={!props.canWrite || props.handoffBusy}
                      onClick={() => void props.onHandoffTransition(record, "declined")}
                    >
                      Decline
                    </button>
                  </div>
                ) : null}
              </div>
            );
          })}
        </div>
      ) : (
        <div className="text-[11px] text-white/50">No handoff events yet.</div>
      )}
      <div className="grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Kind</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.handoffKind}
            onChange={(e) => props.setHandoffKind(e.target.value)}
            data-testid="team-run-handoff-kind"
            disabled={!props.canWrite || props.handoffBusy}
          >
            <option value="role">role</option>
            <option value="cross_deployment">cross_deployment</option>
          </select>
          <FieldLabel>From</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.handoffFromRole}
            onChange={(e) => props.setHandoffFromRole(e.target.value)}
            data-testid="team-run-handoff-from-role"
            placeholder="planner"
            disabled={!props.canWrite || props.handoffBusy}
          />
          <FieldLabel>To</FieldLabel>
          <input
            className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.handoffToRole}
            onChange={(e) => props.setHandoffToRole(e.target.value)}
            data-testid="team-run-handoff-to-role"
            placeholder="executor"
            disabled={!props.canWrite || props.handoffBusy}
          />
          <FieldLabel>Reason</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.handoffReason}
            onChange={(e) => props.setHandoffReason(e.target.value)}
            data-testid="team-run-handoff-reason"
            placeholder="optional"
            disabled={!props.canWrite || props.handoffBusy}
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Message</FieldLabel>
          <input
            className="min-w-[240px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.handoffMessage}
            onChange={(e) => props.setHandoffMessage(e.target.value)}
            data-testid="team-run-handoff-message"
            placeholder="handoff note"
            disabled={!props.canWrite || props.handoffBusy}
          />
        </div>
        {props.handoffKind === "cross_deployment" ? (
          <div className="grid gap-2 rounded-md border border-sky-400/20 bg-sky-500/5 p-2">
            <div className="text-[11px] text-sky-100/80">
              Cross-deployment handoff preserves source/target deployment and session identity for replayable accept/decline flow.
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Source deployment</FieldLabel>
              <input
                className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={props.handoffSourceDeployment}
                onChange={(e) => props.setHandoffSourceDeployment(e.target.value)}
                data-testid="team-run-handoff-source-deployment"
                placeholder="dep-a"
                disabled={!props.canWrite || props.handoffBusy}
              />
              <FieldLabel>Source session</FieldLabel>
              <input
                className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={props.handoffSourceSession}
                onChange={(e) => props.setHandoffSourceSession(e.target.value)}
                data-testid="team-run-handoff-source-session"
                placeholder="sess-a"
                disabled={!props.canWrite || props.handoffBusy}
              />
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Target deployment</FieldLabel>
              <input
                className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={props.handoffTargetDeployment}
                onChange={(e) => props.setHandoffTargetDeployment(e.target.value)}
                data-testid="team-run-handoff-target-deployment"
                placeholder="dep-b"
                disabled={!props.canWrite || props.handoffBusy}
              />
              <FieldLabel>Target session</FieldLabel>
              <input
                className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={props.handoffTargetSession}
                onChange={(e) => props.setHandoffTargetSession(e.target.value)}
                data-testid="team-run-handoff-target-session"
                placeholder="sess-b"
                disabled={!props.canWrite || props.handoffBusy}
              />
            </div>
          </div>
        ) : null}
        <div className="grid gap-1">
          <FieldLabel>Data (JSON object, optional)</FieldLabel>
          <textarea
            className="min-h-[60px] w-full rounded-md border border-white/10 bg-black/20 px-2 py-1 text-[11px] text-white/90"
            value={props.handoffData}
            onChange={(e) => props.setHandoffData(e.target.value)}
            placeholder='{"artifact":"..."}'
            disabled={!props.canWrite || props.handoffBusy}
          />
        </div>
        <button
          className="self-start rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!props.canWrite || props.handoffBusy}
          data-testid="team-run-handoff-submit"
          onClick={() => void props.onHandoffEvent()}
        >
          {props.handoffBusy ? "Sending..." : "Emit handoff"}
        </button>
        {props.handoffError ? <div className="text-[11px] text-rose-200">{props.handoffError}</div> : null}
        {props.handoffNote ? <div className="text-[11px] text-white/60">{props.handoffNote}</div> : null}
      </div>
    </div>
  );
}
