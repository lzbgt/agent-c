import type { AgentUIDefaults, ConnectionMode, HostPolicy, ToolMode } from "../runtime_config";

export type RunProfileOverrides = {
  tools?: ToolMode;
  yolo?: boolean;
  hostPolicy?: HostPolicy;
  automationProfile?: string;
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
  memorySearchOrder?: string;
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

export type ServerConnectionProfile = {
  id: string;
  name: string;
  mode: ConnectionMode;
  base: string;
  brokerBase: string;
  brokerAgentId: string;
  brokerDeploymentId: string;
};

export type ServerPrefs = {
  connection: {
    active_profile_id: string;
    profiles: ServerConnectionProfile[];
  };
};

export type ConnectionProfileSecretState = {
  brokerAuthToken?: string;
  daemonAuthToken?: string;
  runOverridesApiKey?: string;
};

export type ConnectionProfileSecretMap = Record<string, ConnectionProfileSecretState>;

export const CONNECTION_PROFILE_SECRETS_KEY = "agentui.connectionProfileSecrets";
export const LEGACY_SECRET_KEYS = ["agentui.brokerAuthToken", "agentui.daemonAuthToken", "agentui.apiKey"] as const;

export const normalizeHttpBase = (raw: string, fallback: string, defaultScheme: "http" | "https") => {
  const b = String(raw || "").trim();
  if (b.length === 0) return fallback;
  if (/^https?:\/\//i.test(b)) return b.replace(/\/+$/, "");

  const hostPart = b.split("/")[0] || "";
  const host =
    hostPart.startsWith("[") && hostPart.includes("]")
      ? hostPart.slice(1, hostPart.indexOf("]"))
      : hostPart.split(":")[0] || "";
  const hostLower = host.toLowerCase();
  const isLoopback = hostLower === "localhost" || hostLower === "::1" || hostLower.startsWith("127.");
  let scheme = defaultScheme;
  if (isLoopback && typeof window !== "undefined") {
    const proto = String(window.location?.protocol || "").toLowerCase();
    if (proto === "http:" || proto === "https:") scheme = proto.slice(0, -1) as "http" | "https";
  }

  return `${scheme}://${b}`.replace(/\/+$/, "");
};

export const safeParse = <T,>(raw: string | null): T | undefined => {
  if (!raw) return undefined;
  try {
    return JSON.parse(raw) as T;
  } catch {
    return undefined;
  }
};

export const readLegacyString = (key: string, fallback: string): string => {
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

const readStorageString = (storage: Storage | undefined, key: string): string | undefined => {
  if (!storage) return undefined;
  const raw = storage.getItem(key);
  if (raw === null) return undefined;
  const parsed = safeParse<string>(raw);
  if (typeof parsed === "string") return parsed;
  if (typeof parsed === "number" && Number.isFinite(parsed)) return String(parsed);
  if (typeof parsed === "boolean") return parsed ? "true" : "false";
  const trimmed = raw.trim();
  return trimmed || undefined;
};

export const readLegacySecretString = (key: string, fallback: string): string => {
  if (typeof window === "undefined") return fallback;
  const sessionValue = readStorageString(window.sessionStorage, key);
  if (sessionValue !== undefined) return sessionValue;
  return readLegacyString(key, fallback);
};

export const readLegacyMode = (key: string, fallback: ConnectionMode): ConnectionMode => {
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

export const generateProfileId = () => {
  try {
    const g: { crypto?: { randomUUID?: () => string } } = typeof globalThis !== "undefined" ? globalThis : {};
    if (g.crypto && typeof g.crypto.randomUUID === "function") {
      return `profile-${g.crypto.randomUUID()}`;
    }
  } catch {
    // ignore
  }
  return `profile-${Date.now()}-${Math.random().toString(16).slice(2)}`;
};

export const profileNameDefault = (p: ConnectionProfile) => {
  if (p.mode === "broker") {
    const agent = String(p.brokerAgentId || "").trim();
    const dep = String(p.brokerDeploymentId || "").trim();
    if (agent) return dep ? `broker:${agent}@${dep}` : `broker:${agent}`;
    return `broker:${hostLabel(p.brokerBase, "https://127.0.0.1:8443", "https")}`;
  }
  return `daemon:${hostLabel(p.base, "http://127.0.0.1:8123", "http")}`;
};

export const normalizeRunOverrides = (raw: unknown): RunProfileOverrides | undefined => {
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
  const automationProfile = readString(v.automationProfile);
  if (automationProfile !== undefined) {
    const trimmed = automationProfile.trim();
    if (
      trimmed === "" ||
      trimmed === "full" ||
      trimmed === "guided" ||
      trimmed === "strict" ||
      trimmed === "custom"
    ) {
      out.automationProfile = trimmed;
    }
  }
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
  const memorySearchOrder = readString(v.memorySearchOrder);
  if (memorySearchOrder !== undefined) out.memorySearchOrder = memorySearchOrder;
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

export const stripSecretRunOverrides = (raw: RunProfileOverrides | undefined): RunProfileOverrides | undefined => {
  if (!raw) return undefined;
  const out = { ...raw };
  delete out.apiKey;
  return Object.keys(out).length > 0 ? out : undefined;
};

export const extractProfileSecrets = (p: Partial<ConnectionProfile>): ConnectionProfileSecretState => {
  const out: ConnectionProfileSecretState = {};
  if (typeof p.brokerAuthToken === "string" && p.brokerAuthToken.trim()) {
    out.brokerAuthToken = p.brokerAuthToken;
  }
  if (typeof p.daemonAuthToken === "string" && p.daemonAuthToken.trim()) {
    out.daemonAuthToken = p.daemonAuthToken;
  }
  const runOverrides = normalizeRunOverrides(p.runOverrides);
  if (typeof runOverrides?.apiKey === "string" && runOverrides.apiKey.trim()) {
    out.runOverridesApiKey = runOverrides.apiKey;
  }
  return out;
};

export const sanitizeProfileForPersistence = (p: ConnectionProfile): ConnectionProfile => ({
  ...p,
  brokerAuthToken: "",
  daemonAuthToken: "",
  runOverrides: stripSecretRunOverrides(p.runOverrides),
});

export const mergeProfileSecrets = (profile: ConnectionProfile, secret: ConnectionProfileSecretState | undefined): ConnectionProfile => {
  if (!secret) return profile;
  const out: ConnectionProfile = { ...profile };
  if (typeof secret.brokerAuthToken === "string") out.brokerAuthToken = secret.brokerAuthToken;
  if (typeof secret.daemonAuthToken === "string") out.daemonAuthToken = secret.daemonAuthToken;
  if (typeof secret.runOverridesApiKey === "string") {
    out.runOverrides = { ...(out.runOverrides || {}), apiKey: secret.runOverridesApiKey };
  }
  return out;
};

export const secretsEqual = (a?: ConnectionProfileSecretState, b?: ConnectionProfileSecretState): boolean =>
  (a?.brokerAuthToken || "") === (b?.brokerAuthToken || "") &&
  (a?.daemonAuthToken || "") === (b?.daemonAuthToken || "") &&
  (a?.runOverridesApiKey || "") === (b?.runOverridesApiKey || "");

export const normalizeSecretMap = (raw: unknown): ConnectionProfileSecretMap => {
  if (!raw || typeof raw !== "object") return {};
  const out: ConnectionProfileSecretMap = {};
  for (const [key, value] of Object.entries(raw as Record<string, unknown>)) {
    if (typeof key !== "string" || !key.trim() || !value || typeof value !== "object") continue;
    const entry = value as Record<string, unknown>;
    const next: ConnectionProfileSecretState = {};
    if (typeof entry.brokerAuthToken === "string" && entry.brokerAuthToken.trim()) {
      next.brokerAuthToken = entry.brokerAuthToken;
    }
    if (typeof entry.daemonAuthToken === "string" && entry.daemonAuthToken.trim()) {
      next.daemonAuthToken = entry.daemonAuthToken;
    }
    if (typeof entry.runOverridesApiKey === "string" && entry.runOverridesApiKey.trim()) {
      next.runOverridesApiKey = entry.runOverridesApiKey;
    }
    if (Object.keys(next).length > 0) out[key] = next;
  }
  return out;
};

export const normalizeProfile = (p: Partial<ConnectionProfile>, defaults: AgentUIDefaults): ConnectionProfile => {
  const mode = p.mode === "broker" ? "broker" : "direct";
  const base = typeof p.base === "string" ? p.base : defaults.daemonBaseUrl;
  const brokerBase = typeof p.brokerBase === "string" ? p.brokerBase : defaults.brokerBaseUrl;
  const brokerAgentId = typeof p.brokerAgentId === "string" ? p.brokerAgentId : defaults.brokerAgentId;
  const brokerDeploymentId = typeof p.brokerDeploymentId === "string" ? p.brokerDeploymentId : defaults.brokerDeploymentId;
  const brokerAuthToken = typeof p.brokerAuthToken === "string" ? p.brokerAuthToken : "";
  const daemonAuthToken = typeof p.daemonAuthToken === "string" ? p.daemonAuthToken : "";
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

const toServerProfile = (p: ConnectionProfile): ServerConnectionProfile => ({
  id: p.id,
  name: p.name,
  mode: p.mode,
  base: p.base,
  brokerBase: p.brokerBase,
  brokerAgentId: p.brokerAgentId,
  brokerDeploymentId: p.brokerDeploymentId,
});

export const buildServerPrefs = (profiles: ConnectionProfile[], activeId: string): ServerPrefs => ({
  connection: {
    active_profile_id: activeId,
    profiles: profiles.map(toServerProfile),
  },
});

export const mergeServerPrefs = (
  prefs: ServerPrefs,
  localProfiles: ConnectionProfile[],
  defaults: AgentUIDefaults,
): { profiles: ConnectionProfile[]; activeProfileId: string } => {
  const incoming = Array.isArray(prefs?.connection?.profiles) ? prefs.connection.profiles : [];
  if (incoming.length === 0) {
    return { profiles: localProfiles, activeProfileId: localProfiles[0]?.id || "" };
  }
  const localById = new Map(localProfiles.map((p) => [p.id, p]));
  const merged = incoming.map((p) => {
    const local = localById.get(p.id);
    return normalizeProfile(
      {
        ...p,
        brokerAuthToken: local?.brokerAuthToken,
        daemonAuthToken: local?.daemonAuthToken,
        runOverridesEnabled: local?.runOverridesEnabled,
        runOverrides: local?.runOverrides,
      },
      defaults,
    );
  });
  const desiredActive = typeof prefs?.connection?.active_profile_id === "string" ? prefs.connection.active_profile_id : "";
  const activeProfileId = merged.some((p) => p.id === desiredActive) ? desiredActive : merged[0]?.id || "";
  return { profiles: merged, activeProfileId };
};
