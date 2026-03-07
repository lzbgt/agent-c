import type { Page } from "@playwright/test";

type SeedBrokerStateOptions = {
  agentId?: string;
  brokerBase?: string;
  brokerToken?: string;
  brokerCookieAuth?: boolean;
  daemonToken?: string;
  deploymentId?: string;
  brokerPanelOpen?: boolean;
  traceLookupOpen?: boolean;
  showSettings?: boolean;
  simpleMode?: boolean;
  allowClientRpcs?: boolean;
  allowClientEffects?: boolean;
};

export async function seedBrokerState(page: Page, options: SeedBrokerStateOptions = {}): Promise<void> {
  await page.addInitScript((config: Required<SeedBrokerStateOptions>) => {
    try {
      const profileId = "profile-broker-test";
      const profile = {
        id: profileId,
        name: config.deploymentId ? `broker:${config.agentId}@${config.deploymentId}` : `broker:${config.agentId}`,
        mode: "broker",
        base: "http://127.0.0.1:8123",
        brokerBase: config.brokerBase,
        brokerAgentId: config.agentId,
        brokerDeploymentId: config.deploymentId,
        brokerCookieAuth: config.brokerCookieAuth,
        brokerAuthToken: "",
        daemonAuthToken: "",
        runOverridesEnabled: false,
      };
      const secrets: Record<string, { brokerAuthToken: string; daemonAuthToken?: string }> = {
        [profileId]: {
          brokerAuthToken: config.brokerToken,
        },
      };
      if (config.daemonToken) {
        secrets[profileId].daemonAuthToken = config.daemonToken;
      }

      window.localStorage.setItem("agentui.simpleMode", JSON.stringify(config.simpleMode));
      window.localStorage.setItem("agentui.connectionProfiles", JSON.stringify([profile]));
      window.localStorage.setItem("agentui.connectionProfileActive", JSON.stringify(profileId));
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("broker"));
      window.localStorage.setItem("agentui.brokerBase", config.brokerBase);
      window.localStorage.setItem("agentui.brokerAgentId", config.agentId);
      window.localStorage.setItem("agentui.brokerDeploymentId", config.deploymentId);
      window.localStorage.setItem("agentui.brokerPanelOpen", JSON.stringify(config.brokerPanelOpen));
      window.localStorage.setItem("agentui.traceLookupOpen", JSON.stringify(config.traceLookupOpen));
      window.localStorage.setItem("agentui.showSettings", JSON.stringify(config.showSettings));
      window.localStorage.setItem("agentui.allowClientRpcs", JSON.stringify(config.allowClientRpcs));
      window.localStorage.setItem("agentui.allowClientEffects", JSON.stringify(config.allowClientEffects));

      window.sessionStorage.setItem("agentui.connectionProfileSecrets", JSON.stringify(secrets));
      window.sessionStorage.setItem("agentui.brokerAuthToken", JSON.stringify(config.brokerToken));
      if (config.daemonToken) {
        window.sessionStorage.setItem("agentui.daemonAuthToken", JSON.stringify(config.daemonToken));
      }
    } catch {
      // ignore storage failures in test bootstrap
    }
  }, {
    agentId: options.agentId ?? "agent1",
    brokerBase: options.brokerBase ?? "https://broker.example.invalid",
    brokerToken: options.brokerToken ?? "test-token",
    brokerCookieAuth: options.brokerCookieAuth ?? false,
    daemonToken: options.daemonToken ?? "",
    deploymentId: options.deploymentId ?? "",
    brokerPanelOpen: options.brokerPanelOpen ?? true,
    traceLookupOpen: options.traceLookupOpen ?? false,
    showSettings: options.showSettings ?? false,
    simpleMode: options.simpleMode ?? false,
    allowClientRpcs: options.allowClientRpcs ?? true,
    allowClientEffects: options.allowClientEffects ?? true,
  });
}
