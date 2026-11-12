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
    run: defaults.localModelProfile ? {
      ...run, tools: "host", hostPolicy: "full", yolo: true, automationProfile: "full",
      model: defaults.model, summaryModel: defaults.model, baseUrl: defaults.baseUrl,
      apiKey: "", proxyUrl: "", timeoutMs: defaults.timeoutMs,
      streamAssistant: true, useAsync: true, trace: false, verbose: false,
      maxChars: "480000", maxSteps: "32", maxToolCallsTotal: "64", maxRepeatedToolCalls: "3",
    } : run,
    client: defaults.localModelProfile ? {
      ...clientPlane.client, allowClientRpcs: false, allowClientEffects: false, allowUnsafePageEval: false,
    } : clientPlane.client,
    brokerPanelOpen: defaults.localModelProfile ? false : clientPlane.brokerPanelOpen,
    setBrokerPanelOpen: clientPlane.setBrokerPanelOpen,
  };
}
