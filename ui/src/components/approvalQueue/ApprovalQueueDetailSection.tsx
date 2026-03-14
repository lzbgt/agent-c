import React from "react";
import FieldLabel from "../FieldLabel";
import type { ApprovalQueueState } from "./approvalQueueTypes";

type ApprovalQueueDetailSectionProps = {
  state: ApprovalQueueState;
};

export default function ApprovalQueueDetailSection({ state }: ApprovalQueueDetailSectionProps) {
  return (
    <section className="rounded-md border border-white/10 bg-black/20 p-3" data-testid="approval-queue-detail">
      <div className="text-xs font-semibold text-white/80">Selected approval</div>
      <div className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
        {state.detailBusy ? (
          <div className="text-[11px] text-white/60">Loading details…</div>
        ) : state.detailError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {state.detailError}
          </div>
        ) : (
          <div className="grid gap-2">
            <div className="text-[11px] text-white/70">
              Required approvals: {state.detailApproval?.required_approvals ?? "-"}
            </div>
            {state.detailApproval?.role_constraints && state.detailApproval.role_constraints.length > 0 ? (
              <div className="text-[11px] text-white/70">
                Role constraints: {state.detailApproval.role_constraints.join(", ")}
              </div>
            ) : null}
            <div className="text-[11px] text-white/70">Decision reason: {state.detailApproval?.decision_reason || "-"}</div>
            <div className="text-[11px] text-white/70">Decisions: {state.detailDecisions.length}</div>
            {state.detailDecisions.length > 0 ? (
              <div className="grid gap-1">
                {state.detailDecisions.map((decision) => (
                  <div
                    key={`${decision.id ?? ""}-${decision.member_id ?? ""}-${decision.decision ?? ""}`}
                    className="rounded border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                  >
                    {decision.member_id || "?"}
                    {decision.member_role ? ` (${decision.member_role})` : ""}: {decision.decision || "?"} (
                    {decision.decision_unix_ms ?? "-"})
                  </div>
                ))}
              </div>
            ) : null}

            <div className="grid gap-2">
              <FieldLabel>Submit decision</FieldLabel>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90 placeholder:text-white/40"
                placeholder="member_id"
                value={state.memberId}
                onChange={(event) => state.setMemberId(event.target.value)}
              />
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90 placeholder:text-white/40"
                placeholder="member_role (optional)"
                value={state.memberRole}
                onChange={(event) => state.setMemberRole(event.target.value)}
                list="approval-role-constraints"
              />
              <datalist id="approval-role-constraints">
                {(state.detailApproval?.role_constraints ?? []).map((role, index) => (
                  <option key={`role-${index}-${role}`} value={role} />
                ))}
              </datalist>
              <input
                className="w-full rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/90 placeholder:text-white/40"
                placeholder="note (optional)"
                value={state.note}
                onChange={(event) => state.setNote(event.target.value)}
              />
              <div className="flex flex-wrap gap-2">
                <button
                  className="rounded-md border border-emerald-500/40 bg-emerald-500/10 px-2 py-1 text-[11px] text-emerald-100 hover:bg-emerald-500/20 disabled:opacity-50"
                  type="button"
                  disabled={!state.isPending || state.decisionBusy}
                  onClick={() => state.submitDecision("approve")}
                >
                  Approve
                </button>
                <button
                  className="rounded-md border border-rose-500/40 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-100 hover:bg-rose-500/20 disabled:opacity-50"
                  type="button"
                  disabled={!state.isPending || state.decisionBusy}
                  onClick={() => state.submitDecision("deny")}
                >
                  Deny
                </button>
              </div>
              {state.decisionError ? (
                <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                  {state.decisionError}
                </div>
              ) : null}
            </div>
          </div>
        )}
      </div>
    </section>
  );
}
