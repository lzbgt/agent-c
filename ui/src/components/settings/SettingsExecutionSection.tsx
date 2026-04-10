import React from "react";
import type { Caps, DaemonConfigResp, OpenRouterModelsResp } from "../../api";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../../hooks/uiSettingsTypes";
import SettingsCapabilitiesSection from "./SettingsCapabilitiesSection";
import SettingsClientPreferencesSection from "./SettingsClientPreferencesSection";
import SettingsDaemonDefaultsSection from "./SettingsDaemonDefaultsSection";
import SettingsMemoryContextSection from "./SettingsMemoryContextSection";
import SettingsModelProviderSection from "./SettingsModelProviderSection";
import SettingsOpenRouterSection from "./SettingsOpenRouterSection";
import SettingsRunLimitsSection from "./SettingsRunLimitsSection";

type SettingsExecutionSectionProps = {
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  daemonConfig: {
    isFetching: boolean;
    refresh: () => void;
  };
  updateDaemonDefaults: {
    pending: boolean;
    error: string | null;
    success: boolean;
    saveDefaults: () => void;
    saveApiKey: () => void;
    clearApiKey: () => void;
  };
  daemonDefaults?: DaemonConfigResp["daemon"];
  caps: {
    data?: Caps;
    source: "live" | "cache" | "none";
    isFetching: boolean;
    error: string | null;
    refresh: () => void;
  };
  capsAge: string;
  capsJson: string;
  connectorStaleMinutes: string;
  setConnectorStaleMinutes: (next: string) => void;
  jobsEnabled: boolean;
  baseUrlLabel: string;
  fetchOpenRouterModelsPending: boolean;
  fetchOpenRouterModelsError: string | null;
  onFetchOpenRouterModels: () => void;
  openrouterModels: OpenRouterModelsResp | null;
};

export default function SettingsExecutionSection(props: SettingsExecutionSectionProps) {
  return (
    <>
      <SettingsClientPreferencesSection
        client={props.client}
        connectorStaleMinutes={props.connectorStaleMinutes}
        setConnectorStaleMinutes={props.setConnectorStaleMinutes}
        daemonConfig={props.daemonConfig}
      />
      <SettingsModelProviderSection
        connection={props.connection}
        run={props.run}
        client={props.client}
        daemonDefaults={props.daemonDefaults}
        jobsEnabled={props.jobsEnabled}
        baseUrlLabel={props.baseUrlLabel}
      />
      <SettingsRunLimitsSection run={props.run} />
      <SettingsMemoryContextSection run={props.run} />
      <SettingsOpenRouterSection
        run={props.run}
        fetchOpenRouterModelsPending={props.fetchOpenRouterModelsPending}
        fetchOpenRouterModelsError={props.fetchOpenRouterModelsError}
        onFetchOpenRouterModels={props.onFetchOpenRouterModels}
        openrouterModels={props.openrouterModels}
      />
      <SettingsDaemonDefaultsSection
        daemonConfig={props.daemonConfig}
        updateDaemonDefaults={props.updateDaemonDefaults}
        daemonDefaults={props.daemonDefaults}
      />
      <SettingsCapabilitiesSection caps={props.caps} capsAge={props.capsAge} capsJson={props.capsJson} />
    </>
  );
}
