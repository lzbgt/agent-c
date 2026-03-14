import React from "react";
import useLocalStorageState from "./useLocalStorageState";
import useSessionStorageState from "./useSessionStorageState";
import {
  apiBrokerCreateAuthSession,
  apiBrokerDeleteAuthSession,
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
  CONNECTION_PROFILE_SECRETS_KEY,
  LEGACY_SECRET_KEYS,
  buildServerPrefs,
  extractProfileSecrets,
  generateProfileId,
  mergeProfileSecrets,
  mergeServerPrefs,
  normalizeHttpBase,
  normalizeProfile,
  normalizeSecretMap,
  profileNameDefault,
  readLegacyMode,
  readLegacySecretString,
  readLegacyString,
  sanitizeProfileForPersistence,
  secretsEqual,
  stripSecretRunOverrides,
  type ConnectionProfile,
  type ConnectionProfileSecretMap,
  type ServerPrefs,
} from "./uiSettingsProfiles";

type UseUiConnectionSettingsParams = {
  defaults: AgentUIDefaults;
  clientId: string;
};

export type ConnectionProfileUpdater = (update: (prev: ConnectionProfile) => ConnectionProfile) => void;

export type UseUiConnectionSettingsResult = {
  connection: ConnectionSettings;
  activeProfile: ConnectionProfile;
  updateActiveProfile: ConnectionProfileUpdater;
};

