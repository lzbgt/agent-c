import React from "react";

import useLocalStorageState from "./useLocalStorageState";
import {
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiGetClientPrefs,
  apiPostClientPrefs,
  daemonFetchInit,
  type ApiAuth,
} from "../api";
import type { AgentUIDefaults, ConnectionMode, ServerPrefsMode } from "../runtime_config";
import type { ConnectionSettings } from "./uiSettingsTypes";
import {
  buildServerPrefs,
  mergeProfileSecrets,
  mergeServerPrefs,
  type ConnectionProfile,
  type ConnectionProfileSecretMap,
  type ServerPrefs,
} from "./uiSettingsProfiles";

type UseConnectionServerPrefsParams = {
  defaults: AgentUIDefaults;
  clientId: string;
  connectionMode: ConnectionMode;
  serverPrefsBase: string;
  daemonAuth: ApiAuth;
  brokerAuthReady: boolean;
  connectionProfiles: ConnectionProfile[];
  connectionProfileSecrets: ConnectionProfileSecretMap;
  activeProfileId: string;
  setConnectionProfiles: React.Dispatch<React.SetStateAction<ConnectionProfile[]>>;
  setActiveProfileId: React.Dispatch<React.SetStateAction<string>>;
};

type UseConnectionServerPrefsResult = {
  serverPrefsEnabled: boolean;
  serverPrefsAuto: boolean;
  serverPrefsUserSet: boolean;
  serverPrefsAutoStatus: ConnectionSettings["serverPrefsAutoStatus"];
  serverPrefsAutoError: string | null;
  clearServerPrefsOverride: () => void;
  setServerPrefsEnabled: React.Dispatch<React.SetStateAction<boolean>>;
  serverPrefsStatus: ConnectionSettings["serverPrefsStatus"];
  serverPrefsError: string | null;
  serverPrefsLastSyncMs: number | null;
  pullServerPrefs: () => Promise<void>;
  pushServerPrefs: () => Promise<void>;
};

