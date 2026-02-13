export type ConnectionMode = "direct" | "broker";
export type ToolMode = "host" | "basic" | "none";
export type HostPolicy = "full" | "readonly";

export type AgentUIRuntimeConfig = {
  connectionMode?: ConnectionMode | string;
  daemonBaseUrl?: string;
  brokerBaseUrl?: string;
  brokerAgentId?: string;
  brokerAuthToken?: string;
  daemonAuthToken?: string;
  tools?: ToolMode | string;
  yolo?: boolean | string;
  hostPolicy?: HostPolicy | string;
  verbose?: boolean | string;
  model?: string;
  baseUrl?: string;
  apiKey?: string;
  proxyUrl?: string;
  timeoutMs?: string | number;
  streamAssistant?: boolean | string;
  trace?: boolean | string;
  useAsync?: boolean | string;
  allowClientRpcs?: boolean | string;
  allowClientEffects?: boolean | string;
  allowUnsafePageEval?: boolean | string;
  brokerPanelOpen?: boolean | string;
};

export type AgentUIDefaults = {
  connectionMode: ConnectionMode;
  daemonBaseUrl: string;
  brokerBaseUrl: string;
  brokerAgentId: string;
  brokerAuthToken: string;
  daemonAuthToken: string;
  tools: ToolMode;
  yolo: boolean;
  hostPolicy: HostPolicy;
  verbose: boolean;
  model: string;
  baseUrl: string;
  apiKey: string;
  proxyUrl: string;
  timeoutMs: string;
  streamAssistant: boolean;
  trace: boolean;
  useAsync: boolean;
  allowClientRpcs: boolean;
  allowClientEffects: boolean;
  allowUnsafePageEval: boolean;
  brokerPanelOpen: boolean;
};

declare global {
  interface Window {
    __AGENT_UI_CONFIG__?: AgentUIRuntimeConfig;
  }
}

const DEFAULTS: AgentUIDefaults = {
  connectionMode: "direct",
  daemonBaseUrl: "http://127.0.0.1:8123",
  brokerBaseUrl: "https://127.0.0.1:8443",
  brokerAgentId: "agent1",
  brokerAuthToken: "",
  daemonAuthToken: "",
  tools: "host",
  yolo: true,
  hostPolicy: "full",
  verbose: true,
  model: "deepseek-chat",
  baseUrl: "https://api.deepseek.com",
  apiKey: "",
  proxyUrl: "http://localhost:8120",
  timeoutMs: "60000",
  streamAssistant: false,
  trace: true,
  useAsync: true,
  allowClientRpcs: true,
  allowClientEffects: true,
  allowUnsafePageEval: true,
  brokerPanelOpen: false,
};

const env = (() => {
  try {
    // Vite injects env at build time.
    return (import.meta as any).env || {};
  } catch {
    return {};
  }
})();

const envString = (key: string): string | undefined => {
  const v = env[key];
  if (typeof v !== "string") return undefined;
  const s = v.trim();
  return s ? s : undefined;
};

const coerceBool = (val: unknown): boolean | undefined => {
  if (typeof val === "boolean") return val;
  if (typeof val === "string") {
    const s = val.trim().toLowerCase();
    if (!s) return undefined;
    if (s === "1" || s === "true" || s === "yes" || s === "on") return true;
    if (s === "0" || s === "false" || s === "no" || s === "off") return false;
  }
  return undefined;
};

const coerceString = (val: unknown): string | undefined => {
  if (val === null || val === undefined) return undefined;
  if (typeof val === "string") {
    const s = val.trim();
    return s ? s : undefined;
  }
  if (typeof val === "number" && Number.isFinite(val)) {
    return String(val);
  }
  return undefined;
};

const coerceEnum = <T extends string>(val: unknown, allowed: readonly T[]): T | undefined => {
  if (typeof val !== "string") return undefined;
  const s = val.trim();
  if (!s) return undefined;
  if ((allowed as readonly string[]).includes(s)) return s as T;
  return undefined;
};

const readRuntimeConfig = (): AgentUIRuntimeConfig => {
  if (typeof window === "undefined") return {};
  return (window.__AGENT_UI_CONFIG__ || {}) as AgentUIRuntimeConfig;
};

