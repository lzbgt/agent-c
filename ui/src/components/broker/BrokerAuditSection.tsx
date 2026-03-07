import React from "react";
import type { BrokerMembershipAuditResp } from "../../api";
import FieldLabel from "../FieldLabel";

type BrokerMembershipAuditRow = BrokerMembershipAuditResp["audit"][number];

function fmtTs(ms?: number | null) {
  if (!ms || !Number.isFinite(ms)) return "";
  try {
    return new Date(ms).toLocaleString();
  } catch {
    return String(ms);
  }
}

type BrokerAuditSectionProps = {
  canQuery: boolean;
  agentId: string;
  isFetching: boolean;
  error: unknown;
  auditLimit: string;
  setAuditLimit: (next: string) => void;
  auditRows: BrokerMembershipAuditRow[];
  onRefresh: () => void;
};

export default function BrokerAuditSection(props: BrokerAuditSectionProps) {
  const { canQuery, agentId, isFetching, error, auditLimit, setAuditLimit, auditRows, onRefresh } = props;

  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Membership audit log</div>
        <button
          className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
          type="button"
          disabled={!canQuery || !agentId || isFetching}
          onClick={onRefresh}
        >
          {isFetching ? "Loading…" : "Refresh"}
        </button>
      </div>
      <div className="mb-2 flex flex-wrap items-center gap-2">
        <FieldLabel>Limit</FieldLabel>
        <input
          className="w-24 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
          value={auditLimit}
          onChange={(e) => setAuditLimit(e.target.value)}
        />
        <span className="text-[11px] text-white/50">(1-500)</span>
      </div>

      {!agentId ? (
        <div className="text-[11px] text-white/50">Select an agent to view audit history.</div>
      ) : error ? (
        <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
          {String(error)}
        </div>
      ) : auditRows.length === 0 ? (
        <div className="text-[11px] text-white/50">No audit rows.</div>
      ) : (
        <div className="grid gap-2">
          {auditRows.map((row, idx) => {
            const action = String(row?.action || "");
            const actor = String(row?.actor_sub || "");
            const target = String(row?.target_sub || "");
            const role = String(row?.role || "");
            const traceId = String(row?.trace_id || "");
            const ts = fmtTs(row?.ts_unix_ms);
            return (
              <div
                key={`${actor}-${target}-${action}-${idx}`}
                className="rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
              >
                <div className="text-xs text-white/90">{action || "update"}</div>
                <div className="text-[11px] text-white/50">
                  actor {actor} → target {target}
                  {role ? ` · role ${role}` : ""}
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
