import React from "react";
import type { ApprovalQueuePanelProps } from "./approvalQueue/approvalQueueTypes";
import ApprovalQueueDetailSection from "./approvalQueue/ApprovalQueueDetailSection";
import ApprovalQueueFiltersSection from "./approvalQueue/ApprovalQueueFiltersSection";
import ApprovalQueueListSection from "./approvalQueue/ApprovalQueueListSection";
import { useApprovalQueueState } from "./approvalQueue/useApprovalQueueState";

export type { ApprovalQueuePanelProps } from "./approvalQueue/approvalQueueTypes";

export default function ApprovalQueuePanel(props: ApprovalQueuePanelProps) {
  const state = useApprovalQueueState({
    auth: props.auth,
    baseUrl: props.baseUrl,
  });

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(event) => props.onToggle((event.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Approval queue</div>
          <div className="text-[11px] text-white/50">Gate tool calls with manual approvals</div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <ApprovalQueueFiltersSection state={state} />
        <ApprovalQueueListSection state={state} />
        {state.selectedId ? <ApprovalQueueDetailSection state={state} /> : null}
      </div>
    </details>
  );
}
