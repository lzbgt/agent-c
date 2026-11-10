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
    <section className="tool-workspace" data-testid="approval-queue-panel" aria-label="Policy decisions">
      <div className="mt-3 grid gap-3">
        <ApprovalQueueFiltersSection state={state} />
        <ApprovalQueueListSection state={state} />
        {state.selectedId ? <ApprovalQueueDetailSection state={state} /> : null}
      </div>
    </section>
  );
}
