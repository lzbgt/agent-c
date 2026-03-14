import React from "react";

import useLocalStorageState from "./useLocalStorageState";
import { resolveOverrideValue, type RunValueSetter } from "./uiRunSettingsShared";
import type { AgentUIDefaults } from "../runtime_config";
import type { RunSettings } from "./uiSettingsTypes";
import type { RunProfileOverrides } from "./uiSettingsProfiles";

type UseUiRunMemorySettingsParams = {
  defaults: AgentUIDefaults;
  runOverridesEnabled: boolean;
  runOverrides: RunProfileOverrides;
  setRunValue: RunValueSetter;
};

type RunMemorySettingsSubsetKeys =
  | "memoryContextMode"
  | "setMemoryContextMode"
  | "memoryIncludeStructured"
  | "setMemoryIncludeStructured"
  | "memoryIncludeCore"
  | "setMemoryIncludeCore"
  | "memoryIncludeDaily"
  | "setMemoryIncludeDaily"
  | "memoryIncludeSession"
  | "setMemoryIncludeSession"
  | "memoryDailyDays"
  | "setMemoryDailyDays"
  | "memoryTotalCap"
  | "setMemoryTotalCap"
  | "memorySearchQuery"
  | "setMemorySearchQuery"
  | "memorySearchOrder"
  | "setMemorySearchOrder"
  | "memorySearchUseIndex"
  | "setMemorySearchUseIndex"
  | "memorySearchCaseSensitive"
  | "setMemorySearchCaseSensitive"
  | "memorySearchFallbackToFiles"
  | "setMemorySearchFallbackToFiles"
  | "memorySearchMaxResults"
  | "setMemorySearchMaxResults"
  | "memorySearchMaxSnippetChars"
  | "setMemorySearchMaxSnippetChars"
  | "memorySearchContextLines"
  | "setMemorySearchContextLines";

export type RunMemorySettingsSubset = Pick<RunSettings, RunMemorySettingsSubsetKeys>;

export type UseUiRunMemorySettingsResult = RunMemorySettingsSubset & {
  snapshotOverrides: RunProfileOverrides;
};

export default function useUiRunMemorySettings({
  defaults,
  runOverridesEnabled,
  runOverrides,
  setRunValue,
}: UseUiRunMemorySettingsParams): UseUiRunMemorySettingsResult {
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

  const memoryContextMode = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryContextMode", memoryContextModeGlobal);
  const memoryIncludeStructured = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memoryIncludeStructured",
    memoryIncludeStructuredGlobal,
  );
  const memoryIncludeCore = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryIncludeCore", memoryIncludeCoreGlobal);
  const memoryIncludeDaily = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memoryIncludeDaily",
    memoryIncludeDailyGlobal,
  );
  const memoryIncludeSession = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memoryIncludeSession",
    memoryIncludeSessionGlobal,
  );
  const memoryDailyDays = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryDailyDays", memoryDailyDaysGlobal);
  const memoryTotalCap = resolveOverrideValue(runOverridesEnabled, runOverrides, "memoryTotalCap", memoryTotalCapGlobal);
  const memorySearchQuery = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchQuery", memorySearchQueryGlobal);
  const memorySearchOrder = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchOrder", memorySearchOrderGlobal);
  const memorySearchUseIndex = resolveOverrideValue(runOverridesEnabled, runOverrides, "memorySearchUseIndex", memorySearchUseIndexGlobal);
  const memorySearchCaseSensitive = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memorySearchCaseSensitive",
    memorySearchCaseSensitiveGlobal,
  );
  const memorySearchFallbackToFiles = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memorySearchFallbackToFiles",
    memorySearchFallbackToFilesGlobal,
  );
  const memorySearchMaxResults = resolveOverrideValue(
    runOverridesEnabled,
    runOverrides,
    "memorySearchMaxResults",
    memorySearchMaxResultsGlobal,
  );
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

  const snapshotOverrides = React.useMemo<RunProfileOverrides>(
    () => ({
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
      memoryContextModeGlobal,
      memoryIncludeStructuredGlobal,
      memoryIncludeCoreGlobal,
      memoryIncludeDailyGlobal,
      memoryIncludeSessionGlobal,
      memoryDailyDaysGlobal,
      memoryTotalCapGlobal,
      memorySearchQueryGlobal,
      memorySearchOrderGlobal,
      memorySearchUseIndexGlobal,
      memorySearchCaseSensitiveGlobal,
      memorySearchFallbackToFilesGlobal,
      memorySearchMaxResultsGlobal,
      memorySearchMaxSnippetCharsGlobal,
      memorySearchContextLinesGlobal,
    ],
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

  return {
    snapshotOverrides,
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
  };
}
