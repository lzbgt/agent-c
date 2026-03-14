import React from "react";

import useLocalStorageState from "./useLocalStorageState";
import useSessionStorageState from "./useSessionStorageState";
import type { AgentUIDefaults, ConnectionMode } from "../runtime_config";
import {
  CONNECTION_PROFILE_SECRETS_KEY,
  LEGACY_SECRET_KEYS,
  extractProfileSecrets,
  generateProfileId,
  mergeProfileSecrets,
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
} from "./uiSettingsProfiles";

type UseUiConnectionProfileStateParams = {
  defaults: AgentUIDefaults;
};

export type ConnectionProfileUpdater = (update: (prev: ConnectionProfile) => ConnectionProfile) => void;

export type UseUiConnectionProfileStateResult = {
  connectionProfiles: ConnectionProfile[];
  setConnectionProfiles: React.Dispatch<React.SetStateAction<ConnectionProfile[]>>;
  connectionProfileSecrets: ConnectionProfileSecretMap;
  activeProfileId: string;
  setActiveProfileId: React.Dispatch<React.SetStateAction<string>>;
  activeProfile: ConnectionProfile;
  updateActiveProfile: ConnectionProfileUpdater;
  addProfile: (mode?: ConnectionMode) => void;
  duplicateProfile: () => void;
  deleteProfile: (id: string) => void;
  setProfileName: React.Dispatch<React.SetStateAction<string>>;
};

export default function useUiConnectionProfileState({
  defaults,
}: UseUiConnectionProfileStateParams): UseUiConnectionProfileStateResult {
  const [connectionProfiles, setConnectionProfiles] = useLocalStorageState<ConnectionProfile[]>(
    "agentui.connectionProfiles",
    [],
  );
  const [connectionProfileSecrets, setConnectionProfileSecrets] = useSessionStorageState<ConnectionProfileSecretMap>(
    CONNECTION_PROFILE_SECRETS_KEY,
    {},
  );
  const [activeProfileId, setActiveProfileId] = useLocalStorageState("agentui.connectionProfileActive", "");

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

  return {
    connectionProfiles,
    setConnectionProfiles,
    connectionProfileSecrets,
    activeProfileId,
    setActiveProfileId,
    activeProfile,
    updateActiveProfile,
    addProfile,
    duplicateProfile,
    deleteProfile,
    setProfileName,
  };
}
