import React from "react";

import SettingsConnectionSection from "./settings/SettingsConnectionSection";
import SettingsDiagnosticsSection from "./settings/SettingsDiagnosticsSection";
import SettingsExecutionSection from "./settings/SettingsExecutionSection";
import SettingsModeratorSection from "./settings/SettingsModeratorSection";
import SettingsSessionsSection from "./settings/SettingsSessionsSection";
import type { SettingsDrawerProps } from "./settings/settingsDrawerTypes";
import useSettingsDrawerState from "./settings/useSettingsDrawerState";

export type { SettingsDrawerProps } from "./settings/settingsDrawerTypes";

export default function SettingsDrawer(props: SettingsDrawerProps) {
  const { connection, run, client } = props;
  const state = useSettingsDrawerState(props);

  if (!props.open) return null;

  return (
    <div className="fixed inset-0 z-40" data-testid="settings-drawer">
      <div className="absolute inset-0 bg-black/60" onClick={props.onClose} role="button" tabIndex={0} />
      <div className="absolute right-0 top-0 h-full w-[520px] max-w-[94vw] overflow-auto border-l border-white/10 bg-slate-950 p-4">
        <div className="flex items-center justify-between gap-3">
          <div className="text-sm font-semibold">Settings</div>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={props.onClose}
            type="button"
            data-testid="settings-close"
          >
            Close
          </button>
        </div>

        <SettingsConnectionSection
          connection={connection}
          run={run}
          client={client}
          session={props.session}
          serverPrefsCanSync={state.serverPrefsCanSync}
          serverPrefsTarget={state.serverPrefsTarget}
          serverPrefsStatusLabel={state.serverPrefsStatusLabel}
          serverPrefsAutoNote={state.serverPrefsAutoNote}
          brokerAuthReady={state.brokerAuthReady}
          brokerAgentsBusy={state.brokerAgentsBusy}
          brokerAgentsError={state.brokerAgentsError}
          brokerAgents={state.brokerAgents}
          brokerDeploymentsBusy={state.brokerDeploymentsBusy}
          brokerDeploymentsError={state.brokerDeploymentsError}
          brokerDeployments={state.brokerDeployments}
          brokerDeploymentsDefaultId={state.brokerDeploymentsDefaultId}
          listBrokerAgents={state.listBrokerAgents}
          listBrokerDeployments={state.listBrokerDeployments}
          automationProfiles={state.automationProfiles}
          automationDefault={state.automationDefault}
          automationOverrideAllowed={state.automationOverrideAllowed}
        />

        <SettingsModeratorSection
          connection={connection}
          sessionId={props.session.id}
          moderatorDirective={state.moderatorDirective}
          setModeratorDirective={state.setModeratorDirective}
          moderatorDirectiveScope={state.moderatorDirectiveScope}
          setModeratorDirectiveScope={state.setModeratorDirectiveScope}
          moderatorDirectiveAssignees={state.moderatorDirectiveAssignees}
          setModeratorDirectiveAssignees={state.setModeratorDirectiveAssignees}
          moderatorDirectivePick={state.moderatorDirectivePick}
          setModeratorDirectivePick={state.setModeratorDirectivePick}
          moderatorTaskTitle={state.moderatorTaskTitle}
          setModeratorTaskTitle={state.setModeratorTaskTitle}
          moderatorTaskDetail={state.moderatorTaskDetail}
          setModeratorTaskDetail={state.setModeratorTaskDetail}
          moderatorTaskAssignees={state.moderatorTaskAssignees}
          setModeratorTaskAssignees={state.setModeratorTaskAssignees}
          moderatorTaskPick={state.moderatorTaskPick}
          setModeratorTaskPick={state.setModeratorTaskPick}
          moderatorAppendToSession={state.moderatorAppendToSession}
          setModeratorAppendToSession={state.setModeratorAppendToSession}
          moderatorBusy={state.moderatorBusy}
          moderatorError={state.moderatorError}
          moderatorSuccess={state.moderatorSuccess}
          moderatorEventsAuto={state.moderatorEventsAuto}
          setModeratorEventsAuto={state.setModeratorEventsAuto}
          moderatorEventsMaxBytes={state.moderatorEventsMaxBytes}
          setModeratorEventsMaxBytes={state.setModeratorEventsMaxBytes}
          moderatorEventsIncludeDirectives={state.moderatorEventsIncludeDirectives}
          setModeratorEventsIncludeDirectives={state.setModeratorEventsIncludeDirectives}
          moderatorEventsIncludeTasks={state.moderatorEventsIncludeTasks}
          setModeratorEventsIncludeTasks={state.setModeratorEventsIncludeTasks}
          moderatorEventsFilter={state.moderatorEventsFilter}
          setModeratorEventsFilter={state.setModeratorEventsFilter}
          moderatorEventsExpanded={state.moderatorEventsExpanded}
          setModeratorEventsExpanded={state.setModeratorEventsExpanded}
          brokerAgentOptions={state.brokerAgentOptions}
          brokerAgentsBusy={state.brokerAgentsBusy}
          listBrokerAgents={state.listBrokerAgents}
          moderatorRolePresets={state.moderatorRolePresets}
          addDirectiveAssignee={state.addDirectiveAssignee}
          addTaskAssignee={state.addTaskAssignee}
          applyRuntimeMemberTaskTemplate={state.applyRuntimeMemberTaskTemplate}
          publishModeratorDirective={state.publishModeratorDirective}
          publishModeratorTask={state.publishModeratorTask}
          moderatorEventsEnabled={state.moderatorEventsEnabled}
          moderatorEventsRefetch={() => void state.moderatorEvents.refetch()}
          moderatorEventsFetching={state.moderatorEvents.isFetching}
          moderatorEventsError={state.moderatorEventsError}
          moderatorEventsList={state.moderatorEventsList}
          moderatorEventsFiltered={state.moderatorEventsFiltered}
          moderatorPinnedEvents={state.moderatorPinnedEvents}
          moderatorPinnedEntries={state.moderatorPinnedEntries}
          updateModeratorPinnedEvents={state.updateModeratorPinnedEvents}
          pinImportRef={state.pinImportRef}
          showPinNotice={state.showPinNotice}
          handleCopy={state.handleCopy}
          pinnedCompareOptions={state.pinnedCompareOptions}
          pinnedCompareA={state.pinnedCompareA}
          setPinnedCompareA={state.setPinnedCompareA}
          pinnedCompareB={state.pinnedCompareB}
          setPinnedCompareB={state.setPinnedCompareB}
          pinnedCompareDiffOnly={state.pinnedCompareDiffOnly}
          setPinnedCompareDiffOnly={state.setPinnedCompareDiffOnly}
          copyNotice={state.copyNotice}
          pinNotice={state.pinNotice}
          pinError={state.pinError}
          moderatorDirectivesEnabled={state.moderatorDirectivesEnabled}
          moderatorTasksEnabled={state.moderatorTasksEnabled}
        />

        <SettingsExecutionSection
          connection={connection}
          run={run}
          client={client}
          daemonConfig={props.daemonConfig}
          updateDaemonDefaults={props.updateDaemonDefaults}
          daemonDefaults={state.daemonDefaults}
          caps={props.caps}
          capsAge={state.capsAge}
          capsJson={state.capsJson}
          connectorStaleMinutes={state.connectorStaleMinutes}
          setConnectorStaleMinutes={state.setConnectorStaleMinutes}
          jobsEnabled={state.jobsEnabled}
          baseUrlLabel={state.baseUrlLabel}
          fetchOpenRouterModelsPending={state.fetchOpenRouterModels.isPending}
          fetchOpenRouterModelsError={state.fetchOpenRouterModels.isError ? String(state.fetchOpenRouterModels.error) : null}
          onFetchOpenRouterModels={() => state.fetchOpenRouterModels.mutate()}
          openrouterModels={state.openrouterModels}
        />

        <SettingsDiagnosticsSection
          diagnostics={state.diag}
          diagnosticsProviders={state.diagnosticsProviders.data}
          diagnosticsFetching={state.diagnostics.isFetching}
          diagnosticsProvidersFetching={state.diagnosticsProviders.isFetching}
          diagnosticsError={state.diagnostics.isError ? String(state.diagnostics.error) : null}
          diagnosticsProvidersError={state.diagnosticsProviders.isError ? String(state.diagnosticsProviders.error) : null}
          onRefresh={() => {
            void state.diagnostics.refetch();
            void state.diagnosticsProviders.refetch();
          }}
          sandboxMountHostPath={String(state.sandboxMountHostPath || "")}
          setSandboxMountHostPath={state.setSandboxMountHostPath}
          sandboxMountContainerPath={String(state.sandboxMountContainerPath || "")}
          setSandboxMountContainerPath={state.setSandboxMountContainerPath}
          sandboxMountContainerPrefix={String(state.sandboxMountContainerPrefix || "")}
          setSandboxMountContainerPrefix={state.setSandboxMountContainerPrefix}
          sandboxMountIsMain={state.sandboxMountIsMain}
          setSandboxMountIsMain={state.setSandboxMountIsMain}
          sandboxMountPending={state.sandboxMountValidate.isPending}
          sandboxMountError={state.sandboxMountError}
          sandboxMountResult={state.sandboxMountResult}
          onValidateSandboxMount={() => state.sandboxMountValidate.mutate()}
          canValidateSandboxMount={!!String(connection.effectiveBase || "").trim()}
          providerTests={state.providerTests}
          onRunProviderTest={(provider) => void state.runProviderTest(provider)}
        />

        <SettingsSessionsSection
          session={props.session}
          clearAllArmed={state.clearAllArmed}
          setClearAllArmed={state.setClearAllArmed}
          clearAllArmTimeoutRef={state.clearAllArmTimeoutRef}
        />
      </div>
    </div>
  );
}
