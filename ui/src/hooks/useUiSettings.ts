import React from "react";
import useAutoplayUnlock from "./useAutoplayUnlock";
import useLocalStorageState from "./useLocalStorageState";
import {
  getUiDefaults,
  type AgentUIDefaults,
  type ConnectionMode,
  type HostPolicy,
  type ToolMode,
} from "../runtime_config";
import type { ApiAuth } from "../api";

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
  brokerAuthToken: string;
  setBrokerAuthToken: React.Dispatch<React.SetStateAction<string>>;
  daemonAuthToken: string;
  setDaemonAuthToken: React.Dispatch<React.SetStateAction<string>>;
  effectiveBase: string;
  effectiveSseBase: string;
  daemonAuth: ApiAuth;
  authKey: string;
};

export type RunProfileOverrides = {
  tools?: ToolMode;
  yolo?: boolean;
  hostPolicy?: HostPolicy;
  verbose?: boolean;
  model?: string;
  summaryModel?: string;
  summaryMaxChars?: string;
  baseUrl?: string;
  apiKey?: string;
  proxyUrl?: string;
  timeoutMs?: string;
  maxCaptureBytes?: string;
  streamAssistant?: boolean;
  trace?: boolean;
  useAsync?: boolean;
  maxSteps?: string;
  maxRepeatedToolCalls?: string;
  maxToolCallsTotal?: string;
  maxToolCallsPerTool?: string;
  toolCallLimits?: string;
  maxChars?: string;
  keepLast?: string;
  memoryContextMode?: string;
  memoryIncludeStructured?: boolean;
  memoryIncludeCore?: boolean;
  memoryIncludeDaily?: boolean;
  memoryIncludeSession?: boolean;
  memoryDailyDays?: string;
  memoryTotalCap?: string;
  memorySearchQuery?: string;
  memorySearchUseIndex?: boolean;
  memorySearchCaseSensitive?: boolean;
  memorySearchFallbackToFiles?: boolean;
  memorySearchMaxResults?: string;
  memorySearchMaxSnippetChars?: string;
  memorySearchContextLines?: string;
};

export type ConnectionProfile = {
  id: string;
  name: string;
  mode: ConnectionMode;
  base: string;
  brokerBase: string;
  brokerAgentId: string;
  brokerDeploymentId: string;
  brokerAuthToken: string;
  daemonAuthToken: string;
  runOverridesEnabled?: boolean;
  runOverrides?: RunProfileOverrides;
};

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

const normalizeHttpBase = (raw: string, fallback: string, defaultScheme: "http" | "https") => {
  const b = String(raw || "").trim();
  if (b.length === 0) return fallback;
  const withScheme = /^https?:\/\//i.test(b) ? b : `${defaultScheme}://${b}`;
  return withScheme.replace(/\/+$/, "");
};

const safeParse = <T,>(raw: string | null): T | undefined => {
  if (!raw) return undefined;
  try {
    return JSON.parse(raw) as T;
  } catch {
    return undefined;
  }
};

const readLegacyString = (key: string, fallback: string): string => {
  if (typeof window === "undefined") return fallback;
  const raw = window.localStorage.getItem(key);
  if (raw === null) return fallback;
  const parsed = safeParse<string>(raw);
  if (typeof parsed === "string") return parsed;
  if (typeof parsed === "number" && Number.isFinite(parsed)) return String(parsed);
  if (typeof parsed === "boolean") return parsed ? "true" : "false";
  const trimmed = raw.trim();
  return trimmed ? trimmed : fallback;
};

const readLegacyMode = (key: string, fallback: ConnectionMode): ConnectionMode => {
  const raw = readLegacyString(key, fallback);
  return raw === "broker" ? "broker" : "direct";
};

const hostLabel = (raw: string, fallback: string, scheme: "http" | "https") => {
  const norm = normalizeHttpBase(raw, fallback, scheme);
  try {
    const u = new URL(norm);
    return u.port ? `${u.hostname}:${u.port}` : u.hostname;
  } catch {
    return norm;
  }
};

const generateProfileId = () => {
  try {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const g: any = typeof globalThis !== "undefined" ? globalThis : {};
    if (g.crypto && typeof g.crypto.randomUUID === "function") {
      return `profile-${g.crypto.randomUUID()}`;
    }
  } catch {
    // ignore
  }
  return `profile-${Date.now()}-${Math.random().toString(16).slice(2)}`;
};

const profileNameDefault = (p: ConnectionProfile) => {
  if (p.mode === "broker") {
    const agent = String(p.brokerAgentId || "").trim();
    const dep = String(p.brokerDeploymentId || "").trim();
    if (agent) return dep ? `broker:${agent}@${dep}` : `broker:${agent}`;
    return `broker:${hostLabel(p.brokerBase, "https://127.0.0.1:8443", "https")}`;
  }
  return `daemon:${hostLabel(p.base, "http://127.0.0.1:8123", "http")}`;
};

