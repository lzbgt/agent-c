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
  brokerAuthToken: string;
  setBrokerAuthToken: React.Dispatch<React.SetStateAction<string>>;
  daemonAuthToken: string;
  setDaemonAuthToken: React.Dispatch<React.SetStateAction<string>>;
  effectiveBase: string;
  effectiveSseBase: string;
  daemonAuth: ApiAuth;
  authKey: string;
};

export type ConnectionProfile = {
  id: string;
  name: string;
  mode: ConnectionMode;
  base: string;
  brokerBase: string;
  brokerAgentId: string;
  brokerAuthToken: string;
  daemonAuthToken: string;
};

export type RunSettings = {
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
    if (agent) return `broker:${agent}`;
    return `broker:${hostLabel(p.brokerBase, "https://127.0.0.1:8443", "https")}`;
  }
  return `daemon:${hostLabel(p.base, "http://127.0.0.1:8123", "http")}`;
};

const normalizeProfile = (p: Partial<ConnectionProfile>, defaults: AgentUIDefaults): ConnectionProfile => {
  const mode = p.mode === "broker" ? "broker" : "direct";
  const base = typeof p.base === "string" ? p.base : defaults.daemonBaseUrl;
  const brokerBase = typeof p.brokerBase === "string" ? p.brokerBase : defaults.brokerBaseUrl;
  const brokerAgentId = typeof p.brokerAgentId === "string" ? p.brokerAgentId : defaults.brokerAgentId;
  const brokerAuthToken = typeof p.brokerAuthToken === "string" ? p.brokerAuthToken : defaults.brokerAuthToken;
  const daemonAuthToken = typeof p.daemonAuthToken === "string" ? p.daemonAuthToken : defaults.daemonAuthToken;
  const id = typeof p.id === "string" && p.id.trim() ? p.id : generateProfileId();
  const name = typeof p.name === "string" ? p.name.trim() : "";
  const out: ConnectionProfile = {
    id,
    name: name || "",
    mode,
    base,
    brokerBase,
    brokerAgentId,
    brokerAuthToken,
    daemonAuthToken,
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
      if (
        np.id !== p.id ||
        np.name !== p.name ||
        np.mode !== p.mode ||
        np.base !== p.base ||
        np.brokerBase !== p.brokerBase ||
        np.brokerAgentId !== p.brokerAgentId ||
        np.brokerAuthToken !== p.brokerAuthToken ||
        np.daemonAuthToken !== p.daemonAuthToken
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

  const [tools, setTools] = useLocalStorageState<ToolMode>("agentui.tools", defaults.tools);
  const [yolo, setYolo] = useLocalStorageState("agentui.yolo", defaults.yolo);
  const [hostPolicy, setHostPolicy] = useLocalStorageState<HostPolicy>("agentui.hostPolicy", defaults.hostPolicy);
  const [verbose, setVerbose] = useLocalStorageState("agentui.verbose", defaults.verbose);
  const [model, setModel] = useLocalStorageState("agentui.model", defaults.model);
  const [summaryModel, setSummaryModel] = useLocalStorageState("agentui.summaryModel", "");
  const [summaryMaxChars, setSummaryMaxChars] = useLocalStorageState("agentui.summaryMaxChars", "1200");
  const [baseUrl, setBaseUrl] = useLocalStorageState("agentui.baseUrl", defaults.baseUrl);
  const [apiKey, setApiKey] = useLocalStorageState("agentui.apiKey", defaults.apiKey);
  const [proxyUrl, setProxyUrl] = useLocalStorageState("agentui.proxyUrl", defaults.proxyUrl);
  const [timeoutMs, setTimeoutMs] = useLocalStorageState("agentui.timeoutMs", defaults.timeoutMs);
  const [maxCaptureBytes, setMaxCaptureBytes] = useLocalStorageState("agentui.maxCaptureBytes", "65536");
  const [streamAssistant, setStreamAssistant] = useLocalStorageState("agentui.streamAssistant", defaults.streamAssistant);
  const [trace, setTrace] = useLocalStorageState("agentui.trace", defaults.trace);
  const [useAsync, setUseAsync] = useLocalStorageState("agentui.useAsync", defaults.useAsync);

  const [maxStepsRaw, setMaxStepsRaw] = useLocalStorageState("agentui.maxSteps", "");
  const [maxStepsUserSet, setMaxStepsUserSet] = useLocalStorageState("agentui.maxStepsUserSet", false);
  const [maxRepeatedToolCalls, setMaxRepeatedToolCalls] = useLocalStorageState("agentui.maxRepeatedToolCalls", "0");
  const [maxToolCallsTotal, setMaxToolCallsTotal] = useLocalStorageState("agentui.maxToolCallsTotal", "");
  const [maxToolCallsPerTool, setMaxToolCallsPerTool] = useLocalStorageState("agentui.maxToolCallsPerTool", "");
  const [toolCallLimits, setToolCallLimits] = useLocalStorageState("agentui.toolCallLimits", "");
  const [maxChars, setMaxChars] = useLocalStorageState("agentui.maxChars", "20000");
  const [keepLast, setKeepLast] = useLocalStorageState("agentui.keepLast", "16");

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

  const connectionMode = activeProfile.mode;
  const base = activeProfile.base;
  const brokerBase = activeProfile.brokerBase;
  const brokerAgentId = activeProfile.brokerAgentId;
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
      return { mode: "broker", token: brokerAuthToken, agentdToken: daemonAuthToken };
    }
    return { mode: "direct", token: daemonAuthToken };
  }, [brokerAuthToken, connectionMode, daemonAuthToken]);

  const authKey = React.useMemo(() => {
    const mode = daemonAuth.mode;
    const t = typeof daemonAuth.token === "string" ? daemonAuth.token.trim() : "";
    const at = daemonAuth.mode === "broker" && typeof daemonAuth.agentdToken === "string" ? daemonAuth.agentdToken.trim() : "";
    const pid = String(activeProfileId || "default");
    return mode === "broker"
      ? `broker:pid=${pid}:tlen=${t.length}:alen=${at.length}`
      : `direct:pid=${pid}:tlen=${t.length}`;
  }, [activeProfileId, daemonAuth]);

  const setMaxSteps = React.useCallback<React.Dispatch<React.SetStateAction<string>>>(
    (next) => {
      setMaxStepsUserSet(true);
      setMaxStepsRaw((prev) => (typeof next === "function" ? next(prev) : next));
    },
    [setMaxStepsRaw, setMaxStepsUserSet],
  );

  React.useEffect(() => {
    if (maxStepsUserSet) return;
    if (String(maxStepsRaw) === "0") {
      setMaxStepsRaw("");
    }
  }, [maxStepsRaw, maxStepsUserSet, setMaxStepsRaw]);

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
      maxSteps: maxStepsRaw,
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