export const getUiDefaults = (): AgentUIDefaults => {
  const cfg = readRuntimeConfig();
  const out: AgentUIDefaults = { ...DEFAULTS };

  out.connectionMode =
    coerceEnum(cfg.connectionMode, ["direct", "broker"]) ??
    coerceEnum(envString("VITE_AGENTUI_CONNECTION_MODE"), ["direct", "broker"]) ??
    out.connectionMode;

  out.daemonBaseUrl =
    coerceString(cfg.daemonBaseUrl) ??
    coerceString(envString("VITE_AGENTD_BASE_URL")) ??
    out.daemonBaseUrl;

  out.brokerBaseUrl =
    coerceString(cfg.brokerBaseUrl) ??
    coerceString(envString("VITE_BROKER_BASE_URL")) ??
    out.brokerBaseUrl;

  out.brokerAgentId =
    coerceString(cfg.brokerAgentId) ??
    coerceString(envString("VITE_AGENTUI_BROKER_AGENT_ID")) ??
    out.brokerAgentId;

  out.brokerAuthToken =
    coerceString(cfg.brokerAuthToken) ??
    coerceString(envString("VITE_AGENTUI_BROKER_AUTH_TOKEN")) ??
    out.brokerAuthToken;

  out.daemonAuthToken =
    coerceString(cfg.daemonAuthToken) ??
    coerceString(envString("VITE_AGENTUI_DAEMON_AUTH_TOKEN")) ??
    out.daemonAuthToken;

  out.tools =
    coerceEnum(cfg.tools, ["host", "basic", "none"]) ??
    coerceEnum(envString("VITE_AGENTUI_TOOLS"), ["host", "basic", "none"]) ??
    out.tools;

  out.yolo = coerceBool(cfg.yolo) ?? coerceBool(envString("VITE_AGENTUI_YOLO")) ?? out.yolo;
  out.hostPolicy =
    coerceEnum(cfg.hostPolicy, ["full", "readonly"]) ??
    coerceEnum(envString("VITE_AGENTUI_HOST_POLICY"), ["full", "readonly"]) ??
    out.hostPolicy;

  out.verbose = coerceBool(cfg.verbose) ?? coerceBool(envString("VITE_AGENTUI_VERBOSE")) ?? out.verbose;
  out.model = coerceString(cfg.model) ?? coerceString(envString("VITE_AGENTUI_MODEL")) ?? out.model;
  out.baseUrl = coerceString(cfg.baseUrl) ?? coerceString(envString("VITE_AGENTUI_BASE_URL")) ?? out.baseUrl;
  out.apiKey = coerceString(cfg.apiKey) ?? coerceString(envString("VITE_AGENTUI_API_KEY")) ?? out.apiKey;
  out.proxyUrl = coerceString(cfg.proxyUrl) ?? coerceString(envString("VITE_AGENTUI_PROXY_URL")) ?? out.proxyUrl;
  out.timeoutMs = coerceString(cfg.timeoutMs) ?? coerceString(envString("VITE_AGENTUI_TIMEOUT_MS")) ?? out.timeoutMs;

  out.streamAssistant =
    coerceBool(cfg.streamAssistant) ??
    coerceBool(envString("VITE_AGENTUI_STREAM_ASSISTANT")) ??
    out.streamAssistant;

  out.trace = coerceBool(cfg.trace) ?? coerceBool(envString("VITE_AGENTUI_TRACE")) ?? out.trace;
  out.useAsync = coerceBool(cfg.useAsync) ?? coerceBool(envString("VITE_AGENTUI_USE_ASYNC")) ?? out.useAsync;

  out.allowClientRpcs =
    coerceBool(cfg.allowClientRpcs) ??
    coerceBool(envString("VITE_AGENTUI_ALLOW_CLIENT_RPCS")) ??
    out.allowClientRpcs;
  out.allowClientEffects =
    coerceBool(cfg.allowClientEffects) ??
    coerceBool(envString("VITE_AGENTUI_ALLOW_CLIENT_EFFECTS")) ??
    out.allowClientEffects;
  out.allowUnsafePageEval =
    coerceBool(cfg.allowUnsafePageEval) ??
    coerceBool(envString("VITE_AGENTUI_ALLOW_UNSAFE_PAGE_EVAL")) ??
    out.allowUnsafePageEval;
  out.brokerPanelOpen =
    coerceBool(cfg.brokerPanelOpen) ??
    coerceBool(envString("VITE_AGENTUI_BROKER_PANEL_OPEN")) ??
    out.brokerPanelOpen;

  return out;
};
