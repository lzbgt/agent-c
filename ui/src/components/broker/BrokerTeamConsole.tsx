import React from "react";
import type { ApiAuth } from "../../api";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import FieldLabel from "../FieldLabel";
import BrokerOrchestratorRunPanel from "./BrokerOrchestratorRunPanel";
import BrokerOrchestratorSpawnPanel from "./BrokerOrchestratorSpawnPanel";
import BrokerTeamGuidancePanel from "./BrokerTeamGuidancePanel";
import BrokerTeamMembersPanel from "./BrokerTeamMembersPanel";
import BrokerTeamRunPanel from "./BrokerTeamRunPanel";
import SectionCard from "./BrokerTeamSectionCard";
import BrokerTeamSettingsPanel from "./BrokerTeamSettingsPanel";
import BrokerTeamSetupPanel from "./BrokerTeamSetupPanel";
import useBrokerTeamControlState from "./useBrokerTeamControlState";
import useBrokerTeamEventsState from "./useBrokerTeamEventsState";
import useBrokerTeamSetupState from "./useBrokerTeamSetupState";
import { fmtTs } from "./teamRunUtils";
import type { BrokerEventRow } from "./types";

export type BrokerTeamConsoleProps = {
  base: string;
  auth: ApiAuth;
  authKey: string;
  clientId: string;
  quorumEvents?: BrokerEventRow[];
  mode?: "full" | "inline";
  forcedTab?: "run" | "members" | "setup" | "settings" | "advanced";
};

