import React from "react";

import type { ApiAuth } from "../api";
import type { AgentUIDefaults, ConnectionMode } from "../runtime_config";
import type { ConnectionSettings } from "./uiSettingsTypes";
import { normalizeHttpBase, type ConnectionProfile } from "./uiSettingsProfiles";
import useBrokerCookieSession from "./useBrokerCookieSession";
import useConnectionServerPrefs from "./useConnectionServerPrefs";
import useUiConnectionProfileState, { type ConnectionProfileUpdater } from "./useUiConnectionProfileState";

type UseUiConnectionSettingsParams = {
  defaults: AgentUIDefaults;
  clientId: string;
};

export type UseUiConnectionSettingsResult = {
  connection: ConnectionSettings;
  activeProfile: ConnectionProfile;
  updateActiveProfile: ConnectionProfileUpdater;
};

export default function useUiConnectionSettings({
  defaults,
  clientId,
}: UseUiConnectionSettingsParams): UseUiConnectionSettingsResult {
  const profileState = useUiConnectionProfileState({ defaults });

  const connectionMode = profileState.activeProfile.mode;
  const base = profileState.activeProfile.base;
  const brokerBase = profileState.activeProfile.brokerBase;
  const brokerAgentId = profileState.activeProfile.brokerAgentId;
  const brokerDeploymentId = profileState.activeProfile.brokerDeploymentId;
  const brokerCookieAuth = profileState.activeProfile.brokerCookieAuth;
  const brokerAuthToken = profileState.activeProfile.brokerAuthToken;
  const daemonAuthToken = profileState.activeProfile.daemonAuthToken;

  const setMode = React.useCallback<React.Dispatch<React.SetStateAction<ConnectionMode>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.mode) : next;
        if (value === prev.mode) return prev;
        return { ...prev, mode: value };
      });
    },
    [profileState],
  );

  const setBase = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.base) : next;
        if (value === prev.base) return prev;
        return { ...prev, base: value };
      });
    },
    [profileState],
  );

  const setBrokerBase = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerBase) : next;
        if (value === prev.brokerBase) return prev;
        return { ...prev, brokerBase: value };
      });
    },
    [profileState],
  );

  const setBrokerAgentId = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerAgentId) : next;
        if (value === prev.brokerAgentId) return prev;
        return { ...prev, brokerAgentId: value };
      });
    },
    [profileState],
  );

  const setBrokerDeploymentId = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerDeploymentId) : next;
        if (value === prev.brokerDeploymentId) return prev;
        return { ...prev, brokerDeploymentId: value };
      });
    },
    [profileState],
  );

  const setBrokerCookieAuth = React.useCallback<React.Dispatch<React.SetStateAction<boolean>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerCookieAuth) : next;
        if (value === prev.brokerCookieAuth) return prev;
        return { ...prev, brokerCookieAuth: value };
      });
    },
    [profileState],
  );

  const setBrokerAuthToken = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerAuthToken) : next;
        if (value === prev.brokerAuthToken) return prev;
        return { ...prev, brokerAuthToken: value };
      });
    },
    [profileState],
  );

  const setDaemonAuthToken = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      profileState.updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.daemonAuthToken) : next;
        if (value === prev.daemonAuthToken) return prev;
        return { ...prev, daemonAuthToken: value };
      });
    },
    [profileState],
  );

  const effectiveBase = React.useMemo(() => {
    if (connectionMode === "broker") {
      const normalizedBrokerBase = normalizeHttpBase(brokerBase, "https://127.0.0.1:8443", "https");
      const agentId = String(brokerAgentId || "").trim();
      if (!agentId) return normalizedBrokerBase;
      return `${normalizedBrokerBase}/v1/agents/${encodeURIComponent(agentId)}/proxy`;
    }
    return normalizeHttpBase(base, "http://127.0.0.1:8123", "http");
  }, [base, brokerAgentId, brokerBase, connectionMode]);

  const effectiveSseBase = React.useMemo(() => {
    if (connectionMode !== "broker") return effectiveBase;
    const brokerBaseTrimmed = String(brokerBase || "").trim().replace(/\/+$/, "");
    const withScheme = /^https?:\/\//i.test(brokerBaseTrimmed) ? brokerBaseTrimmed : `https://${brokerBaseTrimmed}`;
    const agentId = String(brokerAgentId || "").trim();
    if (!agentId) return withScheme;
    return `${withScheme}/v1/agents/${encodeURIComponent(agentId)}/proxy_sse`;
  }, [brokerAgentId, brokerBase, connectionMode, effectiveBase]);

  const daemonAuth = React.useMemo<ApiAuth>(() => {
    if (connectionMode === "broker") {
      return {
        mode: "broker",
        token: brokerAuthToken,
        agentdToken: daemonAuthToken,
        deploymentId: brokerDeploymentId,
        useCookieAuth: brokerCookieAuth,
      };
    }
    return { mode: "direct", token: daemonAuthToken };
  }, [brokerAuthToken, brokerCookieAuth, brokerDeploymentId, connectionMode, daemonAuthToken]);

  const authKey = React.useMemo(() => {
    const mode = daemonAuth.mode;
    const token = typeof daemonAuth.token === "string" ? daemonAuth.token.trim() : "";
    const agentdToken =
      daemonAuth.mode === "broker" && typeof daemonAuth.agentdToken === "string" ? daemonAuth.agentdToken.trim() : "";
    const deploymentId =
      daemonAuth.mode === "broker" && typeof daemonAuth.deploymentId === "string" ? daemonAuth.deploymentId.trim() : "";
    const cookie = daemonAuth.mode === "broker" && daemonAuth.useCookieAuth ? "cookie=1" : "cookie=0";
    const profileId = String(profileState.activeProfileId || "default");
    return mode === "broker"
      ? `broker:pid=${profileId}:dep=${deploymentId}:${cookie}:tlen=${token.length}:alen=${agentdToken.length}`
      : `direct:pid=${profileId}:tlen=${token.length}`;
  }, [daemonAuth, profileState.activeProfileId]);

  const serverPrefsBase = React.useMemo(() => {
    if (connectionMode === "broker") {
      return normalizeHttpBase(brokerBase, "https://127.0.0.1:8443", "https");
    }
    return normalizeHttpBase(base, "http://127.0.0.1:8123", "http");
  }, [base, brokerBase, connectionMode]);

  const brokerAuthReady =
    connectionMode !== "broker" || brokerCookieAuth || String(brokerAuthToken || "").trim().length > 0;

  const brokerCookieSession = useBrokerCookieSession({
    activeProfileId: profileState.activeProfileId,
    brokerAuthToken,
    brokerCookieAuth,
    connectionMode,
    serverPrefsBase,
    updateActiveProfile: profileState.updateActiveProfile,
  });

  const serverPrefs = useConnectionServerPrefs({
    defaults,
    clientId,
    connectionMode,
    serverPrefsBase,
    daemonAuth,
    brokerAuthReady,
    connectionProfiles: profileState.connectionProfiles,
    connectionProfileSecrets: profileState.connectionProfileSecrets,
    activeProfileId: profileState.activeProfileId,
    setConnectionProfiles: profileState.setConnectionProfiles,
    setActiveProfileId: profileState.setActiveProfileId,
  });

  return {
    connection: {
      profiles: profileState.connectionProfiles,
      activeProfileId: profileState.activeProfileId,
      setActiveProfileId: profileState.setActiveProfileId,
      profileName: profileState.activeProfile.name,
      setProfileName: profileState.setProfileName,
      addProfile: profileState.addProfile,
      duplicateProfile: profileState.duplicateProfile,
      deleteProfile: profileState.deleteProfile,
      mode: connectionMode,
      setMode,
      base,
      setBase,
      brokerBase,
      setBrokerBase,
      brokerAgentId,
      setBrokerAgentId,
      brokerDeploymentId,
      setBrokerDeploymentId,
      brokerCookieAuth,
      setBrokerCookieAuth,
      brokerAuthToken,
      setBrokerAuthToken,
      brokerCookieSessionStatus: brokerCookieSession.brokerCookieSessionStatus,
      brokerCookieSessionError: brokerCookieSession.brokerCookieSessionError,
      clearBrokerAuthCookie: brokerCookieSession.clearBrokerAuthCookie,
      daemonAuthToken,
      setDaemonAuthToken,
      serverPrefsEnabled: serverPrefs.serverPrefsEnabled,
      serverPrefsAuto: serverPrefs.serverPrefsAuto,
      serverPrefsUserSet: serverPrefs.serverPrefsUserSet,
      serverPrefsAutoStatus: serverPrefs.serverPrefsAutoStatus,
      serverPrefsAutoError: serverPrefs.serverPrefsAutoError,
      clearServerPrefsOverride: serverPrefs.clearServerPrefsOverride,
      setServerPrefsEnabled: serverPrefs.setServerPrefsEnabled,
      serverPrefsStatus: serverPrefs.serverPrefsStatus,
      serverPrefsError: serverPrefs.serverPrefsError,
      serverPrefsLastSyncMs: serverPrefs.serverPrefsLastSyncMs,
      serverPrefsBase,
      pullServerPrefs: serverPrefs.pullServerPrefs,
      pushServerPrefs: serverPrefs.pushServerPrefs,
      effectiveBase,
      effectiveSseBase,
      daemonAuth,
      authKey,
    },
    activeProfile: profileState.activeProfile,
    updateActiveProfile: profileState.updateActiveProfile,
  };
}
