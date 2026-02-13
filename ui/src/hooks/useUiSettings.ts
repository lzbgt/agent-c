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

export default function useUiSettings(): UiSettings {
  const defaults = React.useMemo(() => getUiDefaults(), []);

  const [showSettings, setShowSettings] = useLocalStorageState("agentui.showSettings", false);

  const [connectionMode, setConnectionMode] = useLocalStorageState<ConnectionMode>(
    "agentui.connectionMode",
    defaults.connectionMode,
  );
  const [base, setBase] = useLocalStorageState("agentui.base", defaults.daemonBaseUrl);
  const [brokerBase, setBrokerBase] = useLocalStorageState("agentui.brokerBase", defaults.brokerBaseUrl);
  const [brokerAgentId, setBrokerAgentId] = useLocalStorageState("agentui.brokerAgentId", defaults.brokerAgentId);
  const [brokerAuthToken, setBrokerAuthToken] = useLocalStorageState("agentui.brokerAuthToken", defaults.brokerAuthToken);
  const [daemonAuthToken, setDaemonAuthToken] = useLocalStorageState("agentui.daemonAuthToken", defaults.daemonAuthToken);

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
    return mode === "broker" ? `broker:tlen=${t.length}:alen=${at.length}` : `direct:tlen=${t.length}`;
  }, [daemonAuth]);

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
      mode: connectionMode,
      setMode: setConnectionMode,
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