export default function BrokerTeamConsole(props: BrokerTeamConsoleProps) {
  const authToken = props.auth?.token ? String(props.auth.token).trim() : "";
  const useCookieAuth = props.auth?.mode === "broker" && props.auth.useCookieAuth === true;
  const canQuery = props.base.length > 0 && (authToken.length > 0 || useCookieAuth);
  const mode = props.mode ?? "full";

  const setupState = useBrokerTeamSetupState({
    base: props.base,
    auth: props.auth,
    canQuery,
  });

  const teamTabs = React.useMemo(
    () => [
      { id: "run", label: "Run" },
      { id: "members", label: "Members" },
      { id: "setup", label: "Setup" },
      { id: "settings", label: "Settings" },
      { id: "advanced", label: "Advanced" },
    ],
    [],
  );
  const [teamTab, setTeamTab] = useLocalStorageState<string>("agentui.teamTab", "run");
  const teamTabIds = React.useMemo(() => new Set(teamTabs.map((tab) => tab.id)), [teamTabs]);
  const forcedTab = props.forcedTab && teamTabIds.has(props.forcedTab) ? props.forcedTab : "";
  const activeTab = forcedTab || teamTab;

  React.useEffect(() => {
    if (!teamTabIds.has(teamTab)) setTeamTab("run");
  }, [setTeamTab, teamTab, teamTabIds]);

  React.useEffect(() => {
    if (setupState.teamList.length === 0 && teamTab !== "setup") {
      setTeamTab("setup");
    }
  }, [setTeamTab, setupState.teamList.length, teamTab]);

  const controlState = useBrokerTeamControlState({
    base: props.base,
    auth: props.auth,
    canQuery,
    teamIdTrimmed: setupState.teamIdTrimmed,
    memberAgents: setupState.memberAgents,
  });

  const teamEventsState = useBrokerTeamEventsState({
    base: props.base,
    auth: props.auth,
    authKey: props.authKey,
    clientId: props.clientId,
    canQuery,
    teamIdTrimmed: setupState.teamIdTrimmed,
    quorumEvents: props.quorumEvents,
  });

  return (
    <section className={`rounded-md border border-white/10 bg-black/20 ${mode === "inline" ? "p-2" : "p-3"}`}>
      <div className="mb-2 flex items-center justify-between gap-2">
        <div className="text-xs font-semibold text-white/80">Teams</div>
        <div className="flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || setupState.teamsBusy}
            onClick={() => void setupState.refreshTeams()}
          >
            {setupState.teamsBusy ? "Loading…" : "Refresh"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={!canQuery || setupState.teamsBusy || !setupState.teamIdTrimmed}
            onClick={() => void setupState.handleDeleteTeam()}
          >
            Delete
          </button>
        </div>
      </div>
      <div className="mb-2 text-[11px] text-white/50">Manage team members, quorum rules, and team runs.</div>

      <div className="grid gap-2">
        <div className="flex flex-wrap items-center gap-2">
          <FieldLabel>Team</FieldLabel>
          <select
            className="min-w-[200px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
            data-testid="team-select"
            value={setupState.teamIdTrimmed}
            onChange={(event) => setupState.setTeamId(event.target.value)}
            disabled={setupState.teamList.length === 0}
          >
            {setupState.teamList.length === 0 ? <option value="">(no teams)</option> : null}
            {setupState.teamList.map((team) => (
              <option key={String(team?.team_id)} value={String(team?.team_id)}>
                {String(team?.display_name || team?.team_id || "")}
              </option>
            ))}
          </select>
        </div>
        {controlState.teamDetails ? (
          <div className="text-[11px] text-white/50">
            owner {String(controlState.teamDetails?.owner_sub || "unknown")}
            {controlState.teamDetails?.created_unix_ms ? ` · ${fmtTs(controlState.teamDetails.created_unix_ms)}` : ""}
          </div>
        ) : null}
      </div>

      {forcedTab ? null : (
        <div className="mt-3 flex flex-wrap items-center gap-2" data-testid="team-tabs">
          {teamTabs.map((tab) => {
            const active = tab.id === teamTab;
            return (
              <button
                key={tab.id}
                data-testid={`team-tab-${tab.id}`}
                className={`rounded-md px-3 py-1.5 text-xs ${
                  active ? "bg-indigo-500/20 text-indigo-100" : "bg-black/20 text-white/70 hover:bg-black/30"
                }`}
                type="button"
                onClick={() => setTeamTab(tab.id)}
              >
                {tab.label}
              </button>
            );
          })}
        </div>
      )}

      {activeTab === "run" ? (
        <SectionCard title="Team run" description="Start and monitor team runs." defaultOpen={true}>
          <BrokerTeamRunPanel
            base={props.base}
            auth={props.auth}
            canQuery={canQuery}
            teamId={setupState.teamIdTrimmed}
            members={controlState.membersList}
            rules={controlState.rulesList}
            quorumEvents={teamEventsState.mergedTeamEvents}
            teamMeta={
              controlState.teamDetails?.meta && typeof controlState.teamDetails.meta === "object"
                ? (controlState.teamDetails.meta as Record<string, any>)
                : null
            }
            onMembersRefresh={controlState.refreshMembers}
            onTeamSelect={setupState.setTeamId}
          />
        </SectionCard>
      ) : null}

      {activeTab === "settings" ? (
        <SectionCard title="Team settings" description="Name, tags, shared memory, and role graph defaults.">
          <BrokerTeamSettingsPanel
            canQuery={canQuery}
            teamId={setupState.teamIdTrimmed}
            teamDetails={controlState.teamDetails}
            teamEditName={controlState.teamEditName}
            teamEditTags={controlState.teamEditTags}
            teamEditPolicyRef={controlState.teamEditPolicyRef}
            teamEditSharedScope={controlState.teamEditSharedScope}
            teamEditSharedMode={controlState.teamEditSharedMode}
            teamEditMetaJson={controlState.teamEditMetaJson}
            teamEditRoleOverridesJson={controlState.teamEditRoleOverridesJson}
            teamRoleInstructions={controlState.teamRoleInstructions}
            teamRolePromptMode={controlState.teamRolePromptMode}
            teamRoleGraphEdges={controlState.teamRoleGraphEdges}
            rolePlanOptions={controlState.rolePlanOptions}
            teamEditBusy={controlState.teamEditBusy}
            teamEditError={controlState.teamEditError}
            onLoadTeamEdits={() => controlState.loadTeamEditsFromDetails(controlState.teamDetails)}
            onTeamEditNameChange={controlState.setTeamEditName}
            onTeamEditTagsChange={controlState.setTeamEditTags}
            onTeamEditPolicyRefChange={controlState.setTeamEditPolicyRef}
            onTeamEditSharedScopeChange={controlState.setTeamEditSharedScope}
            onTeamEditSharedModeChange={controlState.setTeamEditSharedMode}
            onTeamEditMetaJsonChange={controlState.setTeamEditMetaJson}
            onTeamEditRoleOverridesJsonChange={controlState.setTeamEditRoleOverridesJson}
            onRoleInstructionsChange={controlState.handleRoleInstructionsChange}
            onRolePromptModeChange={controlState.handleRolePromptModeChange}
            onRoleGraphEdgesChange={controlState.handleRoleGraphEdgesChange}
            onUpdateTeam={() => void controlState.handleUpdateTeam()}
          />
        </SectionCard>
      ) : null}

      {activeTab === "setup" ? (
        <BrokerTeamSetupPanel
          canQuery={canQuery}
          teamsBusy={setupState.teamsBusy}
          memberAgentsBusy={setupState.memberAgentsBusy}
          quickTeamName={setupState.quickTeamName}
          quickTeamId={setupState.quickTeamId}
          quickTeamGoal={setupState.quickTeamGoal}
          quickTemplate={setupState.quickTemplate}
          quickMembers={setupState.quickMembers}
          quickBuilderBusy={setupState.quickBuilderBusy}
          quickBuilderError={setupState.quickBuilderError}
          providerDefaults={setupState.providerDefaults}
          providerModelDefaults={setupState.providerModelDefaults}
          memberAgents={setupState.memberAgents || []}
          newTeamId={setupState.newTeamId}
          newTeamName={setupState.newTeamName}
          onQuickTeamNameChange={setupState.setQuickTeamName}
          onQuickTeamIdChange={setupState.setQuickTeamId}
          onQuickTeamGoalChange={setupState.setQuickTeamGoal}
          onQuickMemberUpdate={setupState.handleQuickMemberUpdate}
          onQuickAddMember={setupState.handleQuickAddMember}
          onQuickRemoveMember={setupState.handleQuickRemoveMember}
          onQuickCreateTeam={() =>
            void setupState.handleQuickCreateTeam({
              onCreated: async (teamId) => {
                await controlState.refreshTeamDetails(teamId);
                await controlState.refreshMembers(teamId);
              },
            })
          }
          onQuickApplyTemplate={setupState.handleQuickBuilderApplyTemplate}
          onRefreshMemberAgents={() => void setupState.refreshMemberAgents()}
          onNewTeamIdChange={setupState.setNewTeamId}
          onNewTeamNameChange={setupState.setNewTeamName}
          onCreateTeam={() => void setupState.handleCreateTeam()}
        />
      ) : null}

      {activeTab === "members" ? (
        <BrokerTeamMembersPanel
          canQuery={canQuery}
          teamIdTrimmed={setupState.teamIdTrimmed}
          membersBusy={controlState.membersBusy}
          membersError={controlState.membersError}
          members={controlState.members}
          memberAgentsBusy={setupState.memberAgentsBusy}
          memberAgentsError={setupState.memberAgentsError}
          memberAgentOptions={controlState.memberAgentOptions}
          memberAgentDeployments={controlState.memberAgentDeployments}
          memberEditAgentDeployments={controlState.memberEditAgentDeployments}
          memberId={controlState.memberId}
          memberRole={controlState.memberRole}
          memberStatus={controlState.memberStatus}
          memberWeight={controlState.memberWeight}
          memberCapabilities={controlState.memberCapabilities}
          memberAgentId={controlState.memberAgentId}
          memberDeploymentId={controlState.memberDeploymentId}
          memberBackendLabel={controlState.memberBackendLabel}
          memberModel={controlState.memberModel}
          memberBaseUrl={controlState.memberBaseUrl}
          memberSummaryModel={controlState.memberSummaryModel}
          memberTools={controlState.memberTools}
          memberTimeoutMs={controlState.memberTimeoutMs}
          memberEditId={controlState.memberEditId}
          memberEditRole={controlState.memberEditRole}
          memberEditStatus={controlState.memberEditStatus}
          memberEditWeight={controlState.memberEditWeight}
          memberEditCapabilities={controlState.memberEditCapabilities}
          memberEditMetaJson={controlState.memberEditMetaJson}
          memberEditBackendLabel={controlState.memberEditBackendLabel}
          memberEditModel={controlState.memberEditModel}
          memberEditBaseUrl={controlState.memberEditBaseUrl}
          memberEditSummaryModel={controlState.memberEditSummaryModel}
          memberEditTools={controlState.memberEditTools}
          memberEditTimeoutMs={controlState.memberEditTimeoutMs}
          memberEditAgentId={controlState.memberEditAgentId}
          memberEditDeploymentId={controlState.memberEditDeploymentId}
          memberEditBusy={controlState.memberEditBusy}
          memberEditError={controlState.memberEditError}
          onMemberIdChange={controlState.setMemberId}
          onMemberRoleChange={controlState.setMemberRole}
          onMemberStatusChange={controlState.setMemberStatus}
          onMemberWeightChange={controlState.setMemberWeight}
          onMemberCapabilitiesChange={controlState.setMemberCapabilities}
          onMemberAgentIdChange={controlState.setMemberAgentId}
          onMemberDeploymentIdChange={controlState.setMemberDeploymentId}
          onMemberBackendLabelChange={controlState.setMemberBackendLabel}
          onMemberModelChange={controlState.setMemberModel}
          onMemberBaseUrlChange={controlState.setMemberBaseUrl}
          onMemberSummaryModelChange={controlState.setMemberSummaryModel}
          onMemberToolsChange={controlState.setMemberTools}
          onMemberTimeoutMsChange={controlState.setMemberTimeoutMs}
          onMemberEditRoleChange={controlState.setMemberEditRole}
          onMemberEditStatusChange={controlState.setMemberEditStatus}
          onMemberEditWeightChange={controlState.setMemberEditWeight}
          onMemberEditCapabilitiesChange={controlState.setMemberEditCapabilities}
          onMemberEditBackendLabelChange={controlState.setMemberEditBackendLabel}
          onMemberEditModelChange={controlState.setMemberEditModel}
          onMemberEditBaseUrlChange={controlState.setMemberEditBaseUrl}
          onMemberEditSummaryModelChange={controlState.setMemberEditSummaryModel}
          onMemberEditToolsChange={controlState.setMemberEditTools}
          onMemberEditTimeoutMsChange={controlState.setMemberEditTimeoutMs}
          onMemberEditMetaJsonChange={controlState.setMemberEditMetaJson}
          onMemberEditAgentIdChange={controlState.setMemberEditAgentId}
          onMemberEditDeploymentIdChange={controlState.setMemberEditDeploymentId}
          onRefreshMembers={() => void controlState.refreshMembers(setupState.teamIdTrimmed)}
          onAddMember={() => void controlState.handleAddMember()}
          onRefreshMemberAgents={() => void setupState.refreshMemberAgents()}
          onAddConnectedAgents={() => void controlState.handleAddConnectedAgentsToTeam()}
          onToggleMemberStatus={(member) => void controlState.handleToggleMemberStatus(member as any)}
          onEditMember={(member) => controlState.handleEditMember(member as any)}
          onDeleteMember={(memberId) => void controlState.handleDeleteMember(memberId)}
          onSaveMemberEdit={() => void controlState.handleSaveMemberEdit()}
          onCancelMemberEdit={controlState.handleCancelMemberEdit}
          onSetAllMemberStatus={(status) => void controlState.handleSetAllMemberStatus(status)}
          onRemovePausedMembers={() => void controlState.handleRemovePausedMembers()}
        />
      ) : null}

      {activeTab === "advanced" ? (
        <>
          <SectionCard title="Quorum rules" description="Approvals and quorum settings.">
            <div className="flex items-center justify-between gap-2">
              <div className="text-xs font-semibold text-white/80">Quorum rules</div>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canQuery || !setupState.teamIdTrimmed || controlState.rulesBusy}
                onClick={() => void controlState.refreshRules(setupState.teamIdTrimmed)}
              >
                {controlState.rulesBusy ? "Loading…" : "Refresh"}
              </button>
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <FieldLabel>Action</FieldLabel>
              <input
                className="min-w-[160px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={controlState.ruleAction}
                onChange={(event) => controlState.setRuleAction(event.target.value)}
              />
              <FieldLabel>Min approvals</FieldLabel>
              <input
                className="w-20 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={controlState.ruleMinApprovals}
                onChange={(event) => controlState.setRuleMinApprovals(event.target.value)}
              />
              <FieldLabel>Mode</FieldLabel>
              <input
                className="min-w-[120px] flex-1 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/90"
                value={controlState.ruleMode}
                onChange={(event) => controlState.setRuleMode(event.target.value)}
              />
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canQuery || !setupState.teamIdTrimmed || controlState.rulesBusy}
                onClick={() => void controlState.handleAddRule()}
              >
                Add rule
              </button>
            </div>
            {controlState.rulesError ? (
              <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                {controlState.rulesError}
              </div>
            ) : null}
            {controlState.rules && controlState.rules.length > 0 ? (
              <div className="grid gap-2">
                {controlState.rules.map((rule, idx) => {
                  const ruleId = String(rule?.rule_id || "");
                  return (
                    <div
                      key={`rule-${ruleId}-${idx}`}
                      className="flex flex-wrap items-center justify-between gap-2 rounded-md border border-white/5 bg-black/30 px-2 py-1 text-[11px] text-white/70"
                    >
                      <div className="text-[11px] text-white/70">
                        <span className="text-white/90">{rule?.action || "team_run"}</span>
                        {rule?.min_approvals ? ` · min ${rule.min_approvals}` : ""}
                        {rule?.quorum_mode ? ` · ${rule.quorum_mode}` : ""}
                        {rule?.created_unix_ms ? ` · ${fmtTs(rule.created_unix_ms)}` : ""}
                      </div>
                      <button
                        className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                        type="button"
                        onClick={() => void controlState.handleDeleteRule(ruleId)}
                      >
                        Remove
                      </button>
                    </div>
                  );
                })}
              </div>
            ) : null}
          </SectionCard>

          <SectionCard title="Team event replay" description="Replay recent events for auditing." defaultOpen={false}>
            <div className="flex flex-wrap items-center gap-2 text-[11px] text-white/60">
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                type="button"
                disabled={!canQuery || !setupState.teamIdTrimmed || teamEventsState.teamReplayBusy}
                onClick={() => void teamEventsState.loadTeamReplay()}
              >
                {teamEventsState.teamReplayBusy ? "Replaying…" : "Replay"}
              </button>
              {teamEventsState.teamReplayNote ? <span className="text-emerald-200">{teamEventsState.teamReplayNote}</span> : null}
              {teamEventsState.teamReplayError ? <span className="text-rose-200">{teamEventsState.teamReplayError}</span> : null}
            </div>
          </SectionCard>

          <SectionCard title="Orchestrator runs" description="Low-level orchestrator controls." defaultOpen={false}>
            <BrokerOrchestratorRunPanel
              base={props.base}
              auth={props.auth}
              canQuery={canQuery}
              teamId={setupState.teamIdTrimmed}
              teamMeta={
                controlState.teamDetails?.meta && typeof controlState.teamDetails.meta === "object"
                  ? (controlState.teamDetails.meta as Record<string, any>)
                  : null
              }
              events={teamEventsState.mergedOrchestratorEvents}
            />
          </SectionCard>

          <SectionCard title="Guidance" description="Send guidance and review receipts." defaultOpen={false}>
            <BrokerTeamGuidancePanel
              base={props.base}
              auth={props.auth}
              canQuery={canQuery}
              teamId={setupState.teamIdTrimmed}
              events={teamEventsState.mergedGuidanceEvents}
            />
          </SectionCard>

          <SectionCard title="Spawn requests" description="Monitor orchestrator spawn requests." defaultOpen={false}>
            <BrokerOrchestratorSpawnPanel
              base={props.base}
              auth={props.auth}
              canQuery={canQuery}
              teamId={setupState.teamIdTrimmed}
              events={teamEventsState.mergedOrchestratorEvents}
            />
          </SectionCard>
        </>
      ) : null}
    </section>
  );
}
