// Runtime defaults for "automouse" WebUI (full automation presets).
// Mount into /usr/share/nginx/html/agentui-config.js for docker-compose.
window.__AGENT_UI_CONFIG__ = {
  connectionMode: "broker",
  brokerBaseUrl: "https://127.0.0.1:8443",
  brokerAgentId: "agent1",
  brokerAuthToken: "",
  daemonAuthToken: "dev-agentd-token",
  tools: "host",
  yolo: true,
  hostPolicy: "full",
  automationProfile: "full",
  serverPrefsMode: "auto",
  brokerPanelOpen: true,
};
