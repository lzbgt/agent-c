import React from "react";
import FieldLabel from "../FieldLabel";
import type { TeamRunCreatePanelProps } from "./TeamRunCreatePanel";

export default function TeamRunCreateApprovalsSection(props: TeamRunCreatePanelProps) {
  const canCreate = props.canQuery && !!props.teamId;
  return (
    <details className="rounded-md border border-white/10 bg-black/20 p-2" data-testid="team-inline-approvals">
      <summary className="cursor-pointer text-[11px] text-white/70">Inline approvals (optional)</summary>
      <div className="mt-2 grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Member ID</FieldLabel>
          <input
            className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalMemberId}
            onChange={(e) => props.setRunApprovalMemberId(e.target.value)}
            placeholder="member id"
            list="team-approvals-members"
          />
          <FieldLabel>Decision</FieldLabel>
          <select
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalDecision}
            onChange={(e) => props.setRunApprovalDecision(e.target.value as "approve" | "deny")}
          >
            <option value="approve">approve</option>
            <option value="deny">deny</option>
          </select>
          <FieldLabel>Rule ID</FieldLabel>
          <input
            className="min-w-[140px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalRuleId}
            onChange={(e) => props.setRunApprovalRuleId(e.target.value)}
            placeholder="optional"
            list="team-approvals-rules"
          />
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Reason</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={props.runApprovalReason}
            onChange={(e) => props.setRunApprovalReason(e.target.value)}
            placeholder="optional"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canCreate || props.runBusy}
            onClick={() => props.handleAddRunApproval()}
          >
            Add approval
          </button>
          {props.runApprovals.length > 0 ? (
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
              type="button"
              onClick={() => props.setRunApprovals([])}
            >
              Clear approvals
            </button>
          ) : null}
        </div>
        {props.runApprovals.length > 0 ? (
          <div className="grid gap-2">
            {props.runApprovals.map((a, idx) => (
              <div
                key={`run-approval-${a.member_id}-${a.rule_id || "any"}-${idx}`}
                className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
              >
                <div className="text-[11px] text-white/70">
                  <span className="text-white/90">{a.member_id}</span>
                  {a.decision ? ` · ${a.decision}` : ""}
                  {a.rule_id ? ` · rule ${a.rule_id}` : ""}
                  {a.reason ? ` · ${a.reason}` : ""}
                </div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                  type="button"
                  onClick={() => props.setRunApprovals((prev) => prev.filter((_, i) => i !== idx))}
                >
                  Remove
                </button>
              </div>
            ))}
          </div>
        ) : (
          <div className="text-[11px] text-white/50">No inline approvals.</div>
        )}
      </div>
    </details>
  );
}
