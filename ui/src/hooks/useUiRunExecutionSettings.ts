import React from "react";

import useLocalStorageState from "./useLocalStorageState";
import useSessionStorageState from "./useSessionStorageState";
import { resolveOverrideValue, type RunValueSetter } from "./uiRunSettingsShared";
import type { AgentUIDefaults, HostPolicy, ToolMode } from "../runtime_config";
import type { RunSettings } from "./uiSettingsTypes";
import { readLegacySecretString, type RunProfileOverrides } from "./uiSettingsProfiles";

type UseUiRunExecutionSettingsParams = {
  defaults: AgentUIDefaults;
  runOverridesEnabled: boolean;
  runOverrides: RunProfileOverrides;
  setRunValue: RunValueSetter;
};

type RunExecutionSettingsSubsetKeys =
  | "tools"
  | "setTools"
  | "yolo"
  | "setYolo"
  | "hostPolicy"
  | "setHostPolicy"
  | "automationProfile"
  | "setAutomationProfile"
  | "verbose"
  | "setVerbose"
  | "model"
  | "setModel"
  | "summaryModel"
  | "setSummaryModel"
  | "summaryMaxChars"
  | "setSummaryMaxChars"
  | "baseUrl"
  | "setBaseUrl"
  | "apiKey"
  | "setApiKey"
  | "proxyUrl"
  | "setProxyUrl"
  | "timeoutMs"
  | "setTimeoutMs"
  | "maxCaptureBytes"
  | "setMaxCaptureBytes"
  | "streamAssistant"
  | "setStreamAssistant"
  | "trace"
  | "setTrace"
  | "useAsync"
  | "setUseAsync"
  | "maxSteps"
  | "setMaxSteps"
  | "maxRepeatedToolCalls"
  | "setMaxRepeatedToolCalls"
  | "maxToolCallsTotal"
  | "setMaxToolCallsTotal"
  | "maxToolCallsPerTool"
  | "setMaxToolCallsPerTool"
  | "toolCallLimits"
  | "setToolCallLimits"
  | "maxChars"
  | "setMaxChars"
  | "keepLast"
  | "setKeepLast"
  | "orMinTotal"
  | "setOrMinTotal"
  | "orMaxTotal"
  | "setOrMaxTotal"
  | "orRequireMultimodal"
  | "setOrRequireMultimodal"
  | "orRequireTools"
  | "setOrRequireTools"
  | "orLimit"
  | "setOrLimit";

export type RunExecutionSettingsSubset = Pick<RunSettings, RunExecutionSettingsSubsetKeys>;

export type UseUiRunExecutionSettingsResult = RunExecutionSettingsSubset & {
  snapshotOverrides: RunProfileOverrides;
};