const normalizeRunOverrides = (raw: unknown): RunProfileOverrides | undefined => {
  if (!raw || typeof raw !== "object") return undefined;
  const v = raw as Record<string, unknown>;
  const out: RunProfileOverrides = {};
  const readString = (val: unknown): string | undefined => {
    if (typeof val === "string") return val;
    if (typeof val === "number" && Number.isFinite(val)) return String(val);
    return undefined;
  };

  if (v.tools === "host" || v.tools === "basic" || v.tools === "none") out.tools = v.tools;
  if (typeof v.yolo === "boolean") out.yolo = v.yolo;
  if (v.hostPolicy === "full" || v.hostPolicy === "readonly") out.hostPolicy = v.hostPolicy;
  if (typeof v.verbose === "boolean") out.verbose = v.verbose;
  const model = readString(v.model);
  if (model !== undefined) out.model = model;
  const summaryModel = readString(v.summaryModel);
  if (summaryModel !== undefined) out.summaryModel = summaryModel;
  const summaryMaxChars = readString(v.summaryMaxChars);
  if (summaryMaxChars !== undefined) out.summaryMaxChars = summaryMaxChars;
  const baseUrl = readString(v.baseUrl);
  if (baseUrl !== undefined) out.baseUrl = baseUrl;
  const apiKey = readString(v.apiKey);
  if (apiKey !== undefined) out.apiKey = apiKey;
  const proxyUrl = readString(v.proxyUrl);
  if (proxyUrl !== undefined) out.proxyUrl = proxyUrl;
  const timeoutMs = readString(v.timeoutMs);
  if (timeoutMs !== undefined) out.timeoutMs = timeoutMs;
  const maxCaptureBytes = readString(v.maxCaptureBytes);
  if (maxCaptureBytes !== undefined) out.maxCaptureBytes = maxCaptureBytes;
  if (typeof v.streamAssistant === "boolean") out.streamAssistant = v.streamAssistant;
  if (typeof v.trace === "boolean") out.trace = v.trace;
  if (typeof v.useAsync === "boolean") out.useAsync = v.useAsync;
  const maxSteps = readString(v.maxSteps);
  if (maxSteps !== undefined) out.maxSteps = maxSteps;
  const maxRepeatedToolCalls = readString(v.maxRepeatedToolCalls);
  if (maxRepeatedToolCalls !== undefined) out.maxRepeatedToolCalls = maxRepeatedToolCalls;
  const maxToolCallsTotal = readString(v.maxToolCallsTotal);
  if (maxToolCallsTotal !== undefined) out.maxToolCallsTotal = maxToolCallsTotal;
  const maxToolCallsPerTool = readString(v.maxToolCallsPerTool);
  if (maxToolCallsPerTool !== undefined) out.maxToolCallsPerTool = maxToolCallsPerTool;
  const toolCallLimits = readString(v.toolCallLimits);
  if (toolCallLimits !== undefined) out.toolCallLimits = toolCallLimits;
  const maxChars = readString(v.maxChars);
  if (maxChars !== undefined) out.maxChars = maxChars;
  const keepLast = readString(v.keepLast);
  if (keepLast !== undefined) out.keepLast = keepLast;
  const memoryContextMode = readString(v.memoryContextMode);
  if (memoryContextMode === "files" || memoryContextMode === "search" || memoryContextMode === "index" || memoryContextMode === "salience") {
    out.memoryContextMode = memoryContextMode;
  }
  if (typeof v.memoryIncludeStructured === "boolean") out.memoryIncludeStructured = v.memoryIncludeStructured;
  if (typeof v.memoryIncludeCore === "boolean") out.memoryIncludeCore = v.memoryIncludeCore;
  if (typeof v.memoryIncludeDaily === "boolean") out.memoryIncludeDaily = v.memoryIncludeDaily;
  if (typeof v.memoryIncludeSession === "boolean") out.memoryIncludeSession = v.memoryIncludeSession;
  const memoryDailyDays = readString(v.memoryDailyDays);
  if (memoryDailyDays !== undefined) out.memoryDailyDays = memoryDailyDays;
  const memoryTotalCap = readString(v.memoryTotalCap);
  if (memoryTotalCap !== undefined) out.memoryTotalCap = memoryTotalCap;
  const memorySearchQuery = readString(v.memorySearchQuery);
  if (memorySearchQuery !== undefined) out.memorySearchQuery = memorySearchQuery;
  if (typeof v.memorySearchUseIndex === "boolean") out.memorySearchUseIndex = v.memorySearchUseIndex;
  if (typeof v.memorySearchCaseSensitive === "boolean") out.memorySearchCaseSensitive = v.memorySearchCaseSensitive;
  if (typeof v.memorySearchFallbackToFiles === "boolean") out.memorySearchFallbackToFiles = v.memorySearchFallbackToFiles;
  const memorySearchMaxResults = readString(v.memorySearchMaxResults);
  if (memorySearchMaxResults !== undefined) out.memorySearchMaxResults = memorySearchMaxResults;
  const memorySearchMaxSnippetChars = readString(v.memorySearchMaxSnippetChars);
  if (memorySearchMaxSnippetChars !== undefined) out.memorySearchMaxSnippetChars = memorySearchMaxSnippetChars;
  const memorySearchContextLines = readString(v.memorySearchContextLines);
  if (memorySearchContextLines !== undefined) out.memorySearchContextLines = memorySearchContextLines;

  return Object.keys(out).length > 0 ? out : undefined;
};

