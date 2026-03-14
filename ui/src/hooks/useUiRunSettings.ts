import React from "react";

import useUiRunExecutionSettings from "./useUiRunExecutionSettings";
import useUiRunMemorySettings from "./useUiRunMemorySettings";
import type { RunSettings } from "./uiSettingsTypes";
import type { ConnectionProfile, RunProfileOverrides } from "./uiSettingsProfiles";
import type { RunValueSetter } from "./uiRunSettingsShared";

type UseUiRunSettingsParams = {
  defaults: Parameters<typeof useUiRunExecutionSettings>[0]["defaults"];
  activeProfile: ConnectionProfile;
  updateActiveProfile: (update: (prev: ConnectionProfile) => ConnectionProfile) => void;
};

export default function useUiRunSettings({
  defaults,
  activeProfile,
  updateActiveProfile,
}: UseUiRunSettingsParams): RunSettings {
  const runOverridesEnabled = !!activeProfile.runOverridesEnabled;
  const runOverrides = activeProfile.runOverrides || {};

  const setRunValue = React.useCallback<RunValueSetter>(
    (key, next, globalValue, setGlobal) => {
      if (!runOverridesEnabled) {
        setGlobal(next);
        return;
      }
      updateActiveProfile((prev) => {
        const prevOverrides = (prev.runOverrides || {}) as RunProfileOverrides;
        const hasPrev = Object.prototype.hasOwnProperty.call(prevOverrides, key);
        const prevValue = hasPrev ? (prevOverrides as Record<string, unknown>)[key as string] : globalValue;
        const nextValue = typeof next === "function" ? (next as (value: unknown) => unknown)(prevValue) : next;
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

  const execution = useUiRunExecutionSettings({
    defaults,
    runOverridesEnabled,
    runOverrides,
    setRunValue,
  });

  const memory = useUiRunMemorySettings({
    defaults,
    runOverridesEnabled,
    runOverrides,
    setRunValue,
  });

  const snapshotRunOverrides = React.useMemo<RunProfileOverrides>(
    () => ({
      ...execution.snapshotOverrides,
      ...memory.snapshotOverrides,
    }),
    [execution.snapshotOverrides, memory.snapshotOverrides],
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
          prev.runOverrides && Object.keys(prev.runOverrides).length > 0 ? prev.runOverrides : snapshotRunOverrides;
        return { ...prev, runOverridesEnabled: true, runOverrides: seed };
      });
    },
    [snapshotRunOverrides, updateActiveProfile],
  );

  const copyProfileOverridesFromGlobal = React.useCallback(() => {
    updateActiveProfile((prev) => ({
      ...prev,
      runOverridesEnabled: true,
      runOverrides: snapshotRunOverrides,
    }));
  }, [snapshotRunOverrides, updateActiveProfile]);

  const clearProfileOverrides = React.useCallback(() => {
    updateActiveProfile((prev) => {
      if (!prev.runOverridesEnabled && !prev.runOverrides) return prev;
      return { ...prev, runOverridesEnabled: false, runOverrides: undefined };
    });
  }, [updateActiveProfile]);

  return {
    profileOverridesEnabled: runOverridesEnabled,
    setProfileOverridesEnabled,
    copyProfileOverridesFromGlobal,
    clearProfileOverrides,
    ...execution,
    ...memory,
  };
}
