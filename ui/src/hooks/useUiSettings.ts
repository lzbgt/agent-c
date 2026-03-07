import React from "react";
import useAutoplayUnlock from "./useAutoplayUnlock";
import useLocalStorageState from "./useLocalStorageState";
import useSessionStorageState from "./useSessionStorageState";
import {
  getUiDefaults,
  type AgentUIDefaults,
  type ConnectionMode,
  type HostPolicy,
  type ServerPrefsMode,
  type ToolMode,
} from "../runtime_config";
import {
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiGetClientPrefs,
  apiPostClientPrefs,
  daemonFetchInit,
  daemonHeaders,
  type ApiAuth,
  type ClientPrefs,
} from "../api";
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
  safeParse,
  sanitizeProfileForPersistence,
  secretsEqual,
  stripSecretRunOverrides,
  type ConnectionProfile,
  type ConnectionProfileSecretMap,
  type ConnectionProfileSecretState,
  type RunProfileOverrides,
  type ServerPrefs,
} from "./uiSettingsProfiles";

export type ConnectionSettings = {
  profiles: ConnectionProfile[];
  activeProfileId: string;
  setActiveProfileId: (next: string) => void;
  profileName: string;
  setProfileName: React.Dispatch<React.SetStateAction<string>>;
  addProfile: (mode?: ConnectionMode) => void;
  duplicateProfile: () => void;
  deleteProfile: (id: string) => void;
  mode: ConnectionMode;
  setMode: React.Dispatch<React.SetStateAction<ConnectionMode>>;
  base: string;
  setBase: React.Dispatch<React.SetStateAction<string>>;
  brokerBase: string;
  setBrokerBase: React.Dispatch<React.SetStateAction<string>>;
  brokerAgentId: string;
  setBrokerAgentId: React.Dispatch<React.SetStateAction<string>>;
  brokerDeploymentId: string;
  setBrokerDeploymentId: React.Dispatch<React.SetStateAction<string>>;
  brokerCookieAuth: boolean;
  setBrokerCookieAuth: React.Dispatch<React.SetStateAction<boolean>>;
  brokerAuthToken: string;
  setBrokerAuthToken: React.Dispatch<React.SetStateAction<string>>;
  daemonAuthToken: string;
  setDaemonAuthToken: React.Dispatch<React.SetStateAction<string>>;
  serverPrefsEnabled: boolean;
  serverPrefsAuto: boolean;
  serverPrefsUserSet: boolean;
  serverPrefsAutoStatus: ServerPrefsAutoStatus;
  serverPrefsAutoError: string | null;
  clearServerPrefsOverride: () => void;
  setServerPrefsEnabled: React.Dispatch<React.SetStateAction<boolean>>;
  serverPrefsStatus: "idle" | "loading" | "error" | "synced";
  serverPrefsError: string | null;
  serverPrefsLastSyncMs: number | null;
  serverPrefsBase: string;
  pullServerPrefs: () => void;
  pushServerPrefs: () => void;
  effectiveBase: string;
  effectiveSseBase: string;
  daemonAuth: ApiAuth;
  authKey: string;
};

type ServerPrefsAutoStatus = "idle" | "checking" | "ready" | "unsupported" | "auth_required" | "error";

export type RunSettings = {
  profileOverridesEnabled: boolean;
  setProfileOverridesEnabled: React.Dispatch<React.SetStateAction<boolean>>;
  copyProfileOverridesFromGlobal: () => void;
  clearProfileOverrides: () => void;
  tools: ToolMode;
  setTools: React.Dispatch<React.SetStateAction<ToolMode>>;
  yolo: boolean;
  setYolo: React.Dispatch<React.SetStateAction<boolean>>;
  hostPolicy: HostPolicy;
  setHostPolicy: React.Dispatch<React.SetStateAction<HostPolicy>>;
  automationProfile: string;
  setAutomationProfile: React.Dispatch<React.SetStateAction<string>>;
  verbose: boolean;
  setVerbose: React.Dispatch<React.SetStateAction<boolean>>;
  model: string;
  setModel: React.Dispatch<React.SetStateAction<string>>;
  summaryModel: string;
  setSummaryModel: React.Dispatch<React.SetStateAction<string>>;
  summaryMaxChars: string;
  setSummaryMaxChars: React.Dispatch<React.SetStateAction<string>>;
  baseUrl: string;
  setBaseUrl: React.Dispatch<React.SetStateAction<string>>;
  apiKey: string;
  setApiKey: React.Dispatch<React.SetStateAction<string>>;
  proxyUrl: string;
  setProxyUrl: React.Dispatch<React.SetStateAction<string>>;
  timeoutMs: string;
  setTimeoutMs: React.Dispatch<React.SetStateAction<string>>;
  maxCaptureBytes: string;
  setMaxCaptureBytes: React.Dispatch<React.SetStateAction<string>>;
  streamAssistant: boolean;
  setStreamAssistant: React.Dispatch<React.SetStateAction<boolean>>;
  trace: boolean;
  setTrace: React.Dispatch<React.SetStateAction<boolean>>;
  useAsync: boolean;
  setUseAsync: React.Dispatch<React.SetStateAction<boolean>>;
  maxSteps: string;
  setMaxSteps: React.Dispatch<React.SetStateAction<string>>;
  maxRepeatedToolCalls: string;
  setMaxRepeatedToolCalls: React.Dispatch<React.SetStateAction<string>>;
  maxToolCallsTotal: string;
  setMaxToolCallsTotal: React.Dispatch<React.SetStateAction<string>>;
  maxToolCallsPerTool: string;
  setMaxToolCallsPerTool: React.Dispatch<React.SetStateAction<string>>;
  toolCallLimits: string;
  setToolCallLimits: React.Dispatch<React.SetStateAction<string>>;
  maxChars: string;
  setMaxChars: React.Dispatch<React.SetStateAction<string>>;
  keepLast: string;
  setKeepLast: React.Dispatch<React.SetStateAction<string>>;
  memoryContextMode: string;
  setMemoryContextMode: React.Dispatch<React.SetStateAction<string>>;
  memoryIncludeStructured: boolean;
  setMemoryIncludeStructured: React.Dispatch<React.SetStateAction<boolean>>;
  memoryIncludeCore: boolean;
  setMemoryIncludeCore: React.Dispatch<React.SetStateAction<boolean>>;
  memoryIncludeDaily: boolean;
  setMemoryIncludeDaily: React.Dispatch<React.SetStateAction<boolean>>;
  memoryIncludeSession: boolean;
  setMemoryIncludeSession: React.Dispatch<React.SetStateAction<boolean>>;
  memoryDailyDays: string;
  setMemoryDailyDays: React.Dispatch<React.SetStateAction<string>>;
  memoryTotalCap: string;
  setMemoryTotalCap: React.Dispatch<React.SetStateAction<string>>;
  memorySearchQuery: string;
  setMemorySearchQuery: React.Dispatch<React.SetStateAction<string>>;
  memorySearchOrder: string;
  setMemorySearchOrder: React.Dispatch<React.SetStateAction<string>>;
  memorySearchUseIndex: boolean;
  setMemorySearchUseIndex: React.Dispatch<React.SetStateAction<boolean>>;
  memorySearchCaseSensitive: boolean;
  setMemorySearchCaseSensitive: React.Dispatch<React.SetStateAction<boolean>>;
  memorySearchFallbackToFiles: boolean;
  setMemorySearchFallbackToFiles: React.Dispatch<React.SetStateAction<boolean>>;
  memorySearchMaxResults: string;
  setMemorySearchMaxResults: React.Dispatch<React.SetStateAction<string>>;
  memorySearchMaxSnippetChars: string;
  setMemorySearchMaxSnippetChars: React.Dispatch<React.SetStateAction<string>>;
  memorySearchContextLines: string;
  setMemorySearchContextLines: React.Dispatch<React.SetStateAction<string>>;
  orMinTotal: string;
  setOrMinTotal: React.Dispatch<React.SetStateAction<string>>;
  orMaxTotal: string;
  setOrMaxTotal: React.Dispatch<React.SetStateAction<string>>;
  orRequireMultimodal: boolean;
  setOrRequireMultimodal: React.Dispatch<React.SetStateAction<boolean>>;
  orRequireTools: boolean;
  setOrRequireTools: React.Dispatch<React.SetStateAction<boolean>>;
  orLimit: string;
  setOrLimit: React.Dispatch<React.SetStateAction<string>>;
};