const normalizeProfile = (p: Partial<ConnectionProfile>, defaults: AgentUIDefaults): ConnectionProfile => {
  const mode = p.mode === "broker" ? "broker" : "direct";
  const base = typeof p.base === "string" ? p.base : defaults.daemonBaseUrl;
  const brokerBase = typeof p.brokerBase === "string" ? p.brokerBase : defaults.brokerBaseUrl;
  const brokerAgentId = typeof p.brokerAgentId === "string" ? p.brokerAgentId : defaults.brokerAgentId;
  const brokerDeploymentId = typeof p.brokerDeploymentId === "string" ? p.brokerDeploymentId : defaults.brokerDeploymentId;
  const brokerAuthToken = typeof p.brokerAuthToken === "string" ? p.brokerAuthToken : defaults.brokerAuthToken;
  const daemonAuthToken = typeof p.daemonAuthToken === "string" ? p.daemonAuthToken : defaults.daemonAuthToken;
  const id = typeof p.id === "string" && p.id.trim() ? p.id : generateProfileId();
  const name = typeof p.name === "string" ? p.name.trim() : "";
  const runOverridesEnabled = typeof p.runOverridesEnabled === "boolean" ? p.runOverridesEnabled : false;
  const runOverrides = normalizeRunOverrides(p.runOverrides);
  const out: ConnectionProfile = {
    id,
    name: name || "",
    mode,
    base,
    brokerBase,
    brokerAgentId,
    brokerDeploymentId,
    brokerAuthToken,
    daemonAuthToken,
    runOverridesEnabled,
    runOverrides,
  };
  if (!out.name) out.name = profileNameDefault(out);
  return out;
};

