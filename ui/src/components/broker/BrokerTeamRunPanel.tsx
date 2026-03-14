import React from "react";
import FieldLabel from "../FieldLabel";
import TeamRunCreatePanel from "./TeamRunCreatePanel";
import TeamRunModeratorPanel from "./TeamRunModeratorPanel";
import TeamRunOpsPanel from "./TeamRunOpsPanel";
import TeamRunRecentRunsPanel from "./TeamRunRecentRunsPanel";
import TeamRunStatusPanel from "./TeamRunStatusPanel";
import { fmtSummary, fmtTs } from "./teamRunUtils";
import useBrokerTeamRunControlState from "./useBrokerTeamRunControlState";
import useBrokerTeamRunCreateState from "./useBrokerTeamRunCreateState";
import type { BrokerEventRow, TeamMemberRow, TeamQuorumRuleRow } from "./types";
import type { ApiAuth } from "../../api";

export type BrokerTeamRunPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  members: TeamMemberRow[];
  rules: TeamQuorumRuleRow[];
  quorumEvents?: BrokerEventRow[];
  teamMeta?: Record<string, any> | null;
  onMembersRefresh?: (teamId: string) => Promise<void> | void;
  onTeamSelect?: (teamId: string) => void;
};

type RunSectionProps = {
  title: string;
  defaultOpen?: boolean;
  children: React.ReactNode;
};

function RunSection({ title, defaultOpen = false, children }: RunSectionProps) {
  const [open, setOpen] = React.useState<boolean>(defaultOpen);
  return (
    <details
      className="rounded-md border border-white/10 bg-black/20 p-2"
      open={open}
      onToggle={(event) => setOpen((event.currentTarget as HTMLDetailsElement).open)}
    >
      <summary className="cursor-pointer text-[11px] font-semibold text-white/70">{title}</summary>
      <div className="mt-2 grid gap-2">{children}</div>
    </details>
  );
}

