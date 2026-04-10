import FieldLabel from "../FieldLabel";
import { ToggleRow } from "./SettingsControls";
import type { SettingsConnectionSectionProps } from "./settingsConnectionTypes";

type SettingsConnectionTransportSectionProps = Pick<
  SettingsConnectionSectionProps,
  | "connection"
  | "brokerAuthReady"
  | "brokerAgentsBusy"
  | "brokerAgentsError"
  | "brokerAgents"
  | "brokerDeploymentsBusy"
  | "brokerDeploymentsError"
  | "brokerDeployments"
  | "brokerDeploymentsDefaultId"
  | "listBrokerAgents"
  | "listBrokerDeployments"
>;

export default function SettingsConnectionTransportSection(props: SettingsConnectionTransportSectionProps) {
  const {
    connection,
    brokerAuthReady,
    brokerAgentsBusy,
    brokerAgentsError,
    brokerAgents,
    brokerDeploymentsBusy,
    brokerDeploymentsError,
    brokerDeployments,
    brokerDeploymentsDefaultId,
    listBrokerAgents,
    listBrokerDeployments,
  } = props;

  if (connection.mode === "direct") {
    return (
      <>
        <div className="mt-4">
          <FieldLabel>Daemon base URL</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            data-testid="daemon-base"
            value={connection.base}
            onChange={(e) => connection.setBase(e.target.value)}
          />
        </div>

        <div className="mt-4">
          <FieldLabel>Daemon auth token (optional)</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            data-testid="daemon-auth-token"
            placeholder='Bearer token (e.g. "dev-agentd-token" in docker-compose)'
            value={connection.daemonAuthToken}
            onChange={(e) => connection.setDaemonAuthToken(e.target.value)}
          />
        </div>
      </>
    );
  }

  return (
    <>
      <div className="mt-4">
        <FieldLabel>Broker base URL</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={connection.brokerBase}
          onChange={(e) => connection.setBrokerBase(e.target.value)}
          placeholder='e.g. "https://broker.example.com" (or "https://127.0.0.1:8443" in docker-compose)'
        />
      </div>

      <div className="mt-4">
        <ToggleRow
          label="Use broker auth cookie (HttpOnly)"
          checked={connection.brokerCookieAuth}
          onChange={connection.setBrokerCookieAuth}
        />
        <div className="mt-2 text-[11px] text-white/60">
          Sends browser credentials to the broker instead of relying only on a JS-visible OIDC token. Enable this when the broker is configured with{" "}
          <code className="font-mono">--auth-cookie</code> and <code className="font-mono">--cors-allow-credentials</code>.
        </div>
        {connection.brokerCookieAuth ? (
          <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px]">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => void connection.clearBrokerAuthCookie()}
              disabled={!String(connection.brokerBase || "").trim()}
            >
              Clear auth cookie
            </button>
            {connection.brokerCookieSessionStatus === "exchanging" ? (
              <span className="text-sky-200" data-testid="broker-cookie-session-status">
                Exchanging broker bearer token for an HttpOnly cookie...
              </span>
            ) : null}
            {connection.brokerCookieSessionStatus === "ready" ? (
              <span className="text-emerald-200" data-testid="broker-cookie-session-status">
                Broker auth cookie established. The in-browser bearer token was cleared for this profile.
              </span>
            ) : null}
            {connection.brokerCookieSessionStatus === "error" && connection.brokerCookieSessionError ? (
              <span className="text-rose-200" data-testid="broker-cookie-session-status">
                Cookie exchange failed: {connection.brokerCookieSessionError}
              </span>
            ) : null}
          </div>
        ) : null}
      </div>

      <div className="mt-4">
        <FieldLabel>Broker auth token (OIDC)</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder="Authorization bearer token for broker (OIDC JWT)"
          value={connection.brokerAuthToken}
          onChange={(e) => connection.setBrokerAuthToken(e.target.value)}
        />
        <div className="mt-2 text-[11px] text-white/60">
          Uses <code className="font-mono">Authorization: Bearer &lt;jwt&gt;</code> to call broker endpoints.
          {connection.brokerCookieAuth ? " Optional when a broker auth cookie is present." : ""}
        </div>
      </div>

      <div className="mt-4">
        <FieldLabel>Agent id</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={connection.brokerAgentId}
          onChange={(e) => connection.setBrokerAgentId(e.target.value)}
          placeholder='e.g. "agent1"'
        />
        <div className="mt-2 flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={brokerAgentsBusy || !brokerAuthReady}
            onClick={() => void listBrokerAgents()}
            title="Fetches /v1/agents from the broker."
          >
            {brokerAgentsBusy ? "Listing…" : "List agents"}
          </button>
          <div className="text-[11px] text-white/60">
            Proxy base: <code className="font-mono text-white/70">{String(connection.effectiveBase || "").trim()}</code>
          </div>
        </div>
        {brokerAgentsError ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            List agents failed: {brokerAgentsError}
          </div>
        ) : null}
        {brokerAgents && brokerAgents.length > 0 ? (
          <div className="mt-2 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/20">
            {brokerAgents.map((agent) => {
              const id = typeof agent.agent_id === "string" ? agent.agent_id : "";
              if (!id) return null;
              const connected = agent.connected === true;
              const lastSeen = typeof agent.last_seen_unix_ms === "number" ? agent.last_seen_unix_ms : 0;
              const selected = String(connection.brokerAgentId || "").trim() === id;
              return (
                <button
                  key={id}
                  type="button"
                  className={[
                    "flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5",
                    selected ? "bg-white/10" : "",
                  ].join(" ")}
                  onClick={() => {
                    connection.setBrokerAgentId(id);
                    if (id !== String(connection.brokerAgentId || "").trim()) {
                      connection.setBrokerDeploymentId("");
                    }
                  }}
                  title={agent.remote_addr ? `remote=${String(agent.remote_addr)}` : ""}
                >
                  <span className="font-mono text-white/80">{id}</span>
                  <span className="text-white/60">
                    {connected ? <span className="text-emerald-300">connected</span> : <span className="text-white/40">disconnected</span>}
                    {lastSeen ? ` · last_seen=${new Date(lastSeen).toLocaleString()}` : ""}
                  </span>
                </button>
              );
            })}
          </div>
        ) : null}
      </div>

      <div className="mt-4">
        <FieldLabel>Deployment id (optional)</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={connection.brokerDeploymentId}
          onChange={(e) => connection.setBrokerDeploymentId(e.target.value)}
          placeholder='e.g. "laptop-1" (leave blank to auto-pick latest)'
        />
        <div className="mt-2 flex items-center gap-2">
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
            type="button"
            disabled={brokerDeploymentsBusy || !brokerAuthReady || String(connection.brokerAgentId || "").trim().length === 0}
            onClick={() => void listBrokerDeployments()}
            title="Fetches /v1/agents/{agent_id}/deployments from the broker."
          >
            {brokerDeploymentsBusy ? "Listing…" : "List deployments"}
          </button>
          <button
            className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
            type="button"
            onClick={() => connection.setBrokerDeploymentId("")}
            title="Clear deployment id to use broker default"
          >
            Use default
          </button>
          <div className="text-[11px] text-white/60">
            Uses <code className="font-mono">X-Agentd-Deployment</code>; empty means “latest connected”.
          </div>
        </div>
        {brokerDeploymentsDefaultId ? (
          <div className="mt-2 text-[11px] text-white/60">
            Broker default: <span className="font-mono text-white/80">{brokerDeploymentsDefaultId}</span>
          </div>
        ) : null}
        {brokerDeploymentsError ? (
          <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
            List deployments failed: {brokerDeploymentsError}
          </div>
        ) : null}
        {brokerDeployments && brokerDeployments.length > 0 ? (
          <div className="mt-2 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/20">
            {brokerDeployments.map((deployment) => {
              const id = typeof deployment.deployment_id === "string" ? deployment.deployment_id : "";
              if (!id) return null;
              const connected = deployment.connected === true;
              const lastSeen = typeof deployment.last_seen_unix_ms === "number" ? deployment.last_seen_unix_ms : 0;
              const selected = String(connection.brokerDeploymentId || "").trim() === id;
              const isDefault = brokerDeploymentsDefaultId && brokerDeploymentsDefaultId === id;
              return (
                <button
                  key={id}
                  type="button"
                  className={[
                    "flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5",
                    selected ? "bg-white/10" : "",
                  ].join(" ")}
                  onClick={() => connection.setBrokerDeploymentId(id)}
                  title={deployment.remote_addr ? `remote=${String(deployment.remote_addr)}` : ""}
                >
                  <span className="font-mono text-white/80">{id}</span>
                  <span className="text-white/60">
                    {isDefault ? <span className="text-sky-300">default</span> : null}
                    {isDefault ? " · " : ""}
                    {connected ? <span className="text-emerald-300">connected</span> : <span className="text-white/40">disconnected</span>}
                    {lastSeen ? ` · last_seen=${new Date(lastSeen).toLocaleString()}` : ""}
                  </span>
                </button>
              );
            })}
          </div>
        ) : null}
      </div>

      <div className="mt-4">
        <FieldLabel>Agentd auth token (pass-through)</FieldLabel>
        <input
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          placeholder='Bearer token forwarded to agentd as "X-Agentd-Authorization" (e.g. "dev-agentd-token")'
          value={connection.daemonAuthToken}
          onChange={(e) => connection.setDaemonAuthToken(e.target.value)}
        />
        <div className="mt-2 text-[11px] text-white/60">
          Uses <code className="font-mono">X-Agentd-Authorization: Bearer &lt;token&gt;</code> on proxied requests.
        </div>
      </div>
    </>
  );
}
