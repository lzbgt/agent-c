import React from "react";

import SettingsBasics from "./settings/SettingsBasics";
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
  const [advancedOpen, setAdvancedOpen] = React.useState(false);
  React.useEffect(() => { if (!props.open) setAdvancedOpen(false); }, [props.open]);
  const state = useSettingsDrawerState({ ...props, open: props.open && advancedOpen });
  const dialogRef = React.useRef<HTMLDialogElement>(null);
  React.useEffect(() => {
    const dialog = dialogRef.current;
    if (!props.open || !dialog) return;
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    dialog.showModal();
    dialog.querySelector<HTMLButtonElement>('[data-testid="settings-close"]')?.focus({ preventScroll: true });
    return () => {
      dialog.close();
      if (opener?.isConnected) opener.focus({ preventScroll: true });
    };
  }, [props.open]);

  function containTab(event: React.KeyboardEvent<HTMLDialogElement>) {
    if (event.key !== "Tab") return;
    const controls = Array.from(event.currentTarget.querySelectorAll<HTMLElement>("button, input, select, textarea, a[href], [tabindex]"))
      .filter((el) => el.tabIndex >= 0 && !el.matches(":disabled") && el.getClientRects().length > 0);
    const first = controls[0];
    const last = controls[controls.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault(); last?.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault(); first?.focus();
    }
  }

  if (!props.open) return null;

  return (
    <dialog ref={dialogRef} className="workspace-settings" data-testid="settings-drawer" aria-labelledby="workspace-settings-title" onKeyDown={containTab} onCancel={(event) => { event.preventDefault(); props.onClose(); }}>
      <div className="absolute inset-0 bg-black/60" onClick={props.onClose} aria-hidden="true" />
      <div className="workspace-settings-panel">
        <div className="workspace-settings-heading">
          <h2 id="workspace-settings-title" className="text-sm font-semibold">Settings</h2>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
            onClick={props.onClose}
            type="button"
            data-testid="settings-close"
          >
            Close
          </button>
        </div>

        <SettingsBasics connection={connection} run={run} session={props.session} />
        <details className="settings-disclosure">
          <summary>Manage conversations</summary>
        <SettingsSessionsSection
          session={props.session}
          clearAllArmed={state.clearAllArmed}
          setClearAllArmed={state.setClearAllArmed}
          clearAllArmTimeoutRef={state.clearAllArmTimeoutRef}
        />
        </details>
        <details className="settings-disclosure" data-testid="settings-advanced" open={advancedOpen} onToggle={(event) => setAdvancedOpen(event.currentTarget.open)}>
          <summary>Advanced settings</summary>
          {advancedOpen ? <>
        <details className="settings-disclosure"><summary>Connections & access</summary>
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

        </details>
        <details className="settings-disclosure"><summary>Team coordination</summary>
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

        </details>
        <details className="settings-disclosure"><summary>Execution & providers</summary>
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

        </details>
        <details className="settings-disclosure"><summary>Diagnostics</summary>
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


        </details>
          </> : null}
        </details>
      </div>
    </dialog>
  );
}
