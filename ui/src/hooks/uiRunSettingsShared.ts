import type React from "react";

import type { RunProfileOverrides } from "./uiSettingsProfiles";

export type RunValueSetter = <T>(
  key: keyof RunProfileOverrides,
  next: React.SetStateAction<T>,
  globalValue: T,
  setGlobal: React.Dispatch<React.SetStateAction<T>>,
) => void;

export const resolveOverrideValue = <T,>(
  runOverridesEnabled: boolean,
  runOverrides: RunProfileOverrides,
  key: keyof RunProfileOverrides,
  globalValue: T,
): T => {
  if (!runOverridesEnabled) return globalValue;
  if (Object.prototype.hasOwnProperty.call(runOverrides, key)) {
    return (runOverrides as Record<string, T>)[key as string];
  }
  return globalValue;
};
