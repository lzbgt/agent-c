import React from "react";
import WorkflowScheduleCreateSection from "./WorkflowScheduleCreateSection";
import WorkflowScheduleFiltersBar from "./WorkflowScheduleFiltersBar";
import WorkflowScheduleListSection from "./WorkflowScheduleListSection";
import WorkflowScheduleRunsSection from "./WorkflowScheduleRunsSection";
import type { WorkflowSchedulesSectionProps } from "./workflowScheduleSectionTypes";

export default function WorkflowSchedulesSection(props: WorkflowSchedulesSectionProps) {
  return (
    <div className="rounded-md border border-white/10 bg-black/30 p-3" data-testid="workflow-schedules-panel">
      <WorkflowScheduleFiltersBar {...props} />

      <div className="grid gap-2">
        <WorkflowScheduleCreateSection {...props} />

        {!props.baseUrl ? (
          <div className="rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-xs text-amber-100">
            Set a daemon base URL to manage schedules.
          </div>
        ) : null}
        {props.scheduleListQuery.isError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-xs text-rose-200">
            {String(props.scheduleListQuery.error)}
          </div>
        ) : null}

        <WorkflowScheduleListSection {...props} />
        <WorkflowScheduleRunsSection {...props} />
      </div>
    </div>
  );
}