export default function useUiRunExecutionSettings({
  defaults,
  runOverridesEnabled,
  runOverrides,
  setRunValue,
}: UseUiRunExecutionSettingsParams): UseUiRunExecutionSettingsResult {
  const initialApiKey = React.useMemo(() => readLegacySecretString("agentui.apiKey", defaults.apiKey), [defaults.apiKey]);

  const [toolsGlobal, setToolsGlobal] = useLocalStorageState<ToolMode>("agentui.tools", defaults.tools);
  const [yoloGlobal, setYoloGlobal] = useLocalStorageState("agentui.yolo", defaults.yolo);
  const [hostPolicyGlobal, setHostPolicyGlobal] = useLocalStorageState<HostPolicy>("agentui.hostPolicy", defaults.hostPolicy);
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
  const [maxRepeatedToolCallsGlobal, setMaxRepeatedToolCallsGlobal] = useLocalStorageState("agentui.maxRepeatedToolCalls", "0");
  const [maxToolCallsTotalGlobal, setMaxToolCallsTotalGlobal] = useLocalStorageState("agentui.maxToolCallsTotal", "");
  const [maxToolCallsPerToolGlobal, setMaxToolCallsPerToolGlobal] = useLocalStorageState("agentui.maxToolCallsPerTool", "");
  const [toolCallLimitsGlobal, setToolCallLimitsGlobal] = useLocalStorageState("agentui.toolCallLimits", "");
  const [maxCharsGlobal, setMaxCharsGlobal] = useLocalStorageState("agentui.maxChars", "20000");
  const [keepLastGlobal, setKeepLastGlobal] = useLocalStorageState("agentui.keepLast", "16");
  const [orMinTotal, setOrMinTotal] = useLocalStorageState("agentui.orMinTotal", "0.01");
  const [orMaxTotal, setOrMaxTotal] = useLocalStorageState("agentui.orMaxTotal", "0.50");
  const [orRequireMultimodal, setOrRequireMultimodal] = useLocalStorageState("agentui.orRequireMultimodal", true);
  const [orRequireTools, setOrRequireTools] = useLocalStorageState("agentui.orRequireTools", true);
  const [orLimit, setOrLimit] = useLocalStorageState("agentui.orLimit", "50");

  const tools = resolveOverrideValue(runOverridesEnabled, runOverrides, "tools", toolsGlobal);
  const yolo = resolveOverrideValue(runOverridesEnabled, runOverrides, "yolo", yoloGlobal);
  const hostPolicy = resolveOverrideValue(runOverridesEnabled, runOverrides, "hostPolicy", hostPolicyGlobal);
  const automationProfile = resolveOverrideValue(runOverridesEnabled, runOverrides, "automationProfile", automationProfileGlobal);
  const verbose = resolveOverrideValue(runOverridesEnabled, runOverrides, "verbose", verboseGlobal);
  const model = resolveOverrideValue(runOverridesEnabled, runOverrides, "model", modelGlobal);
  const summaryModel = resolveOverrideValue(runOverridesEnabled, runOverrides, "summaryModel", summaryModelGlobal);
  const summaryMaxChars = resolveOverrideValue(runOverridesEnabled, runOverrides, "summaryMaxChars", summaryMaxCharsGlobal);
  const baseUrl = resolveOverrideValue(runOverridesEnabled, runOverrides, "baseUrl", baseUrlGlobal);
  const apiKey = resolveOverrideValue(runOverridesEnabled, runOverrides, "apiKey", apiKeyGlobal);
  const proxyUrl = resolveOverrideValue(runOverridesEnabled, runOverrides, "proxyUrl", proxyUrlGlobal);
  const timeoutMs = resolveOverrideValue(runOverridesEnabled, runOverrides, "timeoutMs", timeoutMsGlobal);
  const maxCaptureBytes = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxCaptureBytes", maxCaptureBytesGlobal);
  const streamAssistant = resolveOverrideValue(runOverridesEnabled, runOverrides, "streamAssistant", streamAssistantGlobal);
  const trace = resolveOverrideValue(runOverridesEnabled, runOverrides, "trace", traceGlobal);
  const useAsync = resolveOverrideValue(runOverridesEnabled, runOverrides, "useAsync", useAsyncGlobal);
  const maxSteps = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxSteps", maxStepsRawGlobal);
  const maxRepeatedToolCalls = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "maxRepeatedToolCalls",
    maxRepeatedToolCallsGlobal,
  );
  const maxToolCallsTotal = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxToolCallsTotal", maxToolCallsTotalGlobal);
  const maxToolCallsPerTool = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "maxToolCallsPerTool",
    maxToolCallsPerToolGlobal,
  );
  const toolCallLimits = resolveOverrideValue(runOverridesEnabled, runOverrides, "toolCallLimits", toolCallLimitsGlobal);
  const maxChars = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxChars", maxCharsGlobal);
  const keepLast = resolveOverrideValue(runOverridesEnabled, runOverrides, "keepLast", keepLastGlobal);

  const snapshotOverrides = React.useMemo<RunProfileOverrides>(
    () => ({
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

  React.useEffect(() => {
    if (maxStepsUserSet) return;
    if (String(maxStepsRawGlobal) === "0") {
      setMaxStepsRawGlobal("");
    }
  }, [maxStepsRawGlobal, maxStepsUserSet, setMaxStepsRawGlobal]);

  return {
    snapshotOverrides,
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
  };
}
