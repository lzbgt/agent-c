import React from "react";
import type { ClientSettings, ConnectionSettings, RunSettings } from "../../hooks/useUiSettings";
import FieldLabel from "../FieldLabel";
import SettingsBrokerSessionOperatorsSection from "./SettingsBrokerSessionOperatorsSection";
import { ToggleRow } from "./SettingsControls";

type SessionSettings = {
  id: string;
  setId: (next: string) => void;
  leaseSeconds: string;
  setLeaseSeconds: (next: string) => void;
  info?: {
    session_id?: string;
    thread_id?: string | null;
    working?: boolean;
    attachment?: {
      client_id?: string | null;
      lease_seconds?: number | null;
      lease_expires_at_ms?: number | null;
      lease_active?: boolean;
    };
  };
  leaseConflict: {
    requestedClientId: string | null;
    currentAttachment?: {
      client_id?: string | null;
      lease_seconds?: number | null;
      lease_expires_at_ms?: number | null;
      lease_active?: boolean;
    };
    code: string;
    message: string;
    retryable: boolean;
  } | null;
  clearLeaseConflict: () => void;
  attach: () => void;
  attachPending: boolean;
  attachError: string | null;
  renewAttachment: () => void;
  renewPending: boolean;
  renewError: string | null;
  releaseAttachment: () => void;
  releasePending: boolean;
  releaseError: string | null;
  streamStatus: "disabled" | "idle" | "connecting" | "live" | "reconnecting" | "error";
  streamLastEventId: string;
  streamLastEventAtMs: number | null;
  streamUpdatedMs: number | null;
  streamBufferedCount: number;
  streamError: string | null;
};

type SettingsConnectionSectionProps = {
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  session: SessionSettings;
  serverPrefsCanSync: boolean;
  serverPrefsTarget: string;
  serverPrefsStatusLabel: string;
  serverPrefsAutoNote: string | null;
  brokerAuthReady: boolean;
  brokerAgentsBusy: boolean;
  brokerAgentsError: string | null;
  brokerAgents: any[] | null;
  brokerDeploymentsBusy: boolean;
  brokerDeploymentsError: string | null;
  brokerDeployments: any[] | null;
  brokerDeploymentsDefaultId: string | null;
  listBrokerAgents: () => Promise<void>;
  listBrokerDeployments: () => Promise<void>;
  automationProfiles: string[];
  automationDefault: string;
  automationOverrideAllowed: boolean;
};

