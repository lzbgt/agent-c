// Runtime defaults for docker-compose WebUI (local dev).
// This file is mounted into the webui container at /usr/share/nginx/html/agentui-config.js.
window.__AGENT_UI_CONFIG__ = {
  connectionMode: "broker",
  brokerBaseUrl: "https://127.0.0.1:8443",
  brokerAgentId: "agent1",
  brokerAuthToken: "",
  daemonAuthToken: "dev-agentd-token",
};
