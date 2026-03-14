import SettingsConnectionExecutionSection from "./SettingsConnectionExecutionSection";
import SettingsConnectionProfilesSection from "./SettingsConnectionProfilesSection";
import SettingsConnectionTransportSection from "./SettingsConnectionTransportSection";
import SettingsSessionLeaseSection from "./SettingsSessionLeaseSection";
import type { SettingsConnectionSectionProps } from "./settingsConnectionTypes";

export type { SessionSettings, SettingsConnectionSectionProps } from "./settingsConnectionTypes";

export default function SettingsConnectionSection(props: SettingsConnectionSectionProps) {
  return (
    <>
      <SettingsConnectionProfilesSection
        connection={props.connection}
        client={props.client}
        serverPrefsCanSync={props.serverPrefsCanSync}
        serverPrefsTarget={props.serverPrefsTarget}
        serverPrefsStatusLabel={props.serverPrefsStatusLabel}
        serverPrefsAutoNote={props.serverPrefsAutoNote}
      />

      <SettingsConnectionTransportSection
        connection={props.connection}
        brokerAuthReady={props.brokerAuthReady}
        brokerAgentsBusy={props.brokerAgentsBusy}
        brokerAgentsError={props.brokerAgentsError}
        brokerAgents={props.brokerAgents}
        brokerDeploymentsBusy={props.brokerDeploymentsBusy}
        brokerDeploymentsError={props.brokerDeploymentsError}
        brokerDeployments={props.brokerDeployments}
        brokerDeploymentsDefaultId={props.brokerDeploymentsDefaultId}
        listBrokerAgents={props.listBrokerAgents}
        listBrokerDeployments={props.listBrokerDeployments}
      />

      <div className="mt-4 grid grid-cols-2 gap-3">
        <SettingsSessionLeaseSection connection={props.connection} client={props.client} session={props.session} />
        <SettingsConnectionExecutionSection
          run={props.run}
          automationProfiles={props.automationProfiles}
          automationDefault={props.automationDefault}
          automationOverrideAllowed={props.automationOverrideAllowed}
        />
      </div>
    </>
  );
}
