import React from "react";
import type { ApiAuth } from "../api/auth";
import WorkflowComposer from "./WorkflowComposer";
import WorkflowDetailSection from "./workflow/WorkflowDetailSection";
import WorkflowListSection from "./workflow/WorkflowListSection";
import WorkflowSchedulesSection from "./workflow/WorkflowSchedulesSection";
import useWorkflowPanelState from "./workflow/useWorkflowPanelState";

export type WorkflowPanelProps = {
  open: boolean;
  onToggle: (open: boolean) => void;
  baseUrl: string;
  auth?: ApiAuth;
  authKey?: string;
  clientId?: string;
  onTraceIdClick?: (traceId: string) => void;
  workflowDefaults?: Record<string, any>;
  workflowTargets?: string[];
  workflowBearerEnv?: string;
};

export default function WorkflowPanel(props: WorkflowPanelProps) {
  const workflowState = useWorkflowPanelState({
    open: props.open,
    baseUrl: props.baseUrl,
    auth: props.auth,
    authKey: props.authKey,
  });

  return (
    <details
      className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
      open={!!props.open}
      onToggle={(ev) => props.onToggle((ev.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer select-none text-xs text-white/80">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="font-semibold text-white/80">Workflow editor</div>
          <div className="text-[11px] text-white/50">Durable workflow DAGs + composer</div>
        </div>
      </summary>
      <div className="mt-3 grid gap-3">
        <div className="flex flex-wrap items-center gap-2">
          <input
            className="min-w-[260px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
            placeholder="workflow_id (e.g. wf_...)"
            value={workflowState.workflowId}
            onChange={(e) => workflowState.setWorkflowId(e.target.value)}
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={workflowState.workflowLookup.isPending || !workflowState.canLoad}
            onClick={() => workflowState.loadWorkflow(workflowState.workflowId)}
          >
            {workflowState.workflowLookup.isPending ? "Loading…" : "Load"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={workflowState.workflowLookup.isPending}
            onClick={() => workflowState.clearWorkflow()}
          >
            Clear
          </button>
        </div>
        {workflowState.copyNotice ? <div className="text-[10px] text-white/50">{workflowState.copyNotice}</div> : null}
        {workflowState.copyNotice ? (
          <div className="text-[10px] text-white/50">Tip: use the copy buttons in the workflow list rows for quick sharing.</div>
        ) : null}

        <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/70">
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={workflowState.includeResults}
              onChange={(e) => workflowState.setIncludeResults(e.target.checked)}
            />
            include results
          </label>
          <label className="flex items-center gap-1">
            <input
              type="checkbox"
              className="h-3 w-3"
              checked={workflowState.includeSpec}
              onChange={(e) => workflowState.setIncludeSpec(e.target.checked)}
            />
            include spec
          </label>
        </div>

        <WorkflowListSection
          baseUrl={props.baseUrl}
          normalizedListStatus={workflowState.normalizedListStatus}
          listStatus={workflowState.listStatus}
          setListStatus={workflowState.setListStatus}
          listLimit={workflowState.listLimit}
          setListLimit={workflowState.setListLimit}
          listFilter={workflowState.listFilter}
          setListFilter={workflowState.setListFilter}
          listAutoRefresh={workflowState.listAutoRefresh}
          setListAutoRefresh={workflowState.setListAutoRefresh}
          listQuery={workflowState.listQuery}
          filteredWorkflows={workflowState.filteredWorkflows}
          cancelBusyId={workflowState.cancelBusyId}
          onSelectWorkflow={(id) => {
            workflowState.setWorkflowId(id);
            workflowState.loadWorkflow(id);
          }}
          onCopyText={workflowState.copyText}
          onCancelWorkflow={workflowState.cancelWorkflow}
        />

        <WorkflowSchedulesSection
          baseUrl={props.baseUrl}
          normalizedScheduleStatus={workflowState.normalizedScheduleStatus}
          scheduleStatus={workflowState.scheduleStatus}
          setScheduleStatus={workflowState.setScheduleStatus}
          scheduleLimit={workflowState.scheduleLimit}
          setScheduleLimit={workflowState.setScheduleLimit}
          scheduleOffset={workflowState.scheduleOffset}
          setScheduleOffset={workflowState.setScheduleOffset}
          scheduleFilter={workflowState.scheduleFilter}
          setScheduleFilter={workflowState.setScheduleFilter}
          scheduleAutoRefresh={workflowState.scheduleAutoRefresh}
          setScheduleAutoRefresh={workflowState.setScheduleAutoRefresh}
          scheduleLimitValue={workflowState.scheduleLimitValue}
          scheduleOffsetValue={workflowState.scheduleOffsetValue}
          scheduleRunsLimit={workflowState.scheduleRunsLimit}
          setScheduleRunsLimit={workflowState.setScheduleRunsLimit}
          scheduleRunsOffset={workflowState.scheduleRunsOffset}
          setScheduleRunsOffset={workflowState.setScheduleRunsOffset}
          scheduleRunsStatus={workflowState.scheduleRunsStatus}
          setScheduleRunsStatus={workflowState.setScheduleRunsStatus}
          scheduleRunsErrorsOnly={workflowState.scheduleRunsErrorsOnly}
          setScheduleRunsErrorsOnly={workflowState.setScheduleRunsErrorsOnly}
          scheduleRunsFilter={workflowState.scheduleRunsFilter}
          setScheduleRunsFilter={workflowState.setScheduleRunsFilter}
          normalizedScheduleRunsStatus={workflowState.normalizedScheduleRunsStatus}
          scheduleRunsLimitValue={workflowState.scheduleRunsLimitValue}
          scheduleRunsOffsetValue={workflowState.scheduleRunsOffsetValue}
          scheduleCron={workflowState.scheduleCron}
          setScheduleCron={workflowState.setScheduleCron}
          scheduleSpec={workflowState.scheduleSpec}
          setScheduleSpec={workflowState.setScheduleSpec}
          scheduleId={workflowState.scheduleId}
          scheduleError={workflowState.scheduleError}
          setScheduleError={workflowState.setScheduleError}
          scheduleValidation={workflowState.scheduleValidation}
          setScheduleValidation={workflowState.setScheduleValidation}
          scheduleCronValidation={workflowState.scheduleCronValidation}
          setScheduleCronValidation={workflowState.setScheduleCronValidation}
          scheduleBusyId={workflowState.scheduleBusyId}
          scheduleCreateBusy={workflowState.scheduleCreateBusy}
          scheduleListQuery={workflowState.scheduleListQuery}
          scheduleRunsQuery={workflowState.scheduleRunsQuery}
          scheduleList={workflowState.scheduleList}
          scheduleRuns={workflowState.scheduleRuns}
          filteredScheduleList={workflowState.filteredScheduleList}
          filteredScheduleRuns={workflowState.filteredScheduleRuns}
          onCopyText={workflowState.copyText}
          onCopyJson={workflowState.copyJson}
          onDownloadJson={workflowState.downloadJson}
          onLoadScheduleRuns={workflowState.loadScheduleRuns}
          onLoadWorkflowFromRun={(id) => {
            workflowState.setWorkflowId(id);
            workflowState.loadWorkflow(id);
          }}
          onCreateSchedule={workflowState.createSchedule}
          onPauseSchedule={workflowState.pauseSchedule}
          onResumeSchedule={workflowState.resumeSchedule}
          onDeleteSchedule={workflowState.deleteSchedule}
          onLoadSpecFromWorkflow={workflowState.loadSpecFromWorkflow}
          scheduleCurlSnippet={workflowState.scheduleCurlSnippet}
          scheduleCreateCurlSnippet={workflowState.scheduleCreateCurlSnippet}
        />

        {workflowState.detailError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
            {workflowState.detailError}
          </div>
        ) : null}

        <WorkflowDetailSection
          detail={workflowState.detail}
          summary={workflowState.summary}
          tasks={workflowState.tasks}
          taskCounts={workflowState.taskCounts}
          graph={workflowState.graph}
          cancelBusyId={workflowState.cancelBusyId}
          workflowLookupPending={workflowState.workflowLookup.isPending}
          onReloadWorkflow={workflowState.loadWorkflow}
          onCancelWorkflow={workflowState.cancelWorkflow}
          onCopyText={workflowState.copyText}
          onTraceIdClick={props.onTraceIdClick}
        />

        <WorkflowComposer
          baseUrl={props.baseUrl}
          auth={props.auth}
          authKey={props.authKey}
          clientId={props.clientId}
          workflowDefaults={props.workflowDefaults}
          workflowTargets={props.workflowTargets}
          workflowBearerEnv={props.workflowBearerEnv}
          onSubmitted={(workflowId) => {
            workflowState.setWorkflowId(workflowId);
            workflowState.loadWorkflow(workflowId);
          }}
        />
      </div>
    </details>
  );
}
