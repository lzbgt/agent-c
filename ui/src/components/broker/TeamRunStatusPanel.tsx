import React from "react";
import type { ApiAuth } from "../../api";
import TeamRunStatusGoalSection from "./TeamRunStatusGoalSection";
import TeamRunStatusHandoffSection from "./TeamRunStatusHandoffSection";
import TeamRunStatusOverviewSection from "./TeamRunStatusOverviewSection";
import type {
  MemberSession,
  TeamRunHandoffEventRow,
  TeamRunLookupResult,
  TeamRunMemberJobSummaryRow,
  TeamRunRuntimeMemberRow,
} from "./teamRunStatusTypes";
import { normalizeRoleGraphEdges, normalizeRoleInstructionMap } from "./teamRunUtils";
import useTeamRunStatusState from "./useTeamRunStatusState";

export type TeamRunStatusPanelProps = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  teamId: string;
  runId: string;
  runLookupResult: TeamRunLookupResult | null;
  fmtTs: (ms?: number | null) => string;
  fmtSummary: (summary?: TeamRunMemberJobSummaryRow | null) => string;
  runtimeUpdateBusy: boolean;
  memberSessions: MemberSession[];
  onRuntimeMemberToggle: (member: TeamRunRuntimeMemberRow) => Promise<void> | void;
  onRuntimeMemberRemove: (member: TeamRunRuntimeMemberRow) => Promise<void> | void;
  onRefreshRun: (runId: string) => Promise<void> | void;
};

