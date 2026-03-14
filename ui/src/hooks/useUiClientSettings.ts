import React from "react";
import useAutoplayUnlock from "./useAutoplayUnlock";
import useLocalStorageState from "./useLocalStorageState";
import type { ClientSettings } from "./uiSettingsTypes";
import type { AgentUIDefaults } from "../runtime_config";

type UseUiClientSettingsParams = {
  defaults: AgentUIDefaults;
};

export type UseUiClientSettingsResult = {
  client: ClientSettings;
  brokerPanelOpen: boolean;
  setBrokerPanelOpen: React.Dispatch<React.SetStateAction<boolean>>;
};

export default function useUiClientSettings({ defaults }: UseUiClientSettingsParams): UseUiClientSettingsResult {
  const initialClientId = React.useMemo(() => {
    const preset = String(defaults.clientId || "").trim();
    if (preset) return preset;
    try {
      const g: { crypto?: { randomUUID?: () => string } } = typeof globalThis !== "undefined" ? globalThis : {};
      if (g.crypto && typeof g.crypto.randomUUID === "function") {
        return `webui-${g.crypto.randomUUID()}`;
      }
    } catch {
      // ignore
    }
    return `webui-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }, [defaults.clientId]);

  const [clientId, setClientId] = useLocalStorageState("agentui.clientId", initialClientId);
  const [allowAutoplay, setAllowAutoplay] = useLocalStorageState("agentui.allowAutoplay", true);
  useAutoplayUnlock(allowAutoplay);
  const [allowClientRpcs, setAllowClientRpcs] = useLocalStorageState("agentui.allowClientRpcs", defaults.allowClientRpcs);
  const [allowClientEffects, setAllowClientEffects] = useLocalStorageState(
    "agentui.allowClientEffects",
    defaults.allowClientEffects,
  );
  const [allowUnsafePageEval, setAllowUnsafePageEval] = useLocalStorageState(
    "agentui.allowUnsafePageEval",
    defaults.allowUnsafePageEval,
  );
  const [showDebugInConversation, setShowDebugInConversation] = useLocalStorageState(
    "agentui.showDebugInConversation",
    true,
  );
  const [brokerPanelOpen, setBrokerPanelOpen] = useLocalStorageState(
    "agentui.brokerPanelOpen",
    defaults.brokerPanelOpen,
  );

  return {
    client: {
      clientId: String(clientId || "webui"),
      setClientId,
      allowAutoplay,
      setAllowAutoplay,
      allowClientRpcs,
      setAllowClientRpcs,
      allowClientEffects,
      setAllowClientEffects,
      allowUnsafePageEval,
      setAllowUnsafePageEval,
      showDebugInConversation,
      setShowDebugInConversation,
    },
    brokerPanelOpen,
    setBrokerPanelOpen,
  };
}
