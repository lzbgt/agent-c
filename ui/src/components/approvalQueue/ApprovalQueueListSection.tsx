import React from "react";
import { JsonDetails } from "../ToolOutput";
import type { ApprovalQueueState } from "./approvalQueueTypes";

type ApprovalQueueListSectionProps = {
  state: ApprovalQueueState;
};

export default function ApprovalQueueListSection({ state }: ApprovalQueueListSectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="approval-queue-list">
      <div className="flex items-center justify-between">
        <div className="text-xs font-semibold text-white/80">Approvals</div>
        <div className="text-[11px] text-white/50">{state.approvals.length} items</div>
      </div>
      {state.approvals.length === 0 ? (
        <div className="mt-2 text-xs text-white/40">No approvals loaded.</div>
      ) : (
        <div className="mt-2 grid gap-2">
          {state.approvals.map((approval) => {
            const isSelected = state.selectedId === approval.approval_id;
            return (
              <div
                key={approval.approval_id || `${approval.trace_id || "approval"}-${approval.created_unix_ms || 0}`}
                className={`rounded-md border px-3 py-2 text-xs text-white/80 ${
                  isSelected ? "border-emerald-400/60 bg-emerald-500/10" : "border-white/10 bg-black/30"
                }`}
              >
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <div className="font-semibold text-white/90">{approval.tool_name || "Requested action"}</div>
                  <div className="text-[11px] text-white/50">{approval.status || "pending"}</div>
                </div>
                {approval.decision_reason ? <p className="tool-muted">{approval.decision_reason}</p> : null}
                <JsonDetails label="Request details" value={approval} />
                <div className="mt-2 flex flex-wrap gap-2">
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    disabled={!approval.approval_id}
                    onClick={() => approval.approval_id && state.selectApproval(approval.approval_id)}
                  >
                    {isSelected ? "Reload" : "Details"}
                  </button>
                </div>
              </div>
            );
          })}
        </div>
      )}
    </section>
  );
}