export default function useConnectionServerPrefs({
  defaults,
  clientId,
  connectionMode,
  serverPrefsBase,
  daemonAuth,
  brokerAuthReady,
  connectionProfiles,
  connectionProfileSecrets,
  activeProfileId,
  setConnectionProfiles,
  setActiveProfileId,
}: UseConnectionServerPrefsParams): UseConnectionServerPrefsResult {
  const serverPrefsDefaultMode: ServerPrefsMode = defaults.serverPrefsMode;
  const initialServerPrefsUserSet = React.useMemo(() => {
    if (typeof window === "undefined") return false;
    try {
      const raw = window.localStorage.getItem("agentui.serverPrefsEnabledSet");
      if (raw !== null) {
        const parsed = JSON.parse(raw);
        return typeof parsed === "boolean" ? parsed : false;
      }
      const legacy = window.localStorage.getItem("agentui.serverPrefsEnabled");
      return legacy !== null;
    } catch {
      return false;
    }
  }, []);
  const [serverPrefsEnabled, setServerPrefsEnabledState] = useLocalStorageState(
    "agentui.serverPrefsEnabled",
    serverPrefsDefaultMode === "on",
  );
  const [serverPrefsUserSet, setServerPrefsUserSet] = useLocalStorageState(
    "agentui.serverPrefsEnabledSet",
    initialServerPrefsUserSet,
  );
  const [serverPrefsAutoEnabled, setServerPrefsAutoEnabled] = React.useState<boolean>(false);
  const [serverPrefsAutoStatus, setServerPrefsAutoStatus] = React.useState<ConnectionSettings["serverPrefsAutoStatus"]>("idle");
  const [serverPrefsAutoError, setServerPrefsAutoError] = React.useState<string | null>(null);
  const [serverPrefsStatus, setServerPrefsStatus] = React.useState<ConnectionSettings["serverPrefsStatus"]>("idle");
  const [serverPrefsError, setServerPrefsError] = React.useState<string | null>(null);
  const [serverPrefsLastSyncMs, setServerPrefsLastSyncMs] = React.useState<number | null>(null);
  const serverPrefsLastPayloadRef = React.useRef("");
  const serverPrefsPullInFlightRef = React.useRef(false);

  const serverPrefsAuto = serverPrefsDefaultMode === "auto" && !serverPrefsUserSet;
  const serverPrefsEffectiveEnabled = serverPrefsUserSet
    ? serverPrefsEnabled
    : serverPrefsDefaultMode === "auto"
      ? serverPrefsAutoEnabled
      : serverPrefsDefaultMode === "on";

  React.useEffect(() => {
    if (!serverPrefsAuto) {
      setServerPrefsAutoEnabled(false);
      setServerPrefsAutoStatus("idle");
      setServerPrefsAutoError(null);
      return;
    }
    const baseTrimmed = String(serverPrefsBase || "").trim();
    if (!baseTrimmed) {
      setServerPrefsAutoEnabled(false);
      setServerPrefsAutoStatus("idle");
      setServerPrefsAutoError(null);
      return;
    }
    if (connectionMode === "broker" && !brokerAuthReady) {
      setServerPrefsAutoEnabled(false);
      setServerPrefsAutoStatus("auth_required");
      setServerPrefsAutoError(null);
      return;
    }
    let cancelled = false;
    setServerPrefsAutoStatus("checking");
    setServerPrefsAutoError(null);
    (async () => {
      try {
        if (connectionMode === "broker") {
          const baseNoSlash = baseTrimmed.replace(/\/+$/, "");
          const response = await fetch(`${baseNoSlash}/v1/caps`, daemonFetchInit(daemonAuth));
          if (!response.ok) throw new Error(`caps ${response.status}`);
          const payload = await response.json();
          const enabled = !!payload?.features?.client_prefs?.enabled;
          if (cancelled) return;
          setServerPrefsAutoEnabled(enabled);
          setServerPrefsAutoStatus(enabled ? "ready" : "unsupported");
          return;
        }
        const response = await fetch(`${baseTrimmed}/api/v1/caps`, daemonFetchInit(daemonAuth));
        if (response.status === 401 || response.status === 403) {
          if (cancelled) return;
          setServerPrefsAutoEnabled(false);
          setServerPrefsAutoStatus("auth_required");
          setServerPrefsAutoError(null);
          return;
        }
        if (!response.ok) throw new Error(`caps ${response.status}`);
        const payload = await response.json();
        const enabled = !!payload?.features?.client_prefs?.enabled;
        if (cancelled) return;
        setServerPrefsAutoEnabled(enabled);
        setServerPrefsAutoStatus(enabled ? "ready" : "unsupported");
      } catch (err) {
        if (cancelled) return;
        setServerPrefsAutoEnabled(false);
        setServerPrefsAutoStatus("error");
        setServerPrefsAutoError(String(err instanceof Error ? err.message : err));
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [brokerAuthReady, connectionMode, daemonAuth, serverPrefsAuto, serverPrefsBase]);

  const serverPrefsPayload = React.useMemo(
    () => buildServerPrefs(connectionProfiles, activeProfileId),
    [connectionProfiles, activeProfileId],
  );
  const serverPrefsPayloadJson = React.useMemo(() => JSON.stringify(serverPrefsPayload), [serverPrefsPayload]);
  const serverPrefsPayloadKey = React.useMemo(
    () => JSON.stringify({ client_id: String(clientId || "webui"), prefs: serverPrefsPayload }),
    [clientId, serverPrefsPayload],
  );
  const serverPrefsClientKind = "webui";
  const serverPrefsCanUse = serverPrefsEffectiveEnabled && String(serverPrefsBase || "").trim().length > 0;
  const connectionProfilesRef = React.useRef(
    connectionProfiles.map((profile) => mergeProfileSecrets(profile, connectionProfileSecrets[profile.id])),
  );

  React.useEffect(() => {
    connectionProfilesRef.current = connectionProfiles.map((profile) =>
      mergeProfileSecrets(profile, connectionProfileSecrets[profile.id]),
    );
  }, [connectionProfileSecrets, connectionProfiles]);

  const pullServerPrefs = React.useCallback(async () => {
    if (!serverPrefsCanUse) return;
    if (serverPrefsPullInFlightRef.current) return;
    serverPrefsPullInFlightRef.current = true;
    setServerPrefsStatus("loading");
    setServerPrefsError(null);
    try {
      const response =
        connectionMode === "broker"
          ? await apiBrokerGetClientPrefs(serverPrefsBase, String(clientId || "webui"), serverPrefsClientKind, daemonAuth)
          : await apiGetClientPrefs(serverPrefsBase, String(clientId || "webui"), serverPrefsClientKind, daemonAuth);
      if (!response.ok) {
        throw new Error(response.error || response.err || response.code || "client prefs fetch failed");
      }
      if (response.found && response.prefs && typeof response.prefs === "object") {
        const merged = mergeServerPrefs(response.prefs as ServerPrefs, connectionProfilesRef.current, defaults);
        setConnectionProfiles(merged.profiles);
        if (merged.activeProfileId) setActiveProfileId(merged.activeProfileId);
        serverPrefsLastPayloadRef.current = JSON.stringify({
          client_id: String(clientId || "webui"),
          prefs: buildServerPrefs(merged.profiles, merged.activeProfileId),
        });
      }
      setServerPrefsLastSyncMs(typeof response.updated_utc_ms === "number" ? response.updated_utc_ms : Date.now());
      setServerPrefsStatus("synced");
    } catch (err) {
      setServerPrefsStatus("error");
      setServerPrefsError(String(err instanceof Error ? err.message : err));
    } finally {
      serverPrefsPullInFlightRef.current = false;
    }
  }, [
    clientId,
    connectionMode,
    daemonAuth,
    defaults,
    serverPrefsBase,
    serverPrefsCanUse,
    setActiveProfileId,
    setConnectionProfiles,
  ]);

  const pushServerPrefs = React.useCallback(async () => {
    if (!serverPrefsCanUse) return;
    if (serverPrefsPayloadKey === serverPrefsLastPayloadRef.current) return;
    setServerPrefsStatus("loading");
    setServerPrefsError(null);
    try {
      const request = { client_id: String(clientId || "webui"), client_kind: serverPrefsClientKind, prefs: serverPrefsPayload };
      const response =
        connectionMode === "broker"
          ? await apiBrokerPostClientPrefs(serverPrefsBase, request, daemonAuth)
          : await apiPostClientPrefs(serverPrefsBase, request, daemonAuth);
      if (!response.ok) {
        throw new Error(response.error || response.err || response.code || "client prefs update failed");
      }
      serverPrefsLastPayloadRef.current = serverPrefsPayloadKey;
      setServerPrefsLastSyncMs(typeof response.updated_utc_ms === "number" ? response.updated_utc_ms : Date.now());
      setServerPrefsStatus("synced");
    } catch (err) {
      setServerPrefsStatus("error");
      setServerPrefsError(String(err instanceof Error ? err.message : err));
    }
  }, [
    clientId,
    connectionMode,
    daemonAuth,
    serverPrefsBase,
    serverPrefsCanUse,
    serverPrefsPayload,
    serverPrefsPayloadKey,
  ]);

  const setServerPrefsEnabled = React.useCallback<React.Dispatch<React.SetStateAction<boolean>>>(
    (next) => {
      setServerPrefsUserSet(true);
      setServerPrefsEnabledState((prev) => (typeof next === "function" ? next(prev) : next));
    },
    [setServerPrefsEnabledState, setServerPrefsUserSet],
  );

  const clearServerPrefsOverride = React.useCallback(() => {
    setServerPrefsUserSet(false);
    setServerPrefsEnabledState(serverPrefsDefaultMode === "on");
  }, [serverPrefsDefaultMode, setServerPrefsEnabledState, setServerPrefsUserSet]);

  React.useEffect(() => {
    if (!serverPrefsCanUse) return;
    void pullServerPrefs();
  }, [pullServerPrefs, serverPrefsCanUse]);

  React.useEffect(() => {
    if (!serverPrefsCanUse) return;
    if (serverPrefsPayloadJson === serverPrefsLastPayloadRef.current) return;
    const timeoutId = window.setTimeout(() => {
      void pushServerPrefs();
    }, 600);
    return () => window.clearTimeout(timeoutId);
  }, [pushServerPrefs, serverPrefsCanUse, serverPrefsPayloadJson, serverPrefsPayloadKey]);

  return {
    serverPrefsEnabled: serverPrefsEffectiveEnabled,
    serverPrefsAuto,
    serverPrefsUserSet,
    serverPrefsAutoStatus,
    serverPrefsAutoError,
    clearServerPrefsOverride,
    setServerPrefsEnabled,
    serverPrefsStatus,
    serverPrefsError,
    serverPrefsLastSyncMs,
    pullServerPrefs,
    pushServerPrefs,
  };
}