export default function useUiConnectionSettings({
  defaults,
  clientId,
}: UseUiConnectionSettingsParams): UseUiConnectionSettingsResult {
  const [connectionProfiles, setConnectionProfiles] = useLocalStorageState<ConnectionProfile[]>(
    "agentui.connectionProfiles",
    [],
  );
  const [connectionProfileSecrets, setConnectionProfileSecrets] = useSessionStorageState<ConnectionProfileSecretMap>(
    CONNECTION_PROFILE_SECRETS_KEY,
    {},
  );
  const [activeProfileId, setActiveProfileId] = useLocalStorageState("agentui.connectionProfileActive", "");
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
  const [brokerCookieSessionStatus, setBrokerCookieSessionStatus] = React.useState<ConnectionSettings["brokerCookieSessionStatus"]>(
    "idle",
  );
  const [brokerCookieSessionError, setBrokerCookieSessionError] = React.useState<string | null>(null);
  const [serverPrefsStatus, setServerPrefsStatus] = React.useState<ConnectionSettings["serverPrefsStatus"]>("idle");
  const [serverPrefsError, setServerPrefsError] = React.useState<string | null>(null);
  const [serverPrefsLastSyncMs, setServerPrefsLastSyncMs] = React.useState<number | null>(null);
  const serverPrefsLastPayloadRef = React.useRef("");
  const serverPrefsPullInFlightRef = React.useRef(false);
  const brokerCookieExchangeKeyRef = React.useRef("");

  const buildLegacyProfile = React.useCallback((): ConnectionProfile => {
    const mode = readLegacyMode("agentui.connectionMode", defaults.connectionMode);
    return normalizeProfile(
      {
        mode,
        base: readLegacyString("agentui.base", defaults.daemonBaseUrl),
        brokerBase: readLegacyString("agentui.brokerBase", defaults.brokerBaseUrl),
        brokerAgentId: readLegacyString("agentui.brokerAgentId", defaults.brokerAgentId),
        brokerDeploymentId: readLegacyString("agentui.brokerDeploymentId", defaults.brokerDeploymentId),
        brokerCookieAuth: defaults.brokerCookieAuth,
        brokerAuthToken: readLegacySecretString("agentui.brokerAuthToken", defaults.brokerAuthToken),
        daemonAuthToken: readLegacySecretString("agentui.daemonAuthToken", defaults.daemonAuthToken),
      },
      defaults,
    );
  }, [defaults]);

  React.useEffect(() => {
    if (connectionProfiles.length === 0) {
      const legacy = buildLegacyProfile();
      const secret = extractProfileSecrets(legacy);
      setConnectionProfiles([sanitizeProfileForPersistence(legacy)]);
      if (Object.keys(secret).length > 0) {
        setConnectionProfileSecrets((prev) => ({ ...normalizeSecretMap(prev), [legacy.id]: secret }));
      }
      setActiveProfileId(legacy.id);
      return;
    }
    let changed = false;
    let secretsChanged = false;
    const nextSecrets = { ...normalizeSecretMap(connectionProfileSecrets) };
    const normalized = connectionProfiles.map((profile) => {
      const normalizedProfile = normalizeProfile(profile, defaults);
      const extracted = extractProfileSecrets(profile);
      if (Object.keys(extracted).length > 0) {
        const mergedSecret = { ...(nextSecrets[normalizedProfile.id] || {}), ...extracted };
        if (!secretsEqual(nextSecrets[normalizedProfile.id], mergedSecret)) {
          nextSecrets[normalizedProfile.id] = mergedSecret;
          secretsChanged = true;
        }
      }
      const persisted = sanitizeProfileForPersistence(normalizedProfile);
      const prevOverridesJson = JSON.stringify(stripSecretRunOverrides(profile.runOverrides) ?? null);
      const nextOverridesJson = JSON.stringify(persisted.runOverrides ?? null);
      const prevOverridesEnabled = typeof profile.runOverridesEnabled === "boolean" ? profile.runOverridesEnabled : false;
      if (
        persisted.id !== profile.id ||
        persisted.name !== profile.name ||
        persisted.mode !== profile.mode ||
        persisted.base !== profile.base ||
        persisted.brokerBase !== profile.brokerBase ||
        persisted.brokerAgentId !== profile.brokerAgentId ||
        persisted.brokerDeploymentId !== profile.brokerDeploymentId ||
        persisted.brokerCookieAuth !==
          (typeof profile.brokerCookieAuth === "boolean" ? profile.brokerCookieAuth : defaults.brokerCookieAuth) ||
        persisted.brokerAuthToken !== (profile.brokerAuthToken || "") ||
        persisted.daemonAuthToken !== (profile.daemonAuthToken || "") ||
        persisted.runOverridesEnabled !== prevOverridesEnabled ||
        prevOverridesJson !== nextOverridesJson
      ) {
        changed = true;
      }
      return persisted;
    });
    if (changed) setConnectionProfiles(normalized);
    if (secretsChanged) setConnectionProfileSecrets(nextSecrets);
    if (!normalized.some((profile) => profile.id === activeProfileId)) {
      setActiveProfileId(normalized[0]?.id || "");
    }
  }, [
    activeProfileId,
    buildLegacyProfile,
    connectionProfileSecrets,
    connectionProfiles,
    defaults,
    setActiveProfileId,
    setConnectionProfileSecrets,
    setConnectionProfiles,
  ]);

  React.useEffect(() => {
    if (typeof window === "undefined") return;
    for (const key of LEGACY_SECRET_KEYS) {
      try {
        window.localStorage.removeItem(key);
        window.sessionStorage.removeItem(key);
      } catch {
        // ignore storage failures
      }
    }
  }, []);

  const fallbackProfile = React.useMemo(
    () =>
      normalizeProfile(
        {
          id: "fallback",
          name: "default",
          mode: defaults.connectionMode,
          base: defaults.daemonBaseUrl,
          brokerBase: defaults.brokerBaseUrl,
          brokerAgentId: defaults.brokerAgentId,
          brokerDeploymentId: defaults.brokerDeploymentId,
          brokerCookieAuth: defaults.brokerCookieAuth,
          brokerAuthToken: defaults.brokerAuthToken,
          daemonAuthToken: defaults.daemonAuthToken,
        },
        defaults,
      ),
    [defaults],
  );

  const activeProfile = React.useMemo(() => {
    if (connectionProfiles.length === 0) return fallbackProfile;
    const found = connectionProfiles.find((profile) => profile.id === activeProfileId);
    const baseProfile = found || connectionProfiles[0];
    return mergeProfileSecrets(baseProfile, connectionProfileSecrets[baseProfile.id]);
  }, [activeProfileId, connectionProfileSecrets, connectionProfiles, fallbackProfile]);

  const updateActiveProfile = React.useCallback<ConnectionProfileUpdater>(
    (update) => {
      const currentProfile = activeProfile;
      const nextProfile = normalizeProfile(update(currentProfile), defaults);
      const nextSecretId = nextProfile.id;
      const nextSecretState = extractProfileSecrets(nextProfile);
      const sanitized = sanitizeProfileForPersistence(nextProfile);
      setConnectionProfiles((prev) => {
        if (prev.length === 0) return prev;
        const idx = prev.findIndex((profile) => profile.id === currentProfile.id);
        const useIdx = idx >= 0 ? idx : prev.findIndex((profile) => profile.id === activeProfileId);
        const resolvedIdx = useIdx >= 0 ? useIdx : 0;
        if (JSON.stringify(sanitized) === JSON.stringify(prev[resolvedIdx])) return prev;
        const next = prev.slice();
        next[resolvedIdx] = sanitized;
        return next;
      });
      if (nextSecretId) {
        setConnectionProfileSecrets((prev) => {
          const nextSecrets = { ...normalizeSecretMap(prev) };
          if (nextSecretId !== currentProfile.id) {
            delete nextSecrets[currentProfile.id];
          }
          if (nextSecretState && Object.keys(nextSecretState).length > 0) nextSecrets[nextSecretId] = nextSecretState;
          else delete nextSecrets[nextSecretId];
          return nextSecrets;
        });
      }
      if (nextProfile.id !== currentProfile.id) {
        setActiveProfileId(nextProfile.id);
      }
    },
    [activeProfile, activeProfileId, defaults, setActiveProfileId, setConnectionProfileSecrets, setConnectionProfiles],
  );

  const addProfile = React.useCallback(
    (mode?: ConnectionMode) => {
      const id = generateProfileId();
      const seed: Partial<ConnectionProfile> = {
        id,
        mode: mode || defaults.connectionMode,
        base: defaults.daemonBaseUrl,
        brokerBase: defaults.brokerBaseUrl,
        brokerAgentId: defaults.brokerAgentId,
        brokerDeploymentId: defaults.brokerDeploymentId,
        brokerCookieAuth: defaults.brokerCookieAuth,
        brokerAuthToken: defaults.brokerAuthToken,
        daemonAuthToken: defaults.daemonAuthToken,
      };
      const profile = normalizeProfile(seed, defaults);
      const secret = extractProfileSecrets(profile);
      setConnectionProfiles((prev) => [...prev, sanitizeProfileForPersistence(profile)]);
      if (Object.keys(secret).length > 0) {
        setConnectionProfileSecrets((prev) => ({ ...normalizeSecretMap(prev), [id]: secret }));
      }
      setActiveProfileId(id);
    },
    [defaults, setActiveProfileId, setConnectionProfileSecrets, setConnectionProfiles],
  );

  const duplicateProfile = React.useCallback(() => {
    const id = generateProfileId();
    const copy: ConnectionProfile = normalizeProfile(
      {
        ...activeProfile,
        id,
        name: `${activeProfile.name} copy`,
      },
      defaults,
    );
    const secret = extractProfileSecrets(copy);
    setConnectionProfiles((prev) => [...prev, sanitizeProfileForPersistence(copy)]);
    if (Object.keys(secret).length > 0) {
      setConnectionProfileSecrets((prev) => ({ ...normalizeSecretMap(prev), [id]: secret }));
    }
    setActiveProfileId(id);
  }, [activeProfile, defaults, setActiveProfileId, setConnectionProfileSecrets, setConnectionProfiles]);

  const deleteProfile = React.useCallback(
    (id: string) => {
      let nextActive = "";
      setConnectionProfiles((prev) => {
        if (prev.length <= 1) return prev;
        const next = prev.filter((profile) => profile.id !== id);
        if (next.length === 0) return prev;
        if (id === activeProfileId) nextActive = next[0].id;
        return next;
      });
      setConnectionProfileSecrets((prev) => {
        const next = { ...normalizeSecretMap(prev) };
        delete next[id];
        return next;
      });
      if (nextActive) setActiveProfileId(nextActive);
    },
    [activeProfileId, setActiveProfileId, setConnectionProfileSecrets, setConnectionProfiles],
  );

  const connectionMode = activeProfile.mode;
  const base = activeProfile.base;
  const brokerBase = activeProfile.brokerBase;
  const brokerAgentId = activeProfile.brokerAgentId;
  const brokerDeploymentId = activeProfile.brokerDeploymentId;
  const brokerCookieAuth = activeProfile.brokerCookieAuth;
  const brokerAuthToken = activeProfile.brokerAuthToken;
  const daemonAuthToken = activeProfile.daemonAuthToken;

  const setMode = React.useCallback<React.Dispatch<React.SetStateAction<ConnectionMode>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.mode) : next;
        if (value === prev.mode) return prev;
        return { ...prev, mode: value };
      });
    },
    [updateActiveProfile],
  );

  const setBase = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.base) : next;
        if (value === prev.base) return prev;
        return { ...prev, base: value };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerBase = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerBase) : next;
        if (value === prev.brokerBase) return prev;
        return { ...prev, brokerBase: value };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerAgentId = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerAgentId) : next;
        if (value === prev.brokerAgentId) return prev;
        return { ...prev, brokerAgentId: value };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerDeploymentId = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerDeploymentId) : next;
        if (value === prev.brokerDeploymentId) return prev;
        return { ...prev, brokerDeploymentId: value };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerCookieAuth = React.useCallback<React.Dispatch<React.SetStateAction<boolean>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerCookieAuth) : next;
        if (value === prev.brokerCookieAuth) return prev;
        return { ...prev, brokerCookieAuth: value };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerAuthToken = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.brokerAuthToken) : next;
        if (value === prev.brokerAuthToken) return prev;
        return { ...prev, brokerAuthToken: value };
      });
    },
    [updateActiveProfile],
  );

  const setDaemonAuthToken = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.daemonAuthToken) : next;
        if (value === prev.daemonAuthToken) return prev;
        return { ...prev, daemonAuthToken: value };
      });
    },
    [updateActiveProfile],
  );

  const setProfileName = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const value = typeof next === "function" ? next(prev.name) : next;
        const trimmed = String(value || "").trim();
        const nextName = trimmed || profileNameDefault(prev);
        if (nextName === prev.name) return prev;
        return { ...prev, name: nextName };
      });
    },
    [updateActiveProfile],
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
    const profileId = String(activeProfileId || "default");
    return mode === "broker"
      ? `broker:pid=${profileId}:dep=${deploymentId}:${cookie}:tlen=${token.length}:alen=${agentdToken.length}`
      : `direct:pid=${profileId}:tlen=${token.length}`;
  }, [activeProfileId, daemonAuth]);

  const serverPrefsBase = React.useMemo(() => {
    if (connectionMode === "broker") {
      return normalizeHttpBase(brokerBase, "https://127.0.0.1:8443", "https");
    }
    return normalizeHttpBase(base, "http://127.0.0.1:8123", "http");
  }, [base, brokerBase, connectionMode]);

  React.useEffect(() => {
    if (connectionMode !== "broker" || !brokerCookieAuth) {
      brokerCookieExchangeKeyRef.current = "";
      setBrokerCookieSessionStatus("idle");
      setBrokerCookieSessionError(null);
      return;
    }
    const baseTrimmed = String(serverPrefsBase || "").trim();
    const token = String(brokerAuthToken || "").trim();
    if (!baseTrimmed) {
      brokerCookieExchangeKeyRef.current = "";
      setBrokerCookieSessionStatus("idle");
      setBrokerCookieSessionError(null);
      return;
    }
    if (!token) {
      brokerCookieExchangeKeyRef.current = "";
      setBrokerCookieSessionError(null);
      setBrokerCookieSessionStatus((prev) => (prev === "ready" ? prev : "idle"));
      return;
    }
    const exchangeKey = `${activeProfileId}:${token}`;
    if (brokerCookieExchangeKeyRef.current === exchangeKey) {
      return;
    }
    brokerCookieExchangeKeyRef.current = exchangeKey;
    setBrokerCookieSessionStatus("exchanging");
    setBrokerCookieSessionError(null);
    (async () => {
      try {
        await apiBrokerCreateAuthSession(baseTrimmed, { mode: "broker", token });
        updateActiveProfile((prev) => {
          if (prev.mode !== "broker" || !prev.brokerCookieAuth) return prev;
          if (String(prev.brokerAuthToken || "").trim() !== token) return prev;
          return { ...prev, brokerAuthToken: "" };
        });
        if (typeof window !== "undefined") {
          try {
            window.sessionStorage.removeItem("agentui.brokerAuthToken");
          } catch {
            // ignore storage failures
          }
        }
        if (brokerCookieExchangeKeyRef.current === exchangeKey) {
          brokerCookieExchangeKeyRef.current = "";
        }
        setBrokerCookieSessionStatus("ready");
        setBrokerCookieSessionError(null);
      } catch (err) {
        if (brokerCookieExchangeKeyRef.current === exchangeKey) {
          brokerCookieExchangeKeyRef.current = "";
        }
        setBrokerCookieSessionStatus("error");
        setBrokerCookieSessionError(err instanceof Error ? err.message : String(err));
      }
    })();
  }, [activeProfileId, brokerAuthToken, brokerCookieAuth, connectionMode, serverPrefsBase, updateActiveProfile]);

  const clearBrokerAuthCookie = React.useCallback(async () => {
    const baseTrimmed = String(serverPrefsBase || "").trim();
    if (connectionMode !== "broker" || !brokerCookieAuth || !baseTrimmed) {
      return;
    }
    setBrokerCookieSessionError(null);
    await apiBrokerDeleteAuthSession(baseTrimmed, { mode: "broker", useCookieAuth: true });
    setBrokerCookieSessionStatus("idle");
  }, [brokerCookieAuth, connectionMode, serverPrefsBase]);

  const serverPrefsAuthReady =
    connectionMode !== "broker" || brokerCookieAuth || String(brokerAuthToken || "").trim().length > 0;
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
    if (connectionMode === "broker" && !serverPrefsAuthReady) {
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
          if (!response.ok) {
            throw new Error(`caps ${response.status}`);
          }
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
        if (!response.ok) {
          throw new Error(`caps ${response.status}`);
        }
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
  }, [connectionMode, daemonAuth, serverPrefsAuthReady, serverPrefsAuto, serverPrefsBase]);

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
    connection: {
      profiles: connectionProfiles,
      activeProfileId,
      setActiveProfileId,
      profileName: activeProfile.name,
      setProfileName,
      addProfile,
      duplicateProfile,
      deleteProfile,
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
      brokerCookieSessionStatus,
      brokerCookieSessionError,
      clearBrokerAuthCookie,
      daemonAuthToken,
      setDaemonAuthToken,
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
      serverPrefsBase,
      pullServerPrefs,
      pushServerPrefs,
      effectiveBase,
      effectiveSseBase,
      daemonAuth,
      authKey,
    },
    activeProfile,
    updateActiveProfile,
  };
}
