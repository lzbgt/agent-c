import React from "react";
import useLocalStorageState from "./useLocalStorageState";
import useSessionStorageState from "./useSessionStorageState";
import type { AgentUIDefaults, HostPolicy, ToolMode } from "../runtime_config";
import type { RunSettings } from "./uiSettingsTypes";
import {
  normalizeSecretMap,
  readLegacySecretString,
  type ConnectionProfile,
  type ConnectionProfileSecretMap,
  type RunProfileOverrides,
} from "./uiSettingsProfiles";

type UseUiRunSettingsParams = {
  defaults: AgentUIDefaults;
  activeProfile: ConnectionProfile;
  updateActiveProfile: (update: (prev: ConnectionProfile) => ConnectionProfile) => void;
};

const resolveOverrideValue = <T,>(runOverridesEnabled: boolean, runOverrides: RunProfileOverrides, key: keyof RunProfileOverrides, globalValue: T): T => {
  if (!runOverridesEnabled) return globalValue;
  if (Object.prototype.hasOwnProperty.call(runOverrides, key)) {
    return (runOverrides as Record<string, T>)[key as string];
  }
  return globalValue;
};

export default function useUiRunSettings({
  defaults,
  activeProfile,
  updateActiveProfile,
}: UseUiRunSettingsParams): RunSettings {
  const initialApiKey = React.useMemo(() => readLegacySecretString("agentui.apiKey", defaults.apiKey), [defaults.apiKey]);

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
        const nextValue = typeof next === "function" ? (next as (value: T) => T)(prevValue) : next;
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
  const maxRepeatedToolCalls = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxRepeatedToolCalls", maxRepeatedToolCallsGlobal);
  const maxToolCallsTotal = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxToolCallsTotal", maxToolCallsTotalGlobal);
  const maxToolCallsPerTool = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxToolCallsPerTool", maxToolCallsPerToolGlobal);
  const toolCallLimits = resolveOverrideValue(runOverridesEnabled, runOverrides, "toolCallLimits", toolCallLimitsGlobal);
  const maxChars = resolveOverrideValue(runOverridesEnabled, runOverrides, "maxChars", maxCharsGlobal);
  const keepLast = resolveOverrideValue(runOverridesEnabled, runOverrides, "keepLast", keepLastGlobal);
  const memoryContextMode = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryContextMode", memoryContextModeGlobal);
  const memoryIncludeStructured = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryIncludeStructured", memoryIncludeStructuredGlobal);
  const memoryIncludeCore = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryIncludeCore", memoryIncludeCoreGlobal);
  const memoryIncludeDaily = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryIncludeDaily", memoryIncludeDailyGlobal);
  const memoryIncludeSession = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryIncludeSession", memoryIncludeSessionGlobal);
  const memoryDailyDays = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryDailyDays", memoryDailyDaysGlobal);
  const memoryTotalCap = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryTotalCap", memoryTotalCapGlobal);
  const memorySearchQuery = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchQuery", memorySearchQueryGlobal);
  const memorySearchOrder = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchOrder", memorySearchOrderGlobal);
  const memorySearchUseIndex = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchUseIndex", memorySearchUseIndexGlobal);
  const memorySearchCaseSensitive = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchCaseSensitive", memorySearchCaseSensitiveGlobal);
  const memorySearchFallbackToFiles = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memorySearchFallbackToFiles",
    memorySearchFallbackToFilesGlobal,
  );
  const memorySearchMaxResults = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchMaxResults", memorySearchMaxResultsGlobal);
  const memorySearchMaxSnippetChars = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memorySearchMaxSnippetChars",
    memorySearchMaxSnippetCharsGlobal,
  );
  const memorySearchContextLines = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memorySearchContextLines",
    memorySearchContextLinesGlobal,
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

  React.useEffect(() => {
    if (maxStepsUserSet) return;
    if (String(maxStepsRawGlobal) === "0") {
      setMaxStepsRawGlobal("");
    }
  }, [maxStepsRawGlobal, maxStepsUserSet, setMaxStepsRawGlobal]);

  return {
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
  };
}
