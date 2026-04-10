import FieldLabel from "../FieldLabel";
import type { SettingsConnectionSectionProps } from "./settingsConnectionTypes";

type SettingsConnectionProfilesSectionProps = Pick<
  SettingsConnectionSectionProps,
  | "connection"
  | "client"
  | "serverPrefsCanSync"
  | "serverPrefsTarget"
  | "serverPrefsStatusLabel"
  | "serverPrefsAutoNote"
>;

function isConnectionMode(value: string): value is SettingsConnectionSectionProps["connection"]["mode"] {
  return value === "direct" || value === "broker";
}

export default function SettingsConnectionProfilesSection(props: SettingsConnectionProfilesSectionProps) {
  const { connection, client, serverPrefsCanSync, serverPrefsTarget, serverPrefsStatusLabel, serverPrefsAutoNote } = props;

  return (
    <div className="mt-4" data-testid="settings-connection-profiles-section">
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
        onChange={(e) => {
          const next = e.target.value;
          if (isConnectionMode(next)) connection.setMode(next);
        }}
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
  );
}
