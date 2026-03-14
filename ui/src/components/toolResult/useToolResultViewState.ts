import React from "react";
import useLocalStorageState from "../../hooks/useLocalStorageState";
import type { ToolResultUiPrefs } from "./toolResultUtils";

type UseToolResultViewStateArgs = {
  baseUrl: string;
  sessionId?: string;
  toolCallId?: string;
};

export function useToolResultViewState(args: UseToolResultViewStateArgs) {
  const { baseUrl, sessionId, toolCallId } = args;
  const prefsKey = React.useMemo(() => {
    const b = String(baseUrl || "").trim();
    const sid = String(sessionId || "").trim();
    return `agentui.toolResultPrefs:${b}::${sid}`;
  }, [baseUrl, sessionId]);
  const [prefsByToolCallId, setPrefsByToolCallId] = useLocalStorageState<Record<string, ToolResultUiPrefs>>(prefsKey, {});

  const tcid = String(toolCallId || "").trim();
  const canPersist = tcid.length > 0;
  const [volatileShowRaw, setVolatileShowRaw] = React.useState<boolean>(false);
  const [volatileShowFullOutput, setVolatileShowFullOutput] = React.useState<boolean>(false);
  const [volatileRenderMode, setVolatileRenderMode] = React.useState<"auto" | "text" | "markdown">("auto");

  const prefs: ToolResultUiPrefs =
    (tcid && prefsByToolCallId && typeof prefsByToolCallId === "object" ? prefsByToolCallId[tcid] : null) || {};
  const showRaw = canPersist ? !!prefs.showRaw : volatileShowRaw;
  const showFullOutput = canPersist ? !!prefs.showFullOutput : volatileShowFullOutput;
  const renderMode: "auto" | "text" | "markdown" =
    canPersist
      ? prefs.renderMode === "text" || prefs.renderMode === "markdown"
        ? prefs.renderMode
        : "auto"
      : volatileRenderMode;

  const setPrefs = React.useCallback(
    (patch: Partial<ToolResultUiPrefs>) => {
      if (!tcid) return;
      setPrefsByToolCallId((prev) => {
        const base = prev && typeof prev === "object" ? prev : {};
        const next: Record<string, ToolResultUiPrefs> = { ...base, [tcid]: { ...(base[tcid] || {}), ...patch } };
        const keys = Object.keys(next);
        if (keys.length > 200) {
          keys.sort();
          for (let i = 0; i < keys.length - 200; i += 1) delete next[keys[i]];
        }
        return next;
      });
    },
    [setPrefsByToolCallId, tcid],
  );

  const setShowRaw = React.useCallback(
    (value: boolean) => {
      if (canPersist) setPrefs({ showRaw: value });
      else setVolatileShowRaw(value);
    },
    [canPersist, setPrefs],
  );
  const setShowFullOutput = React.useCallback(
    (value: boolean) => {
      if (canPersist) setPrefs({ showFullOutput: value });
      else setVolatileShowFullOutput(value);
    },
    [canPersist, setPrefs],
  );
  const setRenderMode = React.useCallback(
    (value: "auto" | "text" | "markdown") => {
      if (canPersist) setPrefs({ renderMode: value });
      else setVolatileRenderMode(value);
    },
    [canPersist, setPrefs],
  );

  return {
    renderMode,
    setRenderMode,
    setShowFullOutput,
    setShowRaw,
    showFullOutput,
    showRaw,
  };
}