export default function useUiSettings(): UiSettings {
  const defaults = React.useMemo(() => getUiDefaults(), []);

  const [showSettings, setShowSettings] = useLocalStorageState("agentui.showSettings", false);

  const [connectionProfiles, setConnectionProfiles] = useLocalStorageState<ConnectionProfile[]>(
    "agentui.connectionProfiles",
    [],
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
        brokerAuthToken: readLegacyString("agentui.brokerAuthToken", defaults.brokerAuthToken),
        daemonAuthToken: readLegacyString("agentui.daemonAuthToken", defaults.daemonAuthToken),
      },
      defaults,
    );
  }, [defaults]);

  React.useEffect(() => {
    if (connectionProfiles.length === 0) {
      const legacy = buildLegacyProfile();
      setConnectionProfiles([legacy]);
      setActiveProfileId(legacy.id);
      return;
    }
    let changed = false;
    const normalized = connectionProfiles.map((p) => {
      const np = normalizeProfile(p, defaults);
      const prevOverridesJson = JSON.stringify((p as ConnectionProfile).runOverrides ?? null);
      const nextOverridesJson = JSON.stringify(np.runOverrides ?? null);
      const prevOverridesEnabled = typeof p.runOverridesEnabled === "boolean" ? p.runOverridesEnabled : false;
      if (
        np.id !== p.id ||
        np.name !== p.name ||
        np.mode !== p.mode ||
        np.base !== p.base ||
        np.brokerBase !== p.brokerBase ||
        np.brokerAgentId !== p.brokerAgentId ||
        np.brokerDeploymentId !== p.brokerDeploymentId ||
        np.brokerAuthToken !== p.brokerAuthToken ||
        np.daemonAuthToken !== p.daemonAuthToken ||
        np.runOverridesEnabled !== prevOverridesEnabled ||
        prevOverridesJson !== nextOverridesJson
      ) {
        changed = true;
      }
      return np;
    });
    if (changed) setConnectionProfiles(normalized);
    if (!normalized.some((p) => p.id === activeProfileId)) {
      setActiveProfileId(normalized[0]?.id || "");
    }
  }, [activeProfileId, buildLegacyProfile, connectionProfiles, defaults, setActiveProfileId, setConnectionProfiles]);

  const [toolsGlobal, setToolsGlobal] = useLocalStorageState<ToolMode>("agentui.tools", defaults.tools);
  const [yoloGlobal, setYoloGlobal] = useLocalStorageState("agentui.yolo", defaults.yolo);
  const [hostPolicyGlobal, setHostPolicyGlobal] = useLocalStorageState<HostPolicy>(
    "agentui.hostPolicy",
    defaults.hostPolicy,
  );
  const [verboseGlobal, setVerboseGlobal] = useLocalStorageState("agentui.verbose", defaults.verbose);
  const [modelGlobal, setModelGlobal] = useLocalStorageState("agentui.model", defaults.model);
  const [summaryModelGlobal, setSummaryModelGlobal] = useLocalStorageState("agentui.summaryModel", "");
  const [summaryMaxCharsGlobal, setSummaryMaxCharsGlobal] = useLocalStorageState("agentui.summaryMaxChars", "1200");
  const [baseUrlGlobal, setBaseUrlGlobal] = useLocalStorageState("agentui.baseUrl", defaults.baseUrl);
  const [apiKeyGlobal, setApiKeyGlobal] = useLocalStorageState("agentui.apiKey", defaults.apiKey);
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
    return found || connectionProfiles[0];
  }, [activeProfileId, connectionProfiles, fallbackProfile]);

  const updateActiveProfile = React.useCallback(
    (update: (prev: ConnectionProfile) => ConnectionProfile) => {
      setConnectionProfiles((prev) => {
        if (prev.length === 0) return prev;
        const idx = prev.findIndex((p) => p.id === activeProfileId);
        const useIdx = idx >= 0 ? idx : 0;
        const cur = prev[useIdx];
        const next = update(cur);
        if (next === cur) return prev;
        const out = prev.slice();
        out[useIdx] = normalizeProfile(next, defaults);
        return out;
      });
    },
    [activeProfileId, defaults, setConnectionProfiles],
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
        brokerAuthToken: defaults.brokerAuthToken,
        daemonAuthToken: defaults.daemonAuthToken,
      };
      const profile = normalizeProfile(seed, defaults);
      setConnectionProfiles((prev) => [...prev, profile]);
      setActiveProfileId(id);
    },
    [defaults, setActiveProfileId, setConnectionProfiles],
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
    setConnectionProfiles((prev) => [...prev, copy]);
    setActiveProfileId(id);
  }, [activeProfile, defaults, setActiveProfileId, setConnectionProfiles]);

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
      if (nextActive) setActiveProfileId(nextActive);
    },
    [activeProfileId, setActiveProfileId, setConnectionProfiles],
  );

  const runOverridesEnabled = !!activeProfile.runOverridesEnabled;
  const runOverrides = activeProfile.runOverrides || {};

  const snapshotRunOverrides = React.useCallback(
    (): RunProfileOverrides => ({
      tools: toolsGlobal,
      yolo: yoloGlobal,
      hostPolicy: hostPolicyGlobal,
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
      memorySearchUseIndex: memorySearchUseIndexGlobal,
      memorySearchCaseSensitive: memorySearchCaseSensitiveGlobal,
      memorySearchFallbackToFiles: memorySearchFallbackToFilesGlobal,
      memorySearchMaxResults: memorySearchMaxResultsGlobal,
      memorySearchMaxSnippetChars: memorySearchMaxSnippetCharsGlobal,
      memorySearchContextLines: memorySearchContextLinesGlobal,
    }),
    [
      apiKeyGlobal,
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
      };
    }
    return { mode: "direct", token: daemonAuthToken };
  }, [brokerAuthToken, brokerDeploymentId, connectionMode, daemonAuthToken]);

  const authKey = React.useMemo(() => {
    const mode = daemonAuth.mode;
    const t = typeof daemonAuth.token === "string" ? daemonAuth.token.trim() : "";
    const at = daemonAuth.mode === "broker" && typeof daemonAuth.agentdToken === "string" ? daemonAuth.agentdToken.trim() : "";
    const dep = daemonAuth.mode === "broker" && typeof daemonAuth.deploymentId === "string" ? daemonAuth.deploymentId.trim() : "";
    const pid = String(activeProfileId || "default");
    return mode === "broker"
      ? `broker:pid=${pid}:dep=${dep}:tlen=${t.length}:alen=${at.length}`
      : `direct:pid=${pid}:tlen=${t.length}`;
  }, [activeProfileId, daemonAuth]);

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
      brokerAuthToken,
      setBrokerAuthToken,
      daemonAuthToken,
      setDaemonAuthToken,
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
