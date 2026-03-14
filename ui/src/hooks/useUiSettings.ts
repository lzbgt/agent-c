import React from "react";
import useLocalStorageState from "./useLocalStorageState";
import useUiClientSettings from "./useUiClientSettings";
import useUiConnectionSettings from "./useUiConnectionSettings";
import useUiRunSettings from "./useUiRunSettings";
import { getUiDefaults } from "../runtime_config";
import type { ClientSettings, ConnectionSettings, RunSettings, UiSettings } from "./uiSettingsTypes";

export type { ClientSettings, ConnectionSettings, RunSettings, UiSettings } from "./uiSettingsTypes";

export default function useUiSettings(): UiSettings {
  const defaults = React.useMemo(() => getUiDefaults(), []);
  const [showSettings, setShowSettings] = useLocalStorageState("agentui.showSettings", false);

  const clientPlane = useUiClientSettings({ defaults });
  const connectionPlane = useUiConnectionSettings({
    defaults,
    clientId: clientPlane.client.clientId,
  });
  const run = useUiRunSettings({
    defaults,
    activeProfile: connectionPlane.activeProfile,
    updateActiveProfile: connectionPlane.updateActiveProfile,
  });

  return {
    defaults,
    showSettings,
    setShowSettings,
    connection: connectionPlane.connection,
    run,
    client: clientPlane.client,
    brokerPanelOpen: clientPlane.brokerPanelOpen,
    setBrokerPanelOpen: clientPlane.setBrokerPanelOpen,
  };
}