export default function SettingsConnectionSection(props: SettingsConnectionSectionProps) {
  const {
    connection,
    run,
    client,
    session,
    serverPrefsCanSync,
    serverPrefsTarget,
    serverPrefsStatusLabel,
    serverPrefsAutoNote,
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
    automationProfiles,
    automationDefault,
    automationOverrideAllowed,
  } = props;
  const attachment = session.info?.attachment;
  const holderClientId = typeof attachment?.client_id === "string" ? attachment.client_id.trim() : "";
  const currentClientId = String(client.clientId || "").trim();
  const leaseActive = attachment?.lease_active === true;
  const leaseRole = leaseActive ? (holderClientId && currentClientId && holderClientId === currentClientId ? "owner" : "observer") : "unleased";
  const leaseExpiresLabel =
    typeof attachment?.lease_expires_at_ms === "number" && Number.isFinite(attachment.lease_expires_at_ms)
      ? new Date(attachment.lease_expires_at_ms).toLocaleString()
      : "";
  const streamLastEventLabel =
    typeof session.streamLastEventAtMs === "number" && Number.isFinite(session.streamLastEventAtMs)
      ? new Date(session.streamLastEventAtMs).toLocaleString()
      : "";
  const streamUpdatedLabel =
    typeof session.streamUpdatedMs === "number" && Number.isFinite(session.streamUpdatedMs)
      ? new Date(session.streamUpdatedMs).toLocaleString()
      : "";
  const sessionBusy = session.attachPending || session.renewPending || session.releasePending;

  return (
    <>
      <div className="mt-4">
        <FieldLabel>Connection</FieldLabel>
        <div className="mt-2">
          <FieldLabel>Connection profile</FieldLabel>
          <div className="mt-1 flex flex-wrap items-center gap-2">
            <select
              className="min-w-[220px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
              value={connection.activeProfileId}
              onChange={(e) => connection.setActiveProfileId(e.target.value)}
            >
              {(connection.profiles || []).map((p) => (
                <option key={p.id} value={p.id}>
                  {p.name}
                </option>
              ))}
            </select>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => connection.addProfile(connection.mode)}
            >
              New
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => connection.duplicateProfile()}
              disabled={(connection.profiles || []).length === 0}
            >
              Duplicate
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-2 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              disabled={(connection.profiles || []).length <= 1}
              onClick={() => {
                if ((connection.profiles || []).length <= 1) return;
                const name = connection.profileName || "profile";
                if (!window.confirm(`Delete connection profile "${name}"?`)) return;
                connection.deleteProfile(connection.activeProfileId);
              }}
            >
              Delete
            </button>
          </div>
          <input
            className="mt-2 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={connection.profileName}
            onChange={(e) => connection.setProfileName(e.target.value)}
            placeholder="Profile name"
          />
          <div className="mt-2 text-[11px] text-white/60">
            Profiles are cached locally and can sync to the server for cross-device persistence.
          </div>
          <div className="mt-3 rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70">
            <div className="text-xs font-semibold text-white/70">Server profile sync</div>
            <div className="mt-1 flex items-center justify-between gap-2">
              <label className="flex items-center gap-2">
                <input
                  type="checkbox"
                  checked={connection.serverPrefsEnabled}
                  onChange={(e) => connection.setServerPrefsEnabled(e.target.checked)}
                  disabled={!serverPrefsCanSync}
                />
                <span>
                  Sync connection profiles to {serverPrefsTarget} (no tokens)
                  {connection.serverPrefsAuto && !connection.serverPrefsUserSet ? " · auto" : ""}
                </span>
              </label>
            </div>
            <div className="mt-1 text-white/50">
              {serverPrefsCanSync
                ? `Status: ${serverPrefsStatusLabel}${connection.serverPrefsLastSyncMs ? ` · ${new Date(connection.serverPrefsLastSyncMs).toLocaleString()}` : ""}`
                : "Set a base URL first."}
            </div>
            {serverPrefsAutoNote ? <div className="mt-1 text-white/50">{serverPrefsAutoNote}</div> : null}
            {connection.serverPrefsError ? <div className="mt-1 text-rose-200">Sync error: {connection.serverPrefsError}</div> : null}
            <div className="mt-3">
              <FieldLabel>Server prefs client id</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={client.clientId}
                onChange={(e) => client.setClientId(e.target.value)}
                placeholder="e.g. webui"
              />
              <div className="mt-1 text-white/50">
                Use a stable id to share profiles across devices (server-side). Leave as default for per-device storage.
              </div>
              <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/70">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                  type="button"
                  onClick={() => client.setClientId("webui")}
                >
                  Use shared id
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                  type="button"
                  onClick={() => client.setClientId(`webui-${Date.now()}-${Math.random().toString(16).slice(2)}`)}
                >
                  New random id
                </button>
              </div>
            </div>
            <div className="mt-2 flex flex-wrap items-center gap-2">
              {connection.serverPrefsAuto && connection.serverPrefsUserSet ? (
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  onClick={() => connection.clearServerPrefsOverride()}
                  disabled={!serverPrefsCanSync}
                  title="Resume auto sync behavior"
                >
                  Use auto
                </button>
              ) : null}
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => connection.pullServerPrefs()}
                disabled={!serverPrefsCanSync || !connection.serverPrefsEnabled}
                title={connection.serverPrefsBase ? `Pull from ${connection.serverPrefsBase}` : "Set a daemon base URL first"}
              >
                Pull
              </button>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                type="button"
                onClick={() => connection.pushServerPrefs()}
                disabled={!serverPrefsCanSync || !connection.serverPrefsEnabled}
                title={connection.serverPrefsBase ? `Push to ${connection.serverPrefsBase}` : "Set a daemon base URL first"}
              >
                Push
              </button>
            </div>
          </div>
        </div>
        <select
          className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
          value={connection.mode}
          onChange={(e) => connection.setMode(e.target.value as any)}
        >
          <option value="direct">direct (agentd)</option>
          <option value="broker">broker (OIDC + agent_id)</option>
        </select>
        <div className="mt-2 text-[11px] text-white/60">
          {connection.mode === "direct"
            ? "Direct: the browser calls agentd over HTTP."
            : "Broker: the browser calls a broker (OIDC), which proxies to a connected agent by id."}
        </div>
      </div>

      {connection.mode === "direct" ? (
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
      ) : (
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
                {brokerAgents.map((a: any) => {
                  const id = typeof a?.agent_id === "string" ? a.agent_id : "";
                  if (!id) return null;
                  const connected = a?.connected === true;
                  const lastSeen = typeof a?.last_seen_unix_ms === "number" ? a.last_seen_unix_ms : 0;
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
                      title={a?.remote_addr ? `remote=${String(a.remote_addr)}` : ""}
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
                {brokerDeployments.map((d: any) => {
                  const id = typeof d?.deployment_id === "string" ? d.deployment_id : "";
                  if (!id) return null;
                  const connected = d?.connected === true;
                  const lastSeen = typeof d?.last_seen_unix_ms === "number" ? d.last_seen_unix_ms : 0;
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
                      title={d?.remote_addr ? `remote=${String(d.remote_addr)}` : ""}
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
      )}

      <div className="mt-4 grid grid-cols-2 gap-3">
        <div className="col-span-2">
          <FieldLabel>Session</FieldLabel>
          <input
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            data-testid="session-id-input"
            value={session.id}
            onChange={(e) => session.setId(e.target.value)}
          />
          <div className="mt-2 grid grid-cols-2 gap-3">
            <div>
              <FieldLabel>Lease seconds</FieldLabel>
              <input
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                data-testid="session-lease-seconds-input"
                value={session.leaseSeconds}
                onChange={(e) => session.setLeaseSeconds(e.target.value)}
                inputMode="numeric"
                placeholder="90"
              />
            </div>
            <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70">
              <div className="font-semibold text-white/80">Lease status</div>
              <div className="mt-1">
                {leaseRole === "owner"
                  ? "Owner: this client currently holds the attachment lease."
                  : leaseRole === "observer"
                    ? `Observer: ${holderClientId || "another client"} currently holds the active lease.`
                    : "No active lease reported for this session."}
              </div>
              {holderClientId ? <div className="mt-1">holder: <code className="font-mono text-white/80">{holderClientId}</code></div> : null}
              {attachment?.lease_seconds ? <div className="mt-1">lease_seconds: {attachment.lease_seconds}</div> : null}
              {leaseExpiresLabel ? <div className="mt-1">expires: {leaseExpiresLabel}</div> : null}
              {session.info?.thread_id ? <div className="mt-1">thread: <code className="font-mono text-white/80">{session.info.thread_id}</code></div> : null}
            </div>
          </div>
          <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px]">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => session.attach()}
              disabled={!String(session.id || "").trim() || sessionBusy}
            >
              {session.attachPending ? "Attaching…" : "Attach / claim"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => session.renewAttachment()}
              disabled={!String(session.id || "").trim() || sessionBusy}
            >
              {session.renewPending ? "Renewing…" : "Renew lease"}
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/80 hover:bg-black/40 disabled:opacity-50"
              type="button"
              onClick={() => session.releaseAttachment()}
              disabled={!String(session.id || "").trim() || sessionBusy}
            >
              {session.releasePending ? "Releasing…" : "Release lease"}
            </button>
            {session.leaseConflict ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-white/70 hover:bg-black/40"
                type="button"
                onClick={() => session.clearLeaseConflict()}
              >
                Clear conflict
              </button>
            ) : null}
          </div>
          <div className="mt-2 text-[11px] text-white/60">
            In broker mode, these controls use the broker session attachment surface so owner/observer/rival conflicts stay explicit.
          </div>
          <div className="mt-2 rounded-md border border-white/10 bg-black/20 px-3 py-2 text-[11px] text-white/70" data-testid="session-stream-status">
            <div className="font-semibold text-white/80">Session event stream</div>
            <div className="mt-1">
              status: <span className="font-mono text-white/80">{session.streamStatus}</span>
            </div>
            {session.streamLastEventId ? (
              <div className="mt-1">
                last_event_id: <code className="font-mono text-white/80">{session.streamLastEventId}</code>
              </div>
            ) : null}
            <div className="mt-1">buffered_events: {session.streamBufferedCount}</div>
            {streamLastEventLabel ? <div className="mt-1">last_event_at: {streamLastEventLabel}</div> : null}
            {streamUpdatedLabel ? <div className="mt-1">persisted_at: {streamUpdatedLabel}</div> : null}
            {session.streamError ? <div className="mt-1 text-rose-200">stream error: {session.streamError}</div> : null}
            <div className="mt-1 text-white/50">Broker session replay uses <code className="font-mono">Last-Event-ID</code> and persists a bounded event buffer locally.</div>
          </div>
          {session.attachError ? <div className="mt-2 text-[11px] text-rose-200">Attach failed: {session.attachError}</div> : null}
          {session.renewError ? <div className="mt-2 text-[11px] text-rose-200">Renew failed: {session.renewError}</div> : null}
          {session.releaseError ? <div className="mt-2 text-[11px] text-rose-200">Release failed: {session.releaseError}</div> : null}
          {session.leaseConflict ? (
            <div className="mt-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-3 py-2 text-[11px] text-amber-100">
              <div className="font-semibold">Lease conflict</div>
              <div className="mt-1">{session.leaseConflict.message}</div>
              {session.leaseConflict.requestedClientId ? (
                <div className="mt-1">
                  requested client: <code className="font-mono">{session.leaseConflict.requestedClientId}</code>
                </div>
              ) : null}
              {session.leaseConflict.currentAttachment?.client_id ? (
                <div className="mt-1">
                  current holder: <code className="font-mono">{session.leaseConflict.currentAttachment.client_id}</code>
                </div>
              ) : null}
            </div>
          ) : null}
        </div>
        <div className="col-span-2">
          <SettingsBrokerSessionOperatorsSection connection={connection} client={client} session={session} />
        </div>
        <div>
          <FieldLabel>Tools</FieldLabel>
          <select
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.tools}
            onChange={(e) => run.setTools(e.target.value as any)}
          >
            <option value="host">host</option>
            <option value="basic">basic</option>
            <option value="none">none</option>
          </select>
        </div>
        <div>
          <FieldLabel>Host policy</FieldLabel>
          <select
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.hostPolicy}
            onChange={(e) => run.setHostPolicy(e.target.value as any)}
            disabled={run.tools !== "host"}
          >
            <option value="full">full</option>
            <option value="readonly">readonly</option>
          </select>
        </div>
        <div className="col-span-2">
          <FieldLabel>Automation profile</FieldLabel>
          <select
            className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
            value={run.automationProfile}
            onChange={(e) => run.setAutomationProfile(e.target.value)}
            disabled={!automationOverrideAllowed}
          >
            <option value="">{automationDefault ? `default (${automationDefault})` : "default (daemon config)"}</option>
            {automationProfiles.map((p) => (
              <option key={p} value={p}>
                {p}
              </option>
            ))}
          </select>
          <div className="mt-1 text-[11px] text-white/60">
            Overrides yolo/host policy/policy mode when set. Use default to follow daemon config.
          </div>
          {!automationOverrideAllowed ? (
            <div className="mt-1 text-[11px] text-amber-200">Per-run automation override disabled by daemon caps.</div>
          ) : null}
        </div>
      </div>
    </>
  );
}