export default function TeamRunStatusPanel(props: TeamRunStatusPanelProps) {
  const run = props.runLookupResult;
  const runId = String(props.runId || run?.team_run_id || "").trim();
  const teamId = String(props.teamId || "").trim();
  const canWrite = props.canQuery && !!teamId && !!runId;

  const handoffEvents: TeamRunHandoffEventRow[] = run?.handoff_events ?? [];
  const roleInstructions = normalizeRoleInstructionMap(run?.role_instructions);
  const rolePromptMode = typeof run?.role_prompt_mode === "string" ? String(run.role_prompt_mode) : "";
  const roleGraphEdges = normalizeRoleGraphEdges(run?.role_graph);
  const roleGraphRoles = React.useMemo(() => {
    const set = new Set<string>();
    for (const edge of roleGraphEdges) {
      const from = String(edge.from_role || "").trim().toLowerCase();
      const to = String(edge.to_role || "").trim().toLowerCase();
      if (from) set.add(from);
      if (to) set.add(to);
    }
    for (const role of Object.keys(roleInstructions)) {
      const key = String(role || "").trim().toLowerCase();
      if (key) set.add(key);
    }
    for (const member of run?.members ?? []) {
      const role = String(member?.role || "").trim().toLowerCase();
      if (role) set.add(role);
    }
    for (const member of run?.runtime_members ?? []) {
      const role = String(member?.role || "").trim().toLowerCase();
      if (role) set.add(role);
    }
    return Array.from(set).filter(Boolean).sort();
  }, [roleGraphEdges, roleInstructions, run?.members, run?.runtime_members]);
  const sharedMemoryScope = run?.shared_memory_scope_id ? String(run.shared_memory_scope_id) : "";
  const sharedMemoryMode = run?.shared_memory_mode ? String(run.shared_memory_mode) : "";
  const autoAllocateRoles = run?.auto_allocate_roles === true;
  const autoAllocateAllocated = run?.auto_allocate_allocated_roles ?? [];
  const autoAllocateMissing = run?.auto_allocate_missing_roles ?? [];
  const autoAllocateWarning = run?.auto_allocate_warning ? String(run.auto_allocate_warning) : "";
  const roleInstructionCount = Object.keys(roleInstructions).length;
  const statusState = useTeamRunStatusState({
    base: props.base,
    auth: props.auth,
    canWrite,
    teamId,
    runId,
    run,
    handoffEvents,
    onRefreshRun: props.onRefreshRun,
  });

  if (!run) {
    return <div className="text-[11px] text-white/50">No run loaded yet.</div>;
  }

  return (
    <div className="grid gap-2">
      <TeamRunStatusOverviewSection
        run={run}
        fmtTs={props.fmtTs}
        fmtSummary={props.fmtSummary}
        roleGraphEdges={roleGraphEdges}
        roleGraphRoles={roleGraphRoles}
        rolePromptMode={rolePromptMode}
        roleInstructionCount={roleInstructionCount}
        sharedMemoryScope={sharedMemoryScope}
        sharedMemoryMode={sharedMemoryMode}
        autoAllocateRoles={autoAllocateRoles}
        autoAllocateAllocated={autoAllocateAllocated}
        autoAllocateMissing={autoAllocateMissing}
        autoAllocateWarning={autoAllocateWarning}
        runtimeUpdateBusy={props.runtimeUpdateBusy}
        canQuery={props.canQuery}
        memberSessions={props.memberSessions}
        onRuntimeMemberToggle={props.onRuntimeMemberToggle}
        onRuntimeMemberRemove={props.onRuntimeMemberRemove}
      />
      <TeamRunStatusGoalSection
        run={run}
        goalContract={statusState.goalContract}
        goalEventRows={statusState.goalEventRows}
        canWrite={canWrite}
        goalContractGoal={statusState.goalContractGoal}
        setGoalContractGoal={statusState.setGoalContractGoal}
        goalContractCriteria={statusState.goalContractCriteria}
        setGoalContractCriteria={statusState.setGoalContractCriteria}
        goalContractConstraints={statusState.goalContractConstraints}
        setGoalContractConstraints={statusState.setGoalContractConstraints}
        goalEventType={statusState.goalEventType}
        setGoalEventType={statusState.setGoalEventType}
        goalEventMessage={statusState.goalEventMessage}
        setGoalEventMessage={statusState.setGoalEventMessage}
        goalEventData={statusState.goalEventData}
        setGoalEventData={statusState.setGoalEventData}
        goalUpdateBusy={statusState.goalUpdateBusy}
        goalUpdateError={statusState.goalUpdateError}
        goalUpdateNote={statusState.goalUpdateNote}
        fmtTs={props.fmtTs}
        onGoalContractUpdate={statusState.handleGoalContractUpdate}
        onGoalEvent={statusState.handleGoalEvent}
      />
      <TeamRunStatusHandoffSection
        canWrite={canWrite}
        handoffEventRows={statusState.handoffEventRows}
        handoffLatestById={statusState.handoffLatestById}
        handoffKind={statusState.handoffKind}
        setHandoffKind={statusState.setHandoffKind}
        handoffFromRole={statusState.handoffFromRole}
        setHandoffFromRole={statusState.setHandoffFromRole}
        handoffToRole={statusState.handoffToRole}
        setHandoffToRole={statusState.setHandoffToRole}
        handoffReason={statusState.handoffReason}
        setHandoffReason={statusState.setHandoffReason}
        handoffMessage={statusState.handoffMessage}
        setHandoffMessage={statusState.setHandoffMessage}
        handoffSourceDeployment={statusState.handoffSourceDeployment}
        setHandoffSourceDeployment={statusState.setHandoffSourceDeployment}
        handoffSourceSession={statusState.handoffSourceSession}
        setHandoffSourceSession={statusState.setHandoffSourceSession}
        handoffTargetDeployment={statusState.handoffTargetDeployment}
        setHandoffTargetDeployment={statusState.setHandoffTargetDeployment}
        handoffTargetSession={statusState.handoffTargetSession}
        setHandoffTargetSession={statusState.setHandoffTargetSession}
        handoffData={statusState.handoffData}
        setHandoffData={statusState.setHandoffData}
        handoffBusy={statusState.handoffBusy}
        handoffError={statusState.handoffError}
        handoffNote={statusState.handoffNote}
        fmtTs={props.fmtTs}
        onHandoffEvent={statusState.handleHandoffEvent}
        onHandoffTransition={statusState.handleHandoffTransition}
      />
    </div>
  );
}