export type ClientSettings = {
  clientId: string;
  setClientId: React.Dispatch<React.SetStateAction<string>>;
  allowAutoplay: boolean;
  setAllowAutoplay: React.Dispatch<React.SetStateAction<boolean>>;
  allowClientRpcs: boolean;
  setAllowClientRpcs: React.Dispatch<React.SetStateAction<boolean>>;
  allowClientEffects: boolean;
  setAllowClientEffects: React.Dispatch<React.SetStateAction<boolean>>;
  allowUnsafePageEval: boolean;
  setAllowUnsafePageEval: React.Dispatch<React.SetStateAction<boolean>>;
  showDebugInConversation: boolean;
  setShowDebugInConversation: React.Dispatch<React.SetStateAction<boolean>>;
};

export type UiSettings = {
  defaults: AgentUIDefaults;
  showSettings: boolean;
  setShowSettings: React.Dispatch<React.SetStateAction<boolean>>;
  connection: ConnectionSettings;
  run: RunSettings;
  client: ClientSettings;
  brokerPanelOpen: boolean;
  setBrokerPanelOpen: React.Dispatch<React.SetStateAction<boolean>>;
};

export default function useUiSettings(): UiSettings {
  const defaults = React.useMemo(() => getUiDefaults(), []);
  const initialClientId = React.useMemo(() => {
    const preset = String(defaults.clientId || "").trim();
    if (preset) return preset;
    try {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const g: any = typeof globalThis !== "undefined" ? globalThis : {};
      if (g.crypto && typeof g.crypto.randomUUID === "function") {
        return `webui-${g.crypto.randomUUID()}`;
      }
    } catch {
      // ignore
    }
    return `webui-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }, [defaults.clientId]);
  const [clientId, setClientId] = useLocalStorageState("agentui.clientId", initialClientId);

  const [showSettings, setShowSettings] = useLocalStorageState("agentui.showSettings", false);

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
  const [serverPrefsAutoStatus, setServerPrefsAutoStatus] = React.useState<ServerPrefsAutoStatus>("idle");
  const [serverPrefsAutoError, setServerPrefsAutoError] = React.useState<string | null>(null);
  const [serverPrefsStatus, setServerPrefsStatus] = React.useState<"idle" | "loading" | "error" | "synced">("idle");
  const [serverPrefsError, setServerPrefsError] = React.useState<string | null>(null);
  const [serverPrefsLastSyncMs, setServerPrefsLastSyncMs] = React.useState<number | null>(null);
  const serverPrefsLastPayloadRef = React.useRef<string>("");
  const serverPrefsPullInFlightRef = React.useRef(false);
  const initialApiKey = React.useMemo(() => readLegacySecretString("agentui.apiKey", defaults.apiKey), [defaults.apiKey]);

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
    const normalized = connectionProfiles.map((p) => {
      const np = normalizeProfile(p, defaults);
      const extracted = extractProfileSecrets(p);
      if (Object.keys(extracted).length > 0) {
        const mergedSecret = { ...(nextSecrets[np.id] || {}), ...extracted };
        if (!secretsEqual(nextSecrets[np.id], mergedSecret)) {
          nextSecrets[np.id] = mergedSecret;
          secretsChanged = true;
        }
      }
      const sp = sanitizeProfileForPersistence(np);
      const prevOverridesJson = JSON.stringify(stripSecretRunOverrides((p as ConnectionProfile).runOverrides) ?? null);
      const nextOverridesJson = JSON.stringify(sp.runOverrides ?? null);
      const prevOverridesEnabled = typeof p.runOverridesEnabled === "boolean" ? p.runOverridesEnabled : false;
      if (
        sp.id !== p.id ||
        sp.name !== p.name ||
        sp.mode !== p.mode ||
        sp.base !== p.base ||
        sp.brokerBase !== p.brokerBase ||
        sp.brokerAgentId !== p.brokerAgentId ||
        sp.brokerDeploymentId !== p.brokerDeploymentId ||
        sp.brokerCookieAuth !== (typeof p.brokerCookieAuth === "boolean" ? p.brokerCookieAuth : defaults.brokerCookieAuth) ||
        sp.brokerAuthToken !== (p.brokerAuthToken || "") ||
        sp.daemonAuthToken !== (p.daemonAuthToken || "") ||
        sp.runOverridesEnabled !== prevOverridesEnabled ||
        prevOverridesJson !== nextOverridesJson
      ) {
        changed = true;
      }
      return sp;
    });
    if (changed) setConnectionProfiles(normalized);
    if (secretsChanged) setConnectionProfileSecrets(nextSecrets);
    if (!normalized.some((p) => p.id === activeProfileId)) {
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

  const [toolsGlobal, setToolsGlobal] = useLocalStorageState<ToolMode>("agentui.tools", defaults.tools);
  const [yoloGlobal, setYoloGlobal] = useLocalStorageState("agentui.yolo", defaults.yolo);
  const [hostPolicyGlobal, setHostPolicyGlobal] = useLocalStorageState<HostPolicy>(
    "agentui.hostPolicy",
    defaults.hostPolicy,
  );
  const [automationProfileGlobal, setAutomationProfileGlobal] = useLocalStorageState(
    "agentui.automationProfile",
    defaults.automationProfile,
  );
  const [verboseGlobal, setVerboseGlobal] = useLocalStorageState("agentui.verbose", defaults.verbose);
  const [modelGlobal, setModelGlobal] = useLocalStorageState("agentui.model", defaults.model);
  const [summaryModelGlobal, setSummaryModelGlobal] = useLocalStorageState("agentui.summaryModel", "");
  const [summaryMaxCharsGlobal, setSummaryMaxCharsGlobal] = useLocalStorageState("agentui.summaryMaxChars", "1200");
  const [baseUrlGlobal, setBaseUrlGlobal] = useLocalStorageState("agentui.baseUrl", defaults.baseUrl);
  const [apiKeyGlobal, setApiKeyGlobal] = useSessionStorageState("agentui.apiKey", initialApiKey);
  const [proxyUrlGlobal, setProxyUrlGlobal] = useLocalStorageState("agentui.proxyUrl", defaults.proxyUrl);
  const [timeoutMsGlobal, setTimeoutMsGlobal] = useLocalStorageState("agentui.timeoutMs", defaults.timeoutMs);
  const [maxCaptureBytesGlobal, setMaxCaptureBytesGlobal] = useLocalStorageState("agentui.maxCaptureBytes", "65536");
  const [streamAssistantGlobal, setStreamAssistantGlobal] = useLocalStorageState(
    "agentui.streamAssistant",
    defaults.streamAssistant,
  );
  const [traceGlobal, setTraceGlobal] = useLocalStorageState("agentui.trace", defaults.trace);
  const [useAsyncGlobal, setUseAsyncGlobal] = useLocalStorageState("agentui.useAsync", defaults.useAsync);

  const [maxStepsRawGlobal, setMaxStepsRawGlobal] = useLocalStorageState("agentui.maxSteps", "");
  const [maxStepsUserSet, setMaxStepsUserSet] = useLocalStorageState("agentui.maxStepsUserSet", false);
  const [maxRepeatedToolCallsGlobal, setMaxRepeatedToolCallsGlobal] = useLocalStorageState(
    "agentui.maxRepeatedToolCalls",
    "0",
  );
  const [maxToolCallsTotalGlobal, setMaxToolCallsTotalGlobal] = useLocalStorageState("agentui.maxToolCallsTotal", "");
  const [maxToolCallsPerToolGlobal, setMaxToolCallsPerToolGlobal] = useLocalStorageState(
    "agentui.maxToolCallsPerTool",
    "",
  );
  const [toolCallLimitsGlobal, setToolCallLimitsGlobal] = useLocalStorageState("agentui.toolCallLimits", "");
  const [maxCharsGlobal, setMaxCharsGlobal] = useLocalStorageState("agentui.maxChars", "20000");
  const [keepLastGlobal, setKeepLastGlobal] = useLocalStorageState("agentui.keepLast", "16");
  const [memoryContextModeGlobal, setMemoryContextModeGlobal] = useLocalStorageState(
    "agentui.memoryContextMode",
    defaults.memoryContextMode,
  );
  const [memoryIncludeStructuredGlobal, setMemoryIncludeStructuredGlobal] = useLocalStorageState(
    "agentui.memoryIncludeStructured",
    defaults.memoryIncludeStructured,
  );
  const [memoryIncludeCoreGlobal, setMemoryIncludeCoreGlobal] = useLocalStorageState(
    "agentui.memoryIncludeCore",
    defaults.memoryIncludeCore,
  );
  const [memoryIncludeDailyGlobal, setMemoryIncludeDailyGlobal] = useLocalStorageState(
    "agentui.memoryIncludeDaily",
    defaults.memoryIncludeDaily,
  );
  const [memoryIncludeSessionGlobal, setMemoryIncludeSessionGlobal] = useLocalStorageState(
    "agentui.memoryIncludeSession",
    defaults.memoryIncludeSession,
  );
  const [memoryDailyDaysGlobal, setMemoryDailyDaysGlobal] = useLocalStorageState(
    "agentui.memoryDailyDays",
    defaults.memoryDailyDays,
  );
  const [memoryTotalCapGlobal, setMemoryTotalCapGlobal] = useLocalStorageState(
    "agentui.memoryTotalCap",
    defaults.memoryTotalCap,
  );
  const [memorySearchQueryGlobal, setMemorySearchQueryGlobal] = useLocalStorageState(
    "agentui.memorySearchQuery",
    defaults.memorySearchQuery,
  );
  const [memorySearchOrderGlobal, setMemorySearchOrderGlobal] = useLocalStorageState(
    "agentui.memorySearchOrder",
    defaults.memorySearchOrder,
  );
  const [memorySearchUseIndexGlobal, setMemorySearchUseIndexGlobal] = useLocalStorageState(
    "agentui.memorySearchUseIndex",
    defaults.memorySearchUseIndex,
  );
  const [memorySearchCaseSensitiveGlobal, setMemorySearchCaseSensitiveGlobal] = useLocalStorageState(
    "agentui.memorySearchCaseSensitive",
    defaults.memorySearchCaseSensitive,
  );
  const [memorySearchFallbackToFilesGlobal, setMemorySearchFallbackToFilesGlobal] = useLocalStorageState(
    "agentui.memorySearchFallbackToFiles",
    defaults.memorySearchFallbackToFiles,
  );
  const [memorySearchMaxResultsGlobal, setMemorySearchMaxResultsGlobal] = useLocalStorageState(
    "agentui.memorySearchMaxResults",
    defaults.memorySearchMaxResults,
  );
  const [memorySearchMaxSnippetCharsGlobal, setMemorySearchMaxSnippetCharsGlobal] = useLocalStorageState(
    "agentui.memorySearchMaxSnippetChars",
    defaults.memorySearchMaxSnippetChars,
  );
  const [memorySearchContextLinesGlobal, setMemorySearchContextLinesGlobal] = useLocalStorageState(
    "agentui.memorySearchContextLines",
    defaults.memorySearchContextLines,
  );

  const [orMinTotal, setOrMinTotal] = useLocalStorageState("agentui.orMinTotal", "0.01");
  const [orMaxTotal, setOrMaxTotal] = useLocalStorageState("agentui.orMaxTotal", "0.50");
  const [orRequireMultimodal, setOrRequireMultimodal] = useLocalStorageState("agentui.orRequireMultimodal", true);
  const [orRequireTools, setOrRequireTools] = useLocalStorageState("agentui.orRequireTools", true);
  const [orLimit, setOrLimit] = useLocalStorageState("agentui.orLimit", "50");

  const [allowAutoplay, setAllowAutoplay] = useLocalStorageState("agentui.allowAutoplay", true);
  useAutoplayUnlock(allowAutoplay);
  const [allowClientRpcs, setAllowClientRpcs] = useLocalStorageState("agentui.allowClientRpcs", defaults.allowClientRpcs);
  const [allowClientEffects, setAllowClientEffects] = useLocalStorageState(
    "agentui.allowClientEffects",
    defaults.allowClientEffects,
  );
  const [allowUnsafePageEval, setAllowUnsafePageEval] = useLocalStorageState(
    "agentui.allowUnsafePageEval",
    defaults.allowUnsafePageEval,
  );
  const [showDebugInConversation, setShowDebugInConversation] = useLocalStorageState(
    "agentui.showDebugInConversation",
    true,
  );
  const [brokerPanelOpen, setBrokerPanelOpen] = useLocalStorageState(
    "agentui.brokerPanelOpen",
    defaults.brokerPanelOpen,
  );

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
    const found = connectionProfiles.find((p) => p.id === activeProfileId);
    const baseProfile = found || connectionProfiles[0];
    return mergeProfileSecrets(baseProfile, connectionProfileSecrets[baseProfile.id]);
  }, [activeProfileId, connectionProfileSecrets, connectionProfiles, fallbackProfile]);

  const updateActiveProfile = React.useCallback(
    (update: (prev: ConnectionProfile) => ConnectionProfile) => {
      let nextSecretId = "";
      let nextSecretState: ConnectionProfileSecretState | null = null;
      setConnectionProfiles((prev) => {
        if (prev.length === 0) return prev;
        const idx = prev.findIndex((p) => p.id === activeProfileId);
        const useIdx = idx >= 0 ? idx : 0;
        const curPersisted = normalizeProfile(prev[useIdx], defaults);
        const cur = mergeProfileSecrets(curPersisted, connectionProfileSecrets[curPersisted.id]);
        const next = normalizeProfile(update(cur), defaults);
        nextSecretId = next.id;
        nextSecretState = extractProfileSecrets(next);
        const sanitized = sanitizeProfileForPersistence(next);
        if (JSON.stringify(sanitized) === JSON.stringify(prev[useIdx])) return prev;
        const out = prev.slice();
        out[useIdx] = sanitized;
        return out;
      });
      if (nextSecretId) {
        setConnectionProfileSecrets((prev) => {
          const next = { ...normalizeSecretMap(prev) };
          if (nextSecretState && Object.keys(nextSecretState).length > 0) next[nextSecretId] = nextSecretState;
          else delete next[nextSecretId];
          return next;
        });
      }
    },
    [activeProfileId, connectionProfileSecrets, defaults, setConnectionProfileSecrets, setConnectionProfiles],
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
        const next = prev.filter((p) => p.id !== id);
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

  const runOverridesEnabled = !!activeProfile.runOverridesEnabled;
  const runOverrides = activeProfile.runOverrides || {};

  const snapshotRunOverrides = React.useCallback(
    (): RunProfileOverrides => ({
      tools: toolsGlobal,
      yolo: yoloGlobal,
      hostPolicy: hostPolicyGlobal,
      automationProfile: automationProfileGlobal,
      verbose: verboseGlobal,
      model: modelGlobal,
      summaryModel: summaryModelGlobal,
      summaryMaxChars: summaryMaxCharsGlobal,
      baseUrl: baseUrlGlobal,
      apiKey: apiKeyGlobal,
      proxyUrl: proxyUrlGlobal,
      timeoutMs: timeoutMsGlobal,
      maxCaptureBytes: maxCaptureBytesGlobal,
      streamAssistant: streamAssistantGlobal,
      trace: traceGlobal,
      useAsync: useAsyncGlobal,
      maxSteps: maxStepsRawGlobal,
      maxRepeatedToolCalls: maxRepeatedToolCallsGlobal,
      maxToolCallsTotal: maxToolCallsTotalGlobal,
      maxToolCallsPerTool: maxToolCallsPerToolGlobal,
      toolCallLimits: toolCallLimitsGlobal,
      maxChars: maxCharsGlobal,
      keepLast: keepLastGlobal,
      memoryContextMode: memoryContextModeGlobal,
      memoryIncludeStructured: memoryIncludeStructuredGlobal,
      memoryIncludeCore: memoryIncludeCoreGlobal,
      memoryIncludeDaily: memoryIncludeDailyGlobal,
      memoryIncludeSession: memoryIncludeSessionGlobal,
      memoryDailyDays: memoryDailyDaysGlobal,
      memoryTotalCap: memoryTotalCapGlobal,
      memorySearchQuery: memorySearchQueryGlobal,
      memorySearchOrder: memorySearchOrderGlobal,
      memorySearchUseIndex: memorySearchUseIndexGlobal,
      memorySearchCaseSensitive: memorySearchCaseSensitiveGlobal,
      memorySearchFallbackToFiles: memorySearchFallbackToFilesGlobal,
      memorySearchMaxResults: memorySearchMaxResultsGlobal,
      memorySearchMaxSnippetChars: memorySearchMaxSnippetCharsGlobal,
      memorySearchContextLines: memorySearchContextLinesGlobal,
    }),
    [
      apiKeyGlobal,
      automationProfileGlobal,
      baseUrlGlobal,
      hostPolicyGlobal,
      keepLastGlobal,
      maxCaptureBytesGlobal,
      maxCharsGlobal,
      maxRepeatedToolCallsGlobal,
      maxStepsRawGlobal,
      maxToolCallsPerToolGlobal,
      maxToolCallsTotalGlobal,
      memoryContextModeGlobal,
      memoryDailyDaysGlobal,
      memoryIncludeCoreGlobal,
      memoryIncludeDailyGlobal,
      memoryIncludeSessionGlobal,
      memoryIncludeStructuredGlobal,
      memorySearchCaseSensitiveGlobal,
      memorySearchContextLinesGlobal,
      memorySearchFallbackToFilesGlobal,
      memorySearchMaxResultsGlobal,
      memorySearchMaxSnippetCharsGlobal,
      memorySearchOrderGlobal,
      memorySearchQueryGlobal,
      memorySearchUseIndexGlobal,
      memoryTotalCapGlobal,
      modelGlobal,
      proxyUrlGlobal,
      streamAssistantGlobal,
      summaryMaxCharsGlobal,
      summaryModelGlobal,
      timeoutMsGlobal,
      toolCallLimitsGlobal,
      toolsGlobal,
      traceGlobal,
      useAsyncGlobal,
      verboseGlobal,
      yoloGlobal,
    ],
  );

  const setProfileOverridesEnabled = React.useCallback<React.Dispatch<React.SetStateAction<boolean>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const current = !!prev.runOverridesEnabled;
        const enabled = typeof next === "function" ? next(current) : next;
        if (enabled === current) return prev;
        if (!enabled) {
          return { ...prev, runOverridesEnabled: false };
        }
        const seed =
          prev.runOverrides && Object.keys(prev.runOverrides).length > 0 ? prev.runOverrides : snapshotRunOverrides();
        return { ...prev, runOverridesEnabled: true, runOverrides: seed };
      });
    },
    [snapshotRunOverrides, updateActiveProfile],
  );

  const copyProfileOverridesFromGlobal = React.useCallback(() => {
    updateActiveProfile((prev) => ({
      ...prev,
      runOverridesEnabled: true,
      runOverrides: snapshotRunOverrides(),
    }));
  }, [snapshotRunOverrides, updateActiveProfile]);

  const clearProfileOverrides = React.useCallback(() => {
    updateActiveProfile((prev) => {
      if (!prev.runOverridesEnabled && !prev.runOverrides) return prev;
      return { ...prev, runOverridesEnabled: false, runOverrides: undefined };
    });
  }, [updateActiveProfile]);

  const resolveRunValue = React.useCallback(
    <T,>(key: keyof RunProfileOverrides, globalValue: T): T => {
      if (!runOverridesEnabled) return globalValue;
      if (Object.prototype.hasOwnProperty.call(runOverrides, key)) {
        return (runOverrides as Record<string, T>)[key as string];
      }
      return globalValue;
    },
    [runOverrides, runOverridesEnabled],
  );

  const setRunValue = React.useCallback(
    <T,>(
      key: keyof RunProfileOverrides,
      next: React.SetStateAction<T>,
      globalValue: T,
      setGlobal: React.Dispatch<React.SetStateAction<T>>,
    ) => {
      if (!runOverridesEnabled) {
        setGlobal(next);
        return;
      }
      updateActiveProfile((prev) => {
        const prevOverrides = (prev.runOverrides || {}) as RunProfileOverrides;
        const hasPrev = Object.prototype.hasOwnProperty.call(prevOverrides, key);
        const prevValue = hasPrev ? (prevOverrides as Record<string, T>)[key as string] : globalValue;
        const nextValue = typeof next === "function" ? (next as (v: T) => T)(prevValue) : next;
        if (hasPrev && Object.is(nextValue, prevValue)) return prev;
        return {
          ...prev,
          runOverridesEnabled: true,
          runOverrides: { ...prevOverrides, [key]: nextValue },
        };
      });
    },
    [runOverridesEnabled, updateActiveProfile],
  );

  const tools = resolveRunValue("tools", toolsGlobal);
  const yolo = resolveRunValue("yolo", yoloGlobal);
  const hostPolicy = resolveRunValue("hostPolicy", hostPolicyGlobal);
  const automationProfile = resolveRunValue("automationProfile", automationProfileGlobal);
  const verbose = resolveRunValue("verbose", verboseGlobal);
  const model = resolveRunValue("model", modelGlobal);
  const summaryModel = resolveRunValue("summaryModel", summaryModelGlobal);
  const summaryMaxChars = resolveRunValue("summaryMaxChars", summaryMaxCharsGlobal);
  const baseUrl = resolveRunValue("baseUrl", baseUrlGlobal);
  const apiKey = resolveRunValue("apiKey", apiKeyGlobal);
  const proxyUrl = resolveRunValue("proxyUrl", proxyUrlGlobal);
  const timeoutMs = resolveRunValue("timeoutMs", timeoutMsGlobal);
  const maxCaptureBytes = resolveRunValue("maxCaptureBytes", maxCaptureBytesGlobal);
  const streamAssistant = resolveRunValue("streamAssistant", streamAssistantGlobal);
  const trace = resolveRunValue("trace", traceGlobal);
  const useAsync = resolveRunValue("useAsync", useAsyncGlobal);
  const maxSteps = resolveRunValue("maxSteps", maxStepsRawGlobal);
  const maxRepeatedToolCalls = resolveRunValue("maxRepeatedToolCalls", maxRepeatedToolCallsGlobal);
  const maxToolCallsTotal = resolveRunValue("maxToolCallsTotal", maxToolCallsTotalGlobal);
  const maxToolCallsPerTool = resolveRunValue("maxToolCallsPerTool", maxToolCallsPerToolGlobal);
  const toolCallLimits = resolveRunValue("toolCallLimits", toolCallLimitsGlobal);
  const maxChars = resolveRunValue("maxChars", maxCharsGlobal);
  const keepLast = resolveRunValue("keepLast", keepLastGlobal);
  const memoryContextMode = resolveRunValue("memoryContextMode", memoryContextModeGlobal);
  const memoryIncludeStructured = resolveRunValue("memoryIncludeStructured", memoryIncludeStructuredGlobal);
  const memoryIncludeCore = resolveRunValue("memoryIncludeCore", memoryIncludeCoreGlobal);
  const memoryIncludeDaily = resolveRunValue("memoryIncludeDaily", memoryIncludeDailyGlobal);
  const memoryIncludeSession = resolveRunValue("memoryIncludeSession", memoryIncludeSessionGlobal);
  const memoryDailyDays = resolveRunValue("memoryDailyDays", memoryDailyDaysGlobal);
  const memoryTotalCap = resolveRunValue("memoryTotalCap", memoryTotalCapGlobal);
  const memorySearchQuery = resolveRunValue("memorySearchQuery", memorySearchQueryGlobal);
  const memorySearchOrder = resolveRunValue("memorySearchOrder", memorySearchOrderGlobal);
  const memorySearchUseIndex = resolveRunValue("memorySearchUseIndex", memorySearchUseIndexGlobal);
  const memorySearchCaseSensitive = resolveRunValue("memorySearchCaseSensitive", memorySearchCaseSensitiveGlobal);
  const memorySearchFallbackToFiles = resolveRunValue("memorySearchFallbackToFiles", memorySearchFallbackToFilesGlobal);
  const memorySearchMaxResults = resolveRunValue("memorySearchMaxResults", memorySearchMaxResultsGlobal);
  const memorySearchMaxSnippetChars = resolveRunValue("memorySearchMaxSnippetChars", memorySearchMaxSnippetCharsGlobal);
  const memorySearchContextLines = resolveRunValue("memorySearchContextLines", memorySearchContextLinesGlobal);

  const setTools = React.useCallback(
    (next: React.SetStateAction<ToolMode>) => setRunValue("tools", next, toolsGlobal, setToolsGlobal),
    [setRunValue, setToolsGlobal, toolsGlobal],
  );

  const setYolo = React.useCallback(
    (next: React.SetStateAction<boolean>) => setRunValue("yolo", next, yoloGlobal, setYoloGlobal),
    [setRunValue, setYoloGlobal, yoloGlobal],
  );

  const setHostPolicy = React.useCallback(
    (next: React.SetStateAction<HostPolicy>) => setRunValue("hostPolicy", next, hostPolicyGlobal, setHostPolicyGlobal),
    [hostPolicyGlobal, setHostPolicyGlobal, setRunValue],
  );

  const setAutomationProfile = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("automationProfile", next, automationProfileGlobal, setAutomationProfileGlobal),
    [automationProfileGlobal, setAutomationProfileGlobal, setRunValue],
  );

  const setVerbose = React.useCallback(
    (next: React.SetStateAction<boolean>) => setRunValue("verbose", next, verboseGlobal, setVerboseGlobal),
    [setRunValue, setVerboseGlobal, verboseGlobal],
  );

  const setModel = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("model", next, modelGlobal, setModelGlobal),
    [modelGlobal, setModelGlobal, setRunValue],
  );

  const setSummaryModel = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("summaryModel", next, summaryModelGlobal, setSummaryModelGlobal),
    [setRunValue, setSummaryModelGlobal, summaryModelGlobal],
  );

  const setSummaryMaxChars = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("summaryMaxChars", next, summaryMaxCharsGlobal, setSummaryMaxCharsGlobal),
    [setRunValue, setSummaryMaxCharsGlobal, summaryMaxCharsGlobal],
  );

  const setBaseUrl = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("baseUrl", next, baseUrlGlobal, setBaseUrlGlobal),
    [baseUrlGlobal, setBaseUrlGlobal, setRunValue],
  );

  const setApiKey = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("apiKey", next, apiKeyGlobal, setApiKeyGlobal),
    [apiKeyGlobal, setApiKeyGlobal, setRunValue],
  );

  const setProxyUrl = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("proxyUrl", next, proxyUrlGlobal, setProxyUrlGlobal),
    [proxyUrlGlobal, setProxyUrlGlobal, setRunValue],
  );

  const setTimeoutMs = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("timeoutMs", next, timeoutMsGlobal, setTimeoutMsGlobal),
    [setRunValue, setTimeoutMsGlobal, timeoutMsGlobal],
  );

  const setMaxCaptureBytes = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("maxCaptureBytes", next, maxCaptureBytesGlobal, setMaxCaptureBytesGlobal),
    [maxCaptureBytesGlobal, setMaxCaptureBytesGlobal, setRunValue],
  );

  const setStreamAssistant = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("streamAssistant", next, streamAssistantGlobal, setStreamAssistantGlobal),
    [setRunValue, setStreamAssistantGlobal, streamAssistantGlobal],
  );

  const setTrace = React.useCallback(
    (next: React.SetStateAction<boolean>) => setRunValue("trace", next, traceGlobal, setTraceGlobal),
    [setRunValue, setTraceGlobal, traceGlobal],
  );

  const setUseAsync = React.useCallback(
    (next: React.SetStateAction<boolean>) => setRunValue("useAsync", next, useAsyncGlobal, setUseAsyncGlobal),
    [setRunValue, setUseAsyncGlobal, useAsyncGlobal],
  );

  const setMaxSteps = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      if (runOverridesEnabled) {
        setRunValue("maxSteps", next, maxStepsRawGlobal, setMaxStepsRawGlobal);
        return;
      }
      setMaxStepsUserSet(true);
      setMaxStepsRawGlobal((prev) => (typeof next === "function" ? next(prev) : next));
    },
    [maxStepsRawGlobal, runOverridesEnabled, setMaxStepsRawGlobal, setMaxStepsUserSet, setRunValue],
  );

  const setMaxRepeatedToolCalls = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("maxRepeatedToolCalls", next, maxRepeatedToolCallsGlobal, setMaxRepeatedToolCallsGlobal),
    [maxRepeatedToolCallsGlobal, setMaxRepeatedToolCallsGlobal, setRunValue],
  );

  const setMaxToolCallsTotal = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("maxToolCallsTotal", next, maxToolCallsTotalGlobal, setMaxToolCallsTotalGlobal),
    [maxToolCallsTotalGlobal, setMaxToolCallsTotalGlobal, setRunValue],
  );

  const setMaxToolCallsPerTool = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("maxToolCallsPerTool", next, maxToolCallsPerToolGlobal, setMaxToolCallsPerToolGlobal),
    [maxToolCallsPerToolGlobal, setMaxToolCallsPerToolGlobal, setRunValue],
  );

  const setToolCallLimits = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("toolCallLimits", next, toolCallLimitsGlobal, setToolCallLimitsGlobal),
    [setRunValue, setToolCallLimitsGlobal, toolCallLimitsGlobal],
  );

  const setMaxChars = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("maxChars", next, maxCharsGlobal, setMaxCharsGlobal),
    [maxCharsGlobal, setMaxCharsGlobal, setRunValue],
  );

  const setKeepLast = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("keepLast", next, keepLastGlobal, setKeepLastGlobal),
    [keepLastGlobal, setKeepLastGlobal, setRunValue],
  );
  const setMemoryContextMode = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("memoryContextMode", next, memoryContextModeGlobal, setMemoryContextModeGlobal),
    [memoryContextModeGlobal, setMemoryContextModeGlobal, setRunValue],
  );
  const setMemoryIncludeStructured = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memoryIncludeStructured", next, memoryIncludeStructuredGlobal, setMemoryIncludeStructuredGlobal),
    [memoryIncludeStructuredGlobal, setMemoryIncludeStructuredGlobal, setRunValue],
  );
  const setMemoryIncludeCore = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memoryIncludeCore", next, memoryIncludeCoreGlobal, setMemoryIncludeCoreGlobal),
    [memoryIncludeCoreGlobal, setMemoryIncludeCoreGlobal, setRunValue],
  );
  const setMemoryIncludeDaily = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memoryIncludeDaily", next, memoryIncludeDailyGlobal, setMemoryIncludeDailyGlobal),
    [memoryIncludeDailyGlobal, setMemoryIncludeDailyGlobal, setRunValue],
  );
  const setMemoryIncludeSession = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memoryIncludeSession", next, memoryIncludeSessionGlobal, setMemoryIncludeSessionGlobal),
    [memoryIncludeSessionGlobal, setMemoryIncludeSessionGlobal, setRunValue],
  );
  const setMemoryDailyDays = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("memoryDailyDays", next, memoryDailyDaysGlobal, setMemoryDailyDaysGlobal),
    [memoryDailyDaysGlobal, setMemoryDailyDaysGlobal, setRunValue],
  );
  const setMemoryTotalCap = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("memoryTotalCap", next, memoryTotalCapGlobal, setMemoryTotalCapGlobal),
    [memoryTotalCapGlobal, setMemoryTotalCapGlobal, setRunValue],
  );
  const setMemorySearchQuery = React.useCallback(
    (next: React.SetStateAction<string>) => setRunValue("memorySearchQuery", next, memorySearchQueryGlobal, setMemorySearchQueryGlobal),
    [memorySearchQueryGlobal, setMemorySearchQueryGlobal, setRunValue],
  );
  const setMemorySearchOrder = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("memorySearchOrder", next, memorySearchOrderGlobal, setMemorySearchOrderGlobal),
    [memorySearchOrderGlobal, setMemorySearchOrderGlobal, setRunValue],
  );
  const setMemorySearchUseIndex = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memorySearchUseIndex", next, memorySearchUseIndexGlobal, setMemorySearchUseIndexGlobal),
    [memorySearchUseIndexGlobal, setMemorySearchUseIndexGlobal, setRunValue],
  );
  const setMemorySearchCaseSensitive = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memorySearchCaseSensitive", next, memorySearchCaseSensitiveGlobal, setMemorySearchCaseSensitiveGlobal),
    [memorySearchCaseSensitiveGlobal, setMemorySearchCaseSensitiveGlobal, setRunValue],
  );
  const setMemorySearchFallbackToFiles = React.useCallback(
    (next: React.SetStateAction<boolean>) =>
      setRunValue("memorySearchFallbackToFiles", next, memorySearchFallbackToFilesGlobal, setMemorySearchFallbackToFilesGlobal),
    [memorySearchFallbackToFilesGlobal, setMemorySearchFallbackToFilesGlobal, setRunValue],
  );
  const setMemorySearchMaxResults = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("memorySearchMaxResults", next, memorySearchMaxResultsGlobal, setMemorySearchMaxResultsGlobal),
    [memorySearchMaxResultsGlobal, setMemorySearchMaxResultsGlobal, setRunValue],
  );
  const setMemorySearchMaxSnippetChars = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("memorySearchMaxSnippetChars", next, memorySearchMaxSnippetCharsGlobal, setMemorySearchMaxSnippetCharsGlobal),
    [memorySearchMaxSnippetCharsGlobal, setMemorySearchMaxSnippetCharsGlobal, setRunValue],
  );
  const setMemorySearchContextLines = React.useCallback(
    (next: React.SetStateAction<string>) =>
      setRunValue("memorySearchContextLines", next, memorySearchContextLinesGlobal, setMemorySearchContextLinesGlobal),
    [memorySearchContextLinesGlobal, setMemorySearchContextLinesGlobal, setRunValue],
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
        const v = typeof next === "function" ? next(prev.mode) : next;
        if (v === prev.mode) return prev;
        return { ...prev, mode: v };
      });
    },
    [updateActiveProfile],
  );

  const setBase = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.base) : next;
        if (v === prev.base) return prev;
        return { ...prev, base: v };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerBase = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.brokerBase) : next;
        if (v === prev.brokerBase) return prev;
        return { ...prev, brokerBase: v };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerAgentId = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.brokerAgentId) : next;
        if (v === prev.brokerAgentId) return prev;
        return { ...prev, brokerAgentId: v };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerDeploymentId = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.brokerDeploymentId) : next;
        if (v === prev.brokerDeploymentId) return prev;
        return { ...prev, brokerDeploymentId: v };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerCookieAuth = React.useCallback<React.Dispatch<React.SetStateAction<boolean>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.brokerCookieAuth) : next;
        if (v === prev.brokerCookieAuth) return prev;
        return { ...prev, brokerCookieAuth: v };
      });
    },
    [updateActiveProfile],
  );

  const setBrokerAuthToken = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.brokerAuthToken) : next;
        if (v === prev.brokerAuthToken) return prev;
        return { ...prev, brokerAuthToken: v };
      });
    },
    [updateActiveProfile],
  );

  const setDaemonAuthToken = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.daemonAuthToken) : next;
        if (v === prev.daemonAuthToken) return prev;
        return { ...prev, daemonAuthToken: v };
      });
    },
    [updateActiveProfile],
  );

  const setProfileName = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      updateActiveProfile((prev) => {
        const v = typeof next === "function" ? next(prev.name) : next;
        const trimmed = String(v || "").trim();
        const nextName = trimmed || profileNameDefault(prev);
        if (nextName === prev.name) return prev;
        return { ...prev, name: nextName };
      });
    },
    [updateActiveProfile],
  );

  const effectiveBase = React.useMemo(() => {
    if (connectionMode === "broker") {
      const bb = normalizeHttpBase(brokerBase, "https://127.0.0.1:8443", "https");
      const aid = String(brokerAgentId || "").trim();
      if (!aid) return bb;
      return `${bb}/v1/agents/${encodeURIComponent(aid)}/proxy`;
    }

    return normalizeHttpBase(base, "http://127.0.0.1:8123", "http");
  }, [base, brokerAgentId, brokerBase, connectionMode]);

  const effectiveSseBase = React.useMemo(() => {
    if (connectionMode !== "broker") return effectiveBase;
    const bb = String(brokerBase || "").trim().replace(/\/+$/, "");
    const withScheme = /^https?:\/\//i.test(bb) ? bb : `https://${bb}`;
    const aid = String(brokerAgentId || "").trim();
    if (!aid) return withScheme;
    return `${withScheme}/v1/agents/${encodeURIComponent(aid)}/proxy_sse`;
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
    const t = typeof daemonAuth.token === "string" ? daemonAuth.token.trim() : "";
    const at = daemonAuth.mode === "broker" && typeof daemonAuth.agentdToken === "string" ? daemonAuth.agentdToken.trim() : "";
    const dep = daemonAuth.mode === "broker" && typeof daemonAuth.deploymentId === "string" ? daemonAuth.deploymentId.trim() : "";
    const cookie = daemonAuth.mode === "broker" && daemonAuth.useCookieAuth ? "cookie=1" : "cookie=0";
    const pid = String(activeProfileId || "default");
    return mode === "broker"
      ? `broker:pid=${pid}:dep=${dep}:${cookie}:tlen=${t.length}:alen=${at.length}`
      : `direct:pid=${pid}:tlen=${t.length}`;
  }, [activeProfileId, daemonAuth]);

  const serverPrefsBase = React.useMemo(() => {
    if (connectionMode === "broker") {
      return normalizeHttpBase(brokerBase, "https://127.0.0.1:8443", "https");
    }
    return normalizeHttpBase(base, "http://127.0.0.1:8123", "http");
  }, [base, brokerBase, connectionMode]);

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
          const r = await fetch(`${baseNoSlash}/v1/caps`, daemonFetchInit(daemonAuth));
          if (!r.ok) {
            throw new Error(`caps ${r.status}`);
          }
          const j = await r.json();
          const enabled = !!j?.features?.client_prefs?.enabled;
          if (cancelled) return;
          setServerPrefsAutoEnabled(enabled);
          setServerPrefsAutoStatus(enabled ? "ready" : "unsupported");
          return;
        }
        const r = await fetch(`${baseTrimmed}/api/v1/caps`, daemonFetchInit(daemonAuth));
        if (r.status === 401 || r.status === 403) {
          if (cancelled) return;
          setServerPrefsAutoEnabled(false);
          setServerPrefsAutoStatus("auth_required");
          setServerPrefsAutoError(null);
          return;
        }
        if (!r.ok) {
          throw new Error(`caps ${r.status}`);
        }
        const j = await r.json();
        const enabled = !!j?.features?.client_prefs?.enabled;
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
  }, [
    serverPrefsAuto,
    serverPrefsAuthReady,
    serverPrefsBase,
    connectionMode,
    daemonAuth,
  ]);

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
  const connectionProfilesRef = React.useRef(connectionProfiles.map((p) => mergeProfileSecrets(p, connectionProfileSecrets[p.id])));
  React.useEffect(() => {
    connectionProfilesRef.current = connectionProfiles.map((p) => mergeProfileSecrets(p, connectionProfileSecrets[p.id]));
  }, [connectionProfileSecrets, connectionProfiles]);

  const pullServerPrefs = React.useCallback(async () => {
    if (!serverPrefsCanUse) return;
    if (serverPrefsPullInFlightRef.current) return;
    serverPrefsPullInFlightRef.current = true;
    setServerPrefsStatus("loading");
    setServerPrefsError(null);
    try {
      const isBrokerPrefs = connectionMode === "broker";
      const resp = isBrokerPrefs
        ? await apiBrokerGetClientPrefs(
            serverPrefsBase,
            String(clientId || "webui"),
            serverPrefsClientKind,
            daemonAuth,
          )
        : await apiGetClientPrefs(serverPrefsBase, String(clientId || "webui"), serverPrefsClientKind, daemonAuth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "client prefs fetch failed");
      }
      if (resp.found && resp.prefs && typeof resp.prefs === "object") {
        const merged = mergeServerPrefs(resp.prefs as ServerPrefs, connectionProfilesRef.current, defaults);
        setConnectionProfiles(merged.profiles);
        if (merged.activeProfileId) setActiveProfileId(merged.activeProfileId);
        serverPrefsLastPayloadRef.current = JSON.stringify({
          client_id: String(clientId || "webui"),
          prefs: buildServerPrefs(merged.profiles, merged.activeProfileId),
        });
      }
      setServerPrefsLastSyncMs(typeof resp.updated_utc_ms === "number" ? resp.updated_utc_ms : Date.now());
      setServerPrefsStatus("synced");
    } catch (err) {
      setServerPrefsStatus("error");
      setServerPrefsError(String(err instanceof Error ? err.message : err));
    } finally {
      serverPrefsPullInFlightRef.current = false;
    }
  }, [
    clientId,
    daemonAuth,
    defaults,
    connectionMode,
    serverPrefsBase,
    serverPrefsCanUse,
    serverPrefsClientKind,
    setActiveProfileId,
    setConnectionProfiles,
  ]);

  const pushServerPrefs = React.useCallback(async () => {
    if (!serverPrefsCanUse) return;
    const payload = serverPrefsPayload;
    const payloadKey = serverPrefsPayloadKey;
    if (payloadKey === serverPrefsLastPayloadRef.current) return;
    setServerPrefsStatus("loading");
    setServerPrefsError(null);
    try {
      const isBrokerPrefs = connectionMode === "broker";
      const req = { client_id: String(clientId || "webui"), client_kind: serverPrefsClientKind, prefs: payload };
      const resp = isBrokerPrefs
        ? await apiBrokerPostClientPrefs(serverPrefsBase, req, daemonAuth)
        : await apiPostClientPrefs(serverPrefsBase, req, daemonAuth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "client prefs update failed");
      }
      serverPrefsLastPayloadRef.current = payloadKey;
      setServerPrefsLastSyncMs(typeof resp.updated_utc_ms === "number" ? resp.updated_utc_ms : Date.now());
      setServerPrefsStatus("synced");
    } catch (err) {
      setServerPrefsStatus("error");
      setServerPrefsError(String(err instanceof Error ? err.message : err));
    }
  }, [
    clientId,
    daemonAuth,
    connectionMode,
    serverPrefsBase,
    serverPrefsCanUse,
    serverPrefsClientKind,
    serverPrefsPayload,
    serverPrefsPayloadJson,
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
  }, [serverPrefsCanUse, pullServerPrefs]);

  React.useEffect(() => {
    if (!serverPrefsCanUse) return;
    if (serverPrefsPayloadJson === serverPrefsLastPayloadRef.current) return;
    const t = window.setTimeout(() => {
      void pushServerPrefs();
    }, 600);
    return () => window.clearTimeout(t);
  }, [pushServerPrefs, serverPrefsCanUse, serverPrefsPayloadKey]);

  React.useEffect(() => {
    if (maxStepsUserSet) return;
    if (String(maxStepsRawGlobal) === "0") {
      setMaxStepsRawGlobal("");
    }
  }, [maxStepsRawGlobal, maxStepsUserSet, setMaxStepsRawGlobal]);

  return {
    defaults,
    showSettings,
    setShowSettings,
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
    run: {
      profileOverridesEnabled: runOverridesEnabled,
      setProfileOverridesEnabled,
      copyProfileOverridesFromGlobal,
      clearProfileOverrides,
      tools,
      setTools,
      yolo,
      setYolo,
      hostPolicy,
      setHostPolicy,
      automationProfile,
      setAutomationProfile,
      verbose,
      setVerbose,
      model,
      setModel,
      summaryModel,
      setSummaryModel,
      summaryMaxChars,
      setSummaryMaxChars,
      baseUrl,
      setBaseUrl,
      apiKey,
      setApiKey,
      proxyUrl,
      setProxyUrl,
      timeoutMs,
      setTimeoutMs,
      maxCaptureBytes,
      setMaxCaptureBytes,
      streamAssistant,
      setStreamAssistant,
      trace,
      setTrace,
      useAsync,
      setUseAsync,
      maxSteps,
      setMaxSteps,
      maxRepeatedToolCalls,
      setMaxRepeatedToolCalls,
      maxToolCallsTotal,
      setMaxToolCallsTotal,
      maxToolCallsPerTool,
      setMaxToolCallsPerTool,
      toolCallLimits,
      setToolCallLimits,
      maxChars,
      setMaxChars,
      keepLast,
      setKeepLast,
      memoryContextMode,
      setMemoryContextMode,
      memoryIncludeStructured,
      setMemoryIncludeStructured,
      memoryIncludeCore,
      setMemoryIncludeCore,
      memoryIncludeDaily,
      setMemoryIncludeDaily,
      memoryIncludeSession,
      setMemoryIncludeSession,
      memoryDailyDays,
      setMemoryDailyDays,
      memoryTotalCap,
      setMemoryTotalCap,
      memorySearchQuery,
      setMemorySearchQuery,
      memorySearchOrder,
      setMemorySearchOrder,
      memorySearchUseIndex,
      setMemorySearchUseIndex,
      memorySearchCaseSensitive,
      setMemorySearchCaseSensitive,
      memorySearchFallbackToFiles,
      setMemorySearchFallbackToFiles,
      memorySearchMaxResults,
      setMemorySearchMaxResults,
      memorySearchMaxSnippetChars,
      setMemorySearchMaxSnippetChars,
      memorySearchContextLines,
      setMemorySearchContextLines,
      orMinTotal,
      setOrMinTotal,
      orMaxTotal,
      setOrMaxTotal,
      orRequireMultimodal,
      setOrRequireMultimodal,
      orRequireTools,
      setOrRequireTools,
      orLimit,
      setOrLimit,
    },
    client: {
      clientId: String(clientId || "webui"),
      setClientId,
      allowAutoplay,
      setAllowAutoplay,
      allowClientRpcs,
      setAllowClientRpcs,
      allowClientEffects,
      setAllowClientEffects,
      allowUnsafePageEval,
      setAllowUnsafePageEval,
      showDebugInConversation,
      setShowDebugInConversation,
    },
    brokerPanelOpen,
    setBrokerPanelOpen,
  };
}
