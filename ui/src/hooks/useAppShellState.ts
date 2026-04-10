import React from "react";
import useLocalStorageState from "./useLocalStorageState";
import { loadJson } from "../jsonUtils";
import { buildScopedSessionKey, buildSessionScopeKey } from "../sessionScope";

type UseAppShellStateArgs = {
  profileId: string;
  effectiveBase: string;
  connectionMode: string;
  brokerDeploymentId: string;
  brokerPanelOpen: boolean;
};

export default function useAppShellState({
  profileId,
  effectiveBase,
  connectionMode,
  brokerDeploymentId,
  brokerPanelOpen,
}: UseAppShellStateArgs) {
  const [selectedTeamId, setSelectedTeamId] = useLocalStorageState<string>("agentui.brokerTeamId", "");
  const selectedTeamIdTrimmed = String(selectedTeamId || "").trim();
  const brokerChatAvailable = connectionMode === "broker" && selectedTeamIdTrimmed.length > 0;
  const [chatTarget, setChatTarget] = useLocalStorageState<string>(
    "agentui.chatTarget",
    brokerChatAvailable ? "team" : "session",
  );
  React.useEffect(() => {
    if (connectionMode !== "broker" && chatTarget !== "session") {
      setChatTarget("session");
    }
  }, [chatTarget, connectionMode, setChatTarget]);
  React.useEffect(() => {
    if (chatTarget === "team" && !brokerChatAvailable) {
      setChatTarget("session");
    }
  }, [brokerChatAvailable, chatTarget, setChatTarget]);
  const [teamAction, setTeamAction] = useLocalStorageState<string>("agentui.teamAction", "run");
  React.useEffect(() => {
    if (chatTarget !== "team" && teamAction !== "run") {
      setTeamAction("run");
    }
  }, [chatTarget, setTeamAction, teamAction]);

  const topbarRef = React.useRef<HTMLElement | null>(null);
  const promptbarRef = React.useRef<HTMLDivElement | null>(null);
  const [topbarHeightPx, setTopbarHeightPx] = React.useState<number>(56);
  const [promptbarHeightPx, setPromptbarHeightPx] = React.useState<number>(220);

  const sessionScopeKey = React.useMemo(
    () =>
      buildSessionScopeKey({
        profileId,
        base: effectiveBase,
        mode: connectionMode,
        deploymentId: brokerDeploymentId,
      }),
    [brokerDeploymentId, connectionMode, effectiveBase, profileId],
  );
  const [sessionByScopeJson, setSessionByScopeJson] = useLocalStorageState("agentui.sessionByScope", "{}");
  const [sessionByBaseJson] = useLocalStorageState("agentui.sessionByBase", "{}");
  const sessionId = React.useMemo(() => {
    const map = loadJson(sessionByScopeJson) ?? {};
    const scopedValue = map[sessionScopeKey];
    const scoped = typeof scopedValue === "string" ? scopedValue : "";
    return scoped.trim().length > 0 ? scoped.trim() : "default";
  }, [sessionByScopeJson, sessionScopeKey]);
  const [sessionLeaseSeconds, setSessionLeaseSeconds] = useLocalStorageState<string>("agentui.sessionLeaseSeconds", "90");
  const setSessionId = React.useCallback(
    (sid: string) => {
      const nextSid = String(sid || "").trim() || "default";
      setSessionByScopeJson((prevRaw) => {
        const prev = loadJson(String(prevRaw || "")) ?? {};
        const next = { ...prev, [sessionScopeKey]: nextSid };
        try {
          return JSON.stringify(next);
        } catch {
          return JSON.stringify(prev);
        }
      });
    },
    [sessionScopeKey, setSessionByScopeJson],
  );
  const scopedSessionKey = React.useMemo(
    () => buildScopedSessionKey(sessionScopeKey, sessionId),
    [sessionId, sessionScopeKey],
  );

  React.useEffect(() => {
    if (typeof window === "undefined" || !window.localStorage) return;
    const scopeMap = loadJson(String(sessionByScopeJson || "")) ?? {};
    const have =
      typeof scopeMap?.[sessionScopeKey] === "string" && String(scopeMap[sessionScopeKey]).trim().length > 0;
    if (have) return;

    const baseMap = loadJson(String(sessionByBaseJson || "")) ?? {};
    const legacyBase = typeof baseMap?.[effectiveBase] === "string" ? String(baseMap[effectiveBase]) : "";
    const legacyBaseTrim = legacyBase.trim();
    if (legacyBaseTrim) {
      setSessionId(legacyBaseTrim);
      return;
    }

    const raw = window.localStorage.getItem("agentui.sessionId");
    if (!raw) return;
    let legacy = "";
    try {
      const value = JSON.parse(raw);
      if (typeof value === "string") legacy = value;
    } catch {
      // ignore
    }
    const legacyTrim = String(legacy || "").trim();
    if (!legacyTrim) return;
    setSessionId(legacyTrim);
  }, [effectiveBase, sessionByBaseJson, sessionByScopeJson, sessionScopeKey, setSessionId]);

  const advancedPages = React.useMemo(() => {
    const pages = [
      { id: "memory", label: "Memory" },
      { id: "voice", label: "Voice" },
      { id: "trace", label: "Trace" },
      { id: "workflows", label: "Workflows" },
      { id: "approvals", label: "Approvals" },
      { id: "run-diff", label: "Run Diff" },
    ];
    if (connectionMode === "broker") pages.push({ id: "broker", label: "Broker Console" });
    return pages;
  }, [connectionMode]);
  const advancedPageIds = React.useMemo(() => new Set(advancedPages.map((page) => page.id)), [advancedPages]);
  const [advancedPage, setAdvancedPage] = useLocalStorageState<string>(`agentui.advancedPage:${sessionScopeKey}`, "");
  const legacyTraceLookupCheckedRef = React.useRef<boolean>(false);
  const legacyWorkflowPanelCheckedRef = React.useRef<boolean>(false);
  const [toolsCollapsed, setToolsCollapsed] = useLocalStorageState<boolean>(
    `agentui.toolsCollapsed:${sessionScopeKey}`,
    false,
  );
  React.useEffect(() => {
    if (advancedPage && !advancedPageIds.has(advancedPage)) setAdvancedPage("");
  }, [advancedPage, advancedPageIds, setAdvancedPage]);
  React.useEffect(() => {
    if (legacyTraceLookupCheckedRef.current) return;
    legacyTraceLookupCheckedRef.current = true;
    try {
      const raw = window.localStorage.getItem("agentui.traceLookupOpen");
      if (raw === "true" || raw === "1") setAdvancedPage("trace");
    } catch {
      // ignore
    }
  }, [setAdvancedPage]);
  React.useEffect(() => {
    if (legacyWorkflowPanelCheckedRef.current) return;
    legacyWorkflowPanelCheckedRef.current = true;
    try {
      const raw = window.localStorage.getItem("agentui.workflowPanelOpen");
      if (raw === "true" || raw === "1") setAdvancedPage("workflows");
    } catch {
      // ignore
    }
  }, [setAdvancedPage]);
  React.useEffect(() => {
    if (!brokerPanelOpen || connectionMode !== "broker") return;
    if (advancedPage !== "broker") setAdvancedPage("broker");
  }, [advancedPage, brokerPanelOpen, connectionMode, setAdvancedPage]);

  const historyUiKey = React.useMemo(() => {
    const sid = String(sessionId || "").trim();
    return `agentui.historyUi:${sessionScopeKey}::${sid}`;
  }, [sessionId, sessionScopeKey]);
  const runQueueKey = React.useMemo(() => {
    const baseKey = String(effectiveBase || "").trim() || "default";
    const sidKey = String(sessionId || "").trim() || "default";
    return `agentui.runQueue:${baseKey}::${sidKey}`;
  }, [effectiveBase, sessionId]);
  const [showAllHistoryEntries, setShowAllHistoryEntries] = useLocalStorageState<boolean>(`${historyUiKey}:showAll`, false);
  const [showHistoryMessages, setShowHistoryMessages] = useLocalStorageState<boolean>(`${historyUiKey}:showMessages`, false);
  const [historyExpandedByKey, setHistoryExpandedByKey] = useLocalStorageState<Record<string, boolean>>(
    `${historyUiKey}:expandedByKey`,
    {},
  );
  const [sceneCollapsed, setSceneCollapsed] = useLocalStorageState<boolean>(`${historyUiKey}:sceneCollapsed`, false);
  const [focusAdvancedPanel, setFocusAdvancedPanel] = useLocalStorageState<boolean>("agentui.focusAdvancedPanel", false);
  const [inlineTeamSetupOpen, setInlineTeamSetupOpen] = useLocalStorageState<boolean>("agentui.inlineTeamSetupOpen", false);

  const [mainScrollTop, setMainScrollTop] = useLocalStorageState<number>(`${historyUiKey}:scrollTop`, 0);
  const mainScrollRef = React.useRef<HTMLElement | null>(null);
  const mainScrollRestoredKeyRef = React.useRef<string>("");
  const mainScrollSaveRafRef = React.useRef<number>(0);
  const mainScrollLastSavedRef = React.useRef<number>(-1);

  React.useEffect(() => {
    return () => {
      if (mainScrollSaveRafRef.current) {
        try {
          cancelAnimationFrame(mainScrollSaveRafRef.current);
        } catch {
          // ignore
        }
      }
      mainScrollSaveRafRef.current = 0;
    };
  }, []);

  React.useLayoutEffect(() => {
    const measure = () => {
      try {
        const topbarHeight = topbarRef.current?.getBoundingClientRect().height;
        const promptbarHeight = promptbarRef.current?.getBoundingClientRect().height;
        if (typeof topbarHeight === "number" && Number.isFinite(topbarHeight) && topbarHeight > 0) {
          setTopbarHeightPx(Math.round(topbarHeight));
        }
        if (typeof promptbarHeight === "number" && Number.isFinite(promptbarHeight) && promptbarHeight > 0) {
          setPromptbarHeightPx(Math.round(promptbarHeight));
        }
      } catch {
        // ignore
      }
    };

    measure();

    let observer: ResizeObserver | null = null;
    if (typeof ResizeObserver !== "undefined") {
      observer = new ResizeObserver(() => measure());
      if (topbarRef.current) observer.observe(topbarRef.current);
      if (promptbarRef.current) observer.observe(promptbarRef.current);
    }

    window.addEventListener("resize", measure);
    return () => {
      window.removeEventListener("resize", measure);
      try {
        observer?.disconnect();
      } catch {
        // ignore
      }
    };
  }, []);

  const onMainScroll = React.useCallback(() => {
    const element = mainScrollRef.current;
    if (!element) return;
    if (mainScrollSaveRafRef.current) return;
    mainScrollSaveRafRef.current = requestAnimationFrame(() => {
      mainScrollSaveRafRef.current = 0;
      const current = element.scrollTop;
      if (!Number.isFinite(current)) return;
      if (Math.abs(current - mainScrollLastSavedRef.current) < 2) return;
      mainScrollLastSavedRef.current = current;
      setMainScrollTop(current);
    });
  }, [setMainScrollTop]);

  return {
    selectedTeamId,
    setSelectedTeamId,
    selectedTeamIdTrimmed,
    brokerChatAvailable,
    chatTarget,
    setChatTarget,
    teamAction,
    setTeamAction,
    topbarRef,
    promptbarRef,
    topbarHeightPx,
    promptbarHeightPx,
    sessionScopeKey,
    sessionId,
    setSessionId,
    sessionLeaseSeconds,
    setSessionLeaseSeconds,
    scopedSessionKey,
    advancedPages,
    advancedPage,
    setAdvancedPage,
    toolsCollapsed,
    setToolsCollapsed,
    runQueueKey,
    showAllHistoryEntries,
    setShowAllHistoryEntries,
    showHistoryMessages,
    setShowHistoryMessages,
    historyExpandedByKey,
    setHistoryExpandedByKey,
    sceneCollapsed,
    setSceneCollapsed,
    focusAdvancedPanel,
    setFocusAdvancedPanel,
    inlineTeamSetupOpen,
    setInlineTeamSetupOpen,
    mainScrollTop,
    setMainScrollTop,
    mainScrollRef,
    mainScrollRestoredKeyRef,
    onMainScroll,
  };
}