export default function BrokerTeamRunPanel(props: BrokerTeamRunPanelProps) {
  const teamIdTrimmed = String(props.teamId || "").trim();
  const membersList = Array.isArray(props.members) ? props.members : [];
  const rulesList = Array.isArray(props.rules) ? props.rules : [];
  const teamMetaObj = props.teamMeta && typeof props.teamMeta === "object" ? props.teamMeta : null;

  const createState = useBrokerTeamRunCreateState({
    base: props.base,
    auth: props.auth,
    canQuery: props.canQuery,
    teamIdTrimmed,
    membersList,
    teamMeta: teamMetaObj,
    onMembersRefresh: props.onMembersRefresh,
  });

  const controlState = useBrokerTeamRunControlState({
    base: props.base,
    auth: props.auth,
    canQuery: props.canQuery,
    teamIdTrimmed,
    quorumEvents: props.quorumEvents,
    runResult: createState.runResult,
    runRuntimeMembersJson: createState.runRuntimeMembersJson,
    setRunRuntimeMembersJson: createState.setRunRuntimeMembersJson,
  });

  return (
    <div className="mt-4 grid gap-3">
      <TeamRunCreatePanel
        canQuery={props.canQuery}
        teamId={teamIdTrimmed}
        runPrompt={createState.runPrompt}
        setRunPrompt={createState.setRunPrompt}
        runModel={createState.runModel}
        setRunModel={createState.setRunModel}
        runTools={createState.runTools}
        setRunTools={createState.setRunTools}
        runRole={createState.runRole}
        setRunRole={createState.setRunRole}
        runRoles={createState.runRoles}
        setRunRoles={createState.setRunRoles}
        runConcurrency={createState.runConcurrency}
        setRunConcurrency={createState.setRunConcurrency}
        runTimeoutMs={createState.runTimeoutMs}
        setRunTimeoutMs={createState.setRunTimeoutMs}
        runSharedMemoryScope={createState.runSharedMemoryScope}
        setRunSharedMemoryScope={createState.setRunSharedMemoryScope}
        runSharedMemoryMode={createState.runSharedMemoryMode}
        setRunSharedMemoryMode={createState.setRunSharedMemoryMode}
        runAutoAllocateRoles={createState.runAutoAllocateRoles}
        setRunAutoAllocateRoles={createState.setRunAutoAllocateRoles}
        runAutoAllocateMaxMembers={createState.runAutoAllocateMaxMembers}
        setRunAutoAllocateMaxMembers={createState.setRunAutoAllocateMaxMembers}
        runMode={createState.runMode}
        setRunMode={createState.setRunMode}
        runQuorumMode={createState.runQuorumMode}
        setRunQuorumMode={createState.setRunQuorumMode}
        runBusy={createState.runBusy}
        onCreateRun={() => void createState.handleCreateRun(controlState.setRunLookupId)}
        runOverridesMode={createState.runOverridesMode}
        setRunOverridesMode={createState.setRunOverridesMode}
        runMemberOverridesJson={createState.runMemberOverridesJson}
        setRunMemberOverridesJson={createState.setRunMemberOverridesJson}
        onSeedExplicitOverrides={createState.handleSeedExplicitOverrides}
        runRoleOverridesJson={createState.runRoleOverridesJson}
        setRunRoleOverridesJson={createState.setRunRoleOverridesJson}
        teamRoleOverrideKeys={createState.teamRoleOverrideKeys}
        onSeedRoleOverrides={createState.handleSeedRoleOverrides}
        runRoleInstructionsOverride={createState.runRoleInstructionsOverride}
        setRunRoleInstructionsOverride={createState.setRunRoleInstructionsOverride}
        runRoleInstructions={createState.runRoleInstructions}
        setRunRoleInstructions={createState.setRunRoleInstructions}
        runRolePromptMode={createState.runRolePromptMode}
        setRunRolePromptMode={createState.setRunRolePromptMode}
        teamRoleInstructionKeys={createState.teamRoleInstructionKeys}
        onSeedRoleInstructions={createState.handleSeedRoleInstructions}
        runRolePlanOptions={createState.runRolePlanOptions}
        runRuntimeMembersJson={createState.runRuntimeMembersJson}
        setRunRuntimeMembersJson={createState.setRunRuntimeMembersJson}
        runtimeMembersPreview={createState.runtimeMembersPreview}
        runtimeTeamDiff={createState.runtimeTeamDiff}
        handleSetAllRuntimeStatus={createState.handleSetAllRuntimeStatus}
        handleRemovePausedRuntimeMembers={createState.handleRemovePausedRuntimeMembers}
        handleCompactRuntimeMembers={createState.handleCompactRuntimeMembers}
        handleCopyRuntimeMembers={createState.handleCopyRuntimeMembers}
        runtimeImportRef={createState.runtimeImportRef}
        runtimeImportMerge={createState.runtimeImportMerge}
        setRuntimeImportMerge={createState.setRuntimeImportMerge}
        handleDownloadRuntimeMembers={createState.handleDownloadRuntimeMembers}
        handleExportTeamMembers={createState.handleExportTeamMembers}
        handleImportRuntimeMembers={createState.handleImportRuntimeMembers}
        handleToggleRuntimeMemberStatus={createState.handleToggleRuntimeMemberStatus}
        handleRemoveRuntimeMember={createState.handleRemoveRuntimeMember}
        runtimeMemberId={createState.runtimeMemberId}
        setRuntimeMemberId={createState.setRuntimeMemberId}
        runtimeMemberAgentId={createState.runtimeMemberAgentId}
        setRuntimeMemberAgentId={createState.setRuntimeMemberAgentId}
        runtimeMemberRole={createState.runtimeMemberRole}
        setRuntimeMemberRole={createState.setRuntimeMemberRole}
        runtimeAgentOptions={createState.runtimeAgentOptions}
        runtimeAgentsBusy={createState.runtimeAgentsBusy}
        refreshRuntimeAgents={createState.refreshRuntimeAgents}
        handleAddConnectedAgents={createState.handleAddConnectedAgents}
        handleAllocateRoleRuntimeMembers={createState.handleAllocateRoleRuntimeMembers}
        runtimeSaveBusy={createState.runtimeSaveBusy}
        handleSaveRuntimeMembers={createState.handleSaveRuntimeMembers}
        runtimeAgentsError={createState.runtimeAgentsError}
        runtimeSaveError={createState.runtimeSaveError}
        runtimeSavePreview={createState.runtimeSavePreview}
        handleFixInvalidRuntimeMembers={createState.handleFixInvalidRuntimeMembers}
        runtimeMemberDeploymentId={createState.runtimeMemberDeploymentId}
        setRuntimeMemberDeploymentId={createState.setRuntimeMemberDeploymentId}
        runtimeAgentDeployments={createState.runtimeAgentDeployments}
        runtimeMemberCapabilities={createState.runtimeMemberCapabilities}
        setRuntimeMemberCapabilities={createState.setRuntimeMemberCapabilities}
        runtimeMemberBackendLabel={createState.runtimeMemberBackendLabel}
        setRuntimeMemberBackendLabel={createState.setRuntimeMemberBackendLabel}
        runtimeMemberModel={createState.runtimeMemberModel}
        setRuntimeMemberModel={createState.setRuntimeMemberModel}
        runtimeMemberBaseUrl={createState.runtimeMemberBaseUrl}
        setRuntimeMemberBaseUrl={createState.setRuntimeMemberBaseUrl}
        runtimeMemberSummaryModel={createState.runtimeMemberSummaryModel}
        setRuntimeMemberSummaryModel={createState.setRuntimeMemberSummaryModel}
        runtimeMemberTools={createState.runtimeMemberTools}
        setRuntimeMemberTools={createState.setRuntimeMemberTools}
        runtimeMemberTimeoutMs={createState.runtimeMemberTimeoutMs}
        setRuntimeMemberTimeoutMs={createState.setRuntimeMemberTimeoutMs}
        handleAddRuntimeMember={createState.handleAddRuntimeMember}
        runApprovalMemberId={createState.runApprovalMemberId}
        setRunApprovalMemberId={createState.setRunApprovalMemberId}
        runApprovalDecision={createState.runApprovalDecision}
        setRunApprovalDecision={createState.setRunApprovalDecision}
        runApprovalRuleId={createState.runApprovalRuleId}
        setRunApprovalRuleId={createState.setRunApprovalRuleId}
        runApprovalReason={createState.runApprovalReason}
        setRunApprovalReason={createState.setRunApprovalReason}
        runApprovals={createState.runApprovals}
        setRunApprovals={createState.setRunApprovals}
        handleAddRunApproval={createState.handleAddRunApproval}
        runError={createState.runError}
        runQuorum={createState.runQuorum}
        runResult={createState.runResult}
      />

      <RunSection title="Recent runs" defaultOpen={false}>
        <TeamRunRecentRunsPanel
          canQuery={props.canQuery}
          teamId={teamIdTrimmed}
          recentRunsLimit={controlState.recentRunsLimit}
          setRecentRunsLimit={controlState.setRecentRunsLimit}
          recentRunsStatus={controlState.recentRunsStatus}
          setRecentRunsStatus={controlState.setRecentRunsStatus}
          recentRunsLive={controlState.recentRunsLive}
          setRecentRunsLive={controlState.setRecentRunsLive}
          recentRunsBusy={controlState.recentRunsBusy}
          onRefresh={controlState.loadRecentRuns}
          recentRunsError={controlState.recentRunsError}
          recentRunsItems={controlState.recentRunsItems}
          fmtTs={fmtTs}
          fmtSummary={fmtSummary}
          onLoadRun={(runId) => {
            controlState.setRunLookupId(runId);
            void controlState.fetchRunStatus(runId);
          }}
        />
      </RunSection>

      <RunSection title="Run status & control" defaultOpen={true}>
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Run ID</FieldLabel>
          <input
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            value={controlState.runLookupId}
            onChange={(event) => controlState.setRunLookupId(event.target.value)}
            placeholder="team_run_id"
          />
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!props.canQuery || !teamIdTrimmed || controlState.runLookupBusy}
            onClick={() => void controlState.handleRunLookup()}
          >
            {controlState.runLookupBusy ? "Loading…" : "Get status"}
          </button>
          <button
            className="rounded-md border border-amber-400/40 bg-amber-500/10 px-3 py-1 text-[11px] text-amber-100 hover:bg-amber-500/20 disabled:opacity-50"
            type="button"
            disabled={!props.canQuery || controlState.runCancelBusy || !controlState.resolveRunId()}
            onClick={() => void controlState.handleRunCancel()}
          >
            {controlState.runCancelBusy ? "Cancelling…" : "Cancel run"}
          </button>
        </div>
        <div className="flex flex-wrap gap-3">
          <label className="flex items-center gap-2 text-[11px] text-white/60">
            <input
              type="checkbox"
              className="rounded border-white/20 bg-black/40"
              checked={controlState.autoResumeRunLookup}
              onChange={(event) => controlState.setAutoResumeRunLookup(event.target.checked)}
            />
            Auto resume saved run on reload
          </label>
          <label className="flex items-center gap-2 text-[11px] text-white/60">
            <input
              type="checkbox"
              className="rounded border-white/20 bg-black/40"
              checked={controlState.autoRefreshRunLookup}
              onChange={(event) => controlState.setAutoRefreshRunLookup(event.target.checked)}
            />
            Auto refresh run status on team run events
          </label>
        </div>
        {controlState.runLookupError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {controlState.runLookupError}
          </div>
        ) : null}
        {controlState.runCancelError ? (
          <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            {controlState.runCancelError}
          </div>
        ) : null}
        {controlState.runCancelNote ? (
          <div className="rounded-md border border-amber-400/20 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-100">
            {controlState.runCancelNote}
          </div>
        ) : null}
        <TeamRunStatusPanel
          base={props.base}
          auth={props.auth}
          canQuery={props.canQuery}
          teamId={teamIdTrimmed}
          runId={controlState.resolveRunId()}
          runLookupResult={controlState.runLookupResult}
          fmtTs={fmtTs}
          fmtSummary={fmtSummary}
          runtimeUpdateBusy={controlState.runtimeUpdateBusy}
          memberSessions={controlState.runMemberSessions}
          onRuntimeMemberToggle={controlState.handleRuntimeMemberToggle}
          onRuntimeMemberRemove={controlState.handleRuntimeMemberRemove}
          onRefreshRun={controlState.fetchRunStatus}
        />
      </RunSection>

      <RunSection title="Moderator actions" defaultOpen={false}>
        <TeamRunModeratorPanel
          canQuery={props.canQuery}
          runId={controlState.resolveRunId()}
          memberSessions={controlState.runMemberSessions}
          roleOptions={controlState.runRoleOptions}
          memberOptions={controlState.runMemberOptions}
          agentOptions={controlState.runAgentOptions}
          directive={controlState.moderatorDirective}
          directiveScope={controlState.moderatorDirectiveScope}
          taskTitle={controlState.moderatorTaskTitle}
          taskDetail={controlState.moderatorTaskDetail}
          taskStatus={controlState.moderatorTaskStatus}
          targetRoles={controlState.moderatorTargetRoles}
          targetMembers={controlState.moderatorTargetMembers}
          targetAgents={controlState.moderatorTargetAgents}
          assignees={controlState.moderatorAssignees}
          appendToSession={controlState.moderatorAppendToSession}
          busy={controlState.moderatorBusy}
          error={controlState.moderatorError}
          success={controlState.moderatorSuccess}
          events={controlState.moderatorEvents}
          eventsBusy={controlState.moderatorEventsBusy}
          eventsError={controlState.moderatorEventsError}
          eventsTypes={controlState.moderatorEventsTypes}
          eventsMaxBytes={controlState.moderatorEventsMaxBytes}
          eventsLimit={controlState.moderatorEventsLimit}
          eventsExpanded={controlState.moderatorEventsExpanded}
          onDirectiveChange={controlState.setModeratorDirective}
          onDirectiveScopeChange={controlState.setModeratorDirectiveScope}
          onTaskTitleChange={controlState.setModeratorTaskTitle}
          onTaskDetailChange={controlState.setModeratorTaskDetail}
          onTaskStatusChange={controlState.setModeratorTaskStatus}
          onTargetRolesChange={controlState.setModeratorTargetRoles}
          onTargetMembersChange={controlState.setModeratorTargetMembers}
          onTargetAgentsChange={controlState.setModeratorTargetAgents}
          onAssigneesChange={controlState.setModeratorAssignees}
          onAppendToSessionChange={controlState.setModeratorAppendToSession}
          onPublishDirective={() => void controlState.handleModeratorDirectivePublish()}
          onPublishTask={() => void controlState.handleModeratorTaskPublish()}
          onEventsTypesChange={controlState.setModeratorEventsTypes}
          onEventsMaxBytesChange={controlState.setModeratorEventsMaxBytes}
          onEventsLimitChange={controlState.setModeratorEventsLimit}
          onEventsLoad={() => void controlState.handleModeratorEventsLoad()}
          onEventsToggleExpanded={() => controlState.setModeratorEventsExpanded((prev) => !prev)}
        />
      </RunSection>

      <RunSection title="Operations & approvals" defaultOpen={false}>
        <TeamRunOpsPanel
          canQuery={props.canQuery}
          teamId={teamIdTrimmed}
          runtimeUpdateMode={controlState.runtimeUpdateMode}
          setRuntimeUpdateMode={controlState.setRuntimeUpdateMode}
          runtimeUpdateBusy={controlState.runtimeUpdateBusy}
          runtimeUpdateError={controlState.runtimeUpdateError}
          runtimeUpdateNote={controlState.runtimeUpdateNote}
          onRuntimeMembersLoadFromRun={controlState.handleRuntimeMembersLoadFromRun}
          onRuntimeMembersUpdate={() => void controlState.handleRuntimeMembersUpdate()}
          approvalsLastSyncMs={controlState.approvalsLastSyncMs}
          fmtTs={fmtTs}
          quorumRequestRows={controlState.quorumRequestRows}
          onTeamSelect={props.onTeamSelect}
          setApprovalRunId={controlState.setApprovalRunId}
          setRunLookupId={controlState.setRunLookupId}
          approvalRunId={controlState.approvalRunId}
          approvalsBusy={controlState.approvalsBusy}
          onApprovalsRefresh={() => void controlState.handleApprovalsRefresh()}
          membersList={membersList}
          approvalMemberId={controlState.approvalMemberId}
          setApprovalMemberId={controlState.setApprovalMemberId}
          approvalDecision={controlState.approvalDecision}
          setApprovalDecision={controlState.setApprovalDecision}
          approvalRuleId={controlState.approvalRuleId}
          setApprovalRuleId={controlState.setApprovalRuleId}
          rulesList={rulesList}
          approvalReason={controlState.approvalReason}
          setApprovalReason={controlState.setApprovalReason}
          onApprovalSubmit={() => void controlState.handleApprovalSubmit()}
          approvalsError={controlState.approvalsError}
          approvals={controlState.approvals}
        />
      </RunSection>
    </div>
  );
}
