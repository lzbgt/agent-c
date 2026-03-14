import React from "react";
import type { BrokerEventRow } from "./types";
import { fmtTs } from "./useBrokerPanelState";

type BrokerEventsSectionProps = {
  canQuery: boolean;
  brokerEventsActive: boolean;
  setBrokerEventsActive: React.Dispatch<React.SetStateAction<boolean>>;
  brokerEventsConnected: boolean;
  brokerEventsReplayBusy: boolean;
  brokerEventsReplayNote: string | null;
  brokerEventsReplayError: string | null;
  brokerEventsError: string | null;
  brokerEventsQuorumOnly: boolean;
  setBrokerEventsQuorumOnly: React.Dispatch<React.SetStateAction<boolean>>;
  brokerEvents: BrokerEventRow[];
  brokerEventRows: BrokerEventRow[];
  loadBrokerEventsReplay: () => Promise<void>;
  clearBrokerEvents: () => void;
};

export default function BrokerEventsSection(props: BrokerEventsSectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Live broker events</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={() => props.setBrokerEventsActive((prev) => !prev)}
          >
            {props.brokerEventsActive ? "Pause" : "Resume"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!props.canQuery || props.brokerEventsReplayBusy}
            onClick={() => void props.loadBrokerEventsReplay()}
          >
            {props.brokerEventsReplayBusy ? "Replaying…" : "Replay"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40"
            type="button"
            onClick={props.clearBrokerEvents}
          >
            Clear
          </button>
        </div>
      </div>
      <div className="mb-2 flex flex-wrap items-center gap-3 text-[11px] text-white/50">
        <span>{props.brokerEventsActive ? (props.brokerEventsConnected ? "Connected" : "Connecting…") : "Paused"}</span>
        {props.brokerEventsReplayNote ? <span>{props.brokerEventsReplayNote}</span> : null}
        <label className="flex items-center gap-1">
          <input
            type="checkbox"
            checked={props.brokerEventsQuorumOnly}
            onChange={(e) => props.setBrokerEventsQuorumOnly(e.target.checked)}
          />
          Quorum only
        </label>
        <span>{props.brokerEvents.length} events</span>
      </div>
      {props.brokerEventsReplayError ? (
        <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
          {props.brokerEventsReplayError}
        </div>
      ) : null}
      {props.brokerEventsError ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {props.brokerEventsError}
        </div>
      ) : props.brokerEventRows.length === 0 ? (
        <div className="text-[11px] text-white/50">No events yet.</div>
      ) : (
        <div className="grid gap-2">
          {props.brokerEventRows.map((row, idx) => {
            const type = String(row?.type || "");
            const payload =
              row?.payload && typeof row.payload === "object" ? (row.payload as Record<string, unknown>) : {};
            const ts = fmtTs(row?.ts_unix_ms);
            const traceId = row?.trace_id ? String(row.trace_id) : "";
            let summary = "";
            if (type === "team_quorum_request") {
              const action = payload?.action ? String(payload.action) : "";
              const min = payload?.min_approvals;
              const ruleId = payload?.rule_id ? String(payload.rule_id) : "";
              summary = `${action || "quorum"} · min ${min ?? "?"}${ruleId ? ` · rule ${ruleId}` : ""}`;
            } else if (type === "team_quorum_result") {
              const decision = payload?.decision ? String(payload.decision) : "result";
              const approvals = payload?.approvals;
              const required = payload?.required_approvals;
              summary = `${decision} · ${approvals ?? "?"}/${required ?? "?"}`;
            } else if (type === "team_goal_progress" || type === "team_goal_drift" || type === "team_goal_spawn_validation") {
              const ev =
                payload?.event && typeof payload.event === "object"
                  ? (payload.event as Record<string, unknown>)
                  : {};
              const msg = ev?.message ? String(ev.message) : "";
              const label =
                type === "team_goal_progress"
                  ? "progress"
                  : type === "team_goal_drift"
                    ? "drift"
                    : "spawn validation";
              summary = `${label}${msg ? ` · ${msg}` : ""}`;
            } else if (type === "team_handoff") {
              const ev =
                payload?.event && typeof payload.event === "object"
                  ? (payload.event as Record<string, unknown>)
                  : {};
              const kind = ev?.kind ? String(ev.kind).trim().toLowerCase() : "role";
              const state = ev?.state ? String(ev.state).trim().toLowerCase() : "proposed";
              const fromRole = ev?.from_role ? String(ev.from_role) : "";
              const toRole = ev?.to_role ? String(ev.to_role) : "";
              const reason = ev?.reason ? String(ev.reason) : "";
              if (kind === "cross_deployment") {
                const sourceDeployment = ev?.source_deployment_id ? String(ev.source_deployment_id) : "";
                const targetDeployment = ev?.target_deployment_id ? String(ev.target_deployment_id) : "";
                summary = `${state} handoff ${fromRole || "role"} -> ${toRole || "role"}${sourceDeployment || targetDeployment ? ` · ${sourceDeployment || "src"} -> ${targetDeployment || "dst"}` : ""}${reason ? ` · ${reason}` : ""}`;
              } else {
                summary = `handoff ${fromRole || "role"} -> ${toRole || "role"}${reason ? ` · ${reason}` : ""}`;
              }
            } else if (payload && Object.keys(payload).length > 0) {
              try {
                summary = JSON.stringify(payload);
              } catch {
                summary = String(payload);
              }
              if (summary.length > 120) summary = `${summary.slice(0, 117)}…`;
            }
            return (
              <div
                key={`broker-event-${type}-${idx}`}
                className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
              >
                <div className="text-xs text-white/90">{type || "event"}</div>
                <div className="text-[11px] text-white/50">
                  {summary || "payload captured"}
                  {traceId ? ` · trace ${traceId}` : ""}
                  {ts ? ` · ${ts}` : ""}
                </div>
              </div>
            );
          })}
        </div>
      )}
    </section>
  );
}
