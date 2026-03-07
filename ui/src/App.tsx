import React from "react";
import { useQuery } from "@tanstack/react-query";
import {
  apiGetCaps,
  RunResponse,
  type Caps,
  type AgentEvent,
} from "./api";
import { loadJson } from "./jsonUtils";
import { buildScopedSessionKey, buildSessionScopeKey } from "./sessionScope";
import { brokerBaseFromProxy } from "./utils/brokerBase";
import HistoryPanel from "./components/HistoryPanel";
import SceneView from "./components/SceneView";
import PromptBar, { type Attachment } from "./components/PromptBar";
import SettingsDrawer from "./components/SettingsDrawer";
import BrokerTeamConsole from "./components/broker/BrokerTeamConsole";
import AppConnectionBanner from "./components/app/AppConnectionBanner";
import AppAdvancedPanel from "./components/app/AppAdvancedPanel";
import AppHeader from "./components/app/AppHeader";
import AppToolsSidebar from "./components/app/AppToolsSidebar";
import TeamHubCard from "./components/app/TeamHubCard";
import useLocalStorageState from "./hooks/useLocalStorageState";
import useAppDataPlane from "./hooks/useAppDataPlane";
import useJobStreaming from "./hooks/useJobStreaming";
import useRunExecution, { type QueuedRun } from "./hooks/useRunExecution";
import useRuntimePlane from "./hooks/useRuntimePlane";
import useTeamChatOrchestration from "./hooks/useTeamChatOrchestration";
import useTraceLookup from "./hooks/useTraceLookup";
import useUiSettings from "./hooks/useUiSettings";
import { buildWorkflowDefaults } from "./workflowDefaults";

export default function App() {
  const ui = useUiSettings();
  const { connection, run: runSettings, client: clientSettings } = ui;
  const { brokerPanelOpen } = ui;
  const { effectiveBase, effectiveSseBase, daemonAuth, authKey } = connection;
  const profileName = connection.profileName;
  const profileId = connection.activeProfileId;
  const workflowTargets = ui.defaults.workflowAgentTargets;
  const workflowBearerEnv = ui.defaults.workflowBearerEnv;
  const { showSettings, setShowSettings } = ui;
  const connectionMode = connection.mode;
  const brokerAuthToken = connection.brokerAuthToken;
  const daemonAuthToken = connection.daemonAuthToken;
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
  }, [chatTarget, teamAction, setTeamAction]);
  const [prompt, setPrompt] = useLocalStorageState("agentui.prompt", "");
  const [capsCache, setCapsCache] = useLocalStorageState<Record<string, { caps: Caps; ts: number }>>(
    "agentui.capsByBase",
    {},
  );

  const webOrigin = React.useMemo(() => {
    try {
      // Used for actionable connectivity hints (CORS).
      return typeof window !== "undefined" ? String(window.location.origin || "") : "";
    } catch {
      return "";
    }
  }, []);

  const capsKey = React.useMemo(() => `${effectiveBase}::${authKey}`, [effectiveBase, authKey]);
  const capsQuery = useQuery({
    queryKey: ["caps", effectiveBase, authKey],
    queryFn: () => apiGetCaps(effectiveBase, daemonAuth),
    retry: 1,
    staleTime: 30_000,
    enabled: !!effectiveBase,
  });
  React.useEffect(() => {
    if (!capsKey) return;
    if (!capsQuery.data || !capsQuery.data.ok) return;
    setCapsCache((prev) => ({ ...prev, [capsKey]: { caps: capsQuery.data as Caps, ts: Date.now() } }));
  }, [capsKey, capsQuery.data, setCapsCache]);

  const cachedCaps = capsKey ? capsCache?.[capsKey] : undefined;
  const capsData = capsQuery.data && capsQuery.data.ok ? capsQuery.data : cachedCaps?.caps;
  const capsSource = capsQuery.data && capsQuery.data.ok ? "live" : cachedCaps ? "cache" : "none";
  const capsUpdatedMs = (capsQuery.data && capsQuery.data.ok ? capsQuery.data.now_unix_ms : undefined) ?? cachedCaps?.ts;
  const capsError = capsQuery.isError ? String(capsQuery.error) : null;

  const clientId = clientSettings.clientId;
  const clientInstanceIdRef = React.useRef<string>(
    (() => {
      try {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const g: any = typeof globalThis !== "undefined" ? globalThis : {};
        if (g.crypto && typeof g.crypto.randomUUID === "function") {
          return `tab-${g.crypto.randomUUID()}`;
        }
      } catch {
        // ignore
      }
      return `tab-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    })(),
  );
  const client = React.useMemo(
    () => ({ id: String(clientId || "webui"), kind: "webui", instance_id: clientInstanceIdRef.current }),
    [clientId],
  );
  const {
    tools,
    yolo,
    hostPolicy,
    automationProfile,
    verbose,
    model,
    summaryModel,
    summaryMaxChars,
    baseUrl,
    apiKey,
    proxyUrl,
    timeoutMs,
    maxCaptureBytes,
    streamAssistant,
    maxSteps,
    maxRepeatedToolCalls,
    maxToolCallsTotal,
    maxToolCallsPerTool,
    toolCallLimits,
    maxChars,
    keepLast,
    memoryContextMode,
    memoryIncludeStructured,
    memoryIncludeCore,
    memoryIncludeDaily,
    memoryIncludeSession,
    memoryDailyDays,
    memoryTotalCap,
    memorySearchQuery,
    memorySearchOrder,
    memorySearchUseIndex,
    memorySearchCaseSensitive,
    memorySearchFallbackToFiles,
    memorySearchMaxResults,
    memorySearchMaxSnippetChars,
    memorySearchContextLines,
    trace,
    useAsync,
  } = runSettings;
  const automationProfileTrim = automationProfile.trim();
  const automationProfileValue =
    automationProfileTrim === "full" ||
    automationProfileTrim === "guided" ||
    automationProfileTrim === "strict" ||
    automationProfileTrim === "custom"
      ? automationProfileTrim
      : undefined;
  const workflowDefaults = buildWorkflowDefaults(runSettings);
  const jobsEnabled = (capsData as any)?.features?.jobs?.enabled !== false;
  const uploadMaxBytesRaw = (capsData as any)?.limits?.upload_max_bytes;
  const uploadMaxBytes = typeof uploadMaxBytesRaw === "number" && Number.isFinite(uploadMaxBytesRaw) ? uploadMaxBytesRaw : undefined;
  const uploadsEnabled = uploadMaxBytes === undefined ? true : uploadMaxBytes > 0;
  const effectiveUseAsync = useAsync && jobsEnabled;
  React.useEffect(() => {
    if (!jobsEnabled && useAsync) {
      runSettings.setUseAsync(false);
    }
  }, [jobsEnabled, runSettings, useAsync]);
  const {
    showDebugInConversation,
    allowAutoplay,
    allowClientRpcs,
    allowClientEffects,
    allowUnsafePageEval,
  } = clientSettings;
  const [traceLookupId, setTraceLookupId] = useLocalStorageState("agentui.traceLookupId", "");
  // Keep prompts separate so an active async run does not overwrite the "last completed" view.
  const [lastRunPrompt, setLastRunPrompt] = React.useState("");
  const [lastCompletedPrompt, setLastCompletedPrompt] = React.useState("");
  const lastRunPromptRef = React.useRef<string>("");

  const [activeJobId, setActiveJobId] = React.useState<string | null>(null);
  const [jobStatus, setJobStatus] = React.useState<string | null>(null);
  const [jobError, setJobError] = React.useState<string | null>(null);
  // Non-fatal job transport/status messages (e.g. SSE dropped, polling hiccups).
  // Keep separate from `jobError` so the UI does not claim the job failed when only the connection failed.
  const [jobNotice, setJobNotice] = React.useState<string | null>(null);
  const [jobUpdatedMs, setJobUpdatedMs] = React.useState<number | null>(null);

  const [liveEvents, setLiveEvents] = React.useState<AgentEvent[]>([]);
  const cursorRef = React.useRef<number>(0);

  const lastHeartbeat = React.useMemo(() => {
    for (let i = liveEvents.length - 1; i >= 0; i--) {
      const ev: any = liveEvents[i];
      if (ev && typeof ev === "object" && ev.type === "heartbeat") {
        return ev.data ?? null;
      }
    }
    return null;
  }, [liveEvents]);

  const jobProgressLabel = React.useMemo(() => {
    const hb: any = lastHeartbeat && typeof lastHeartbeat === "object" ? lastHeartbeat : null;
    const phase = hb && typeof hb.phase === "number" ? hb.phase : null;
    if (phase === 1) return "waiting_llm";
    if (phase === 2) return "running_tool";
    if (phase === 0) return "idle";
    return "";
  }, [lastHeartbeat]);
  const runWatchPrefsBase = React.useMemo(() => {
    const base = String(effectiveBase || "").trim();
    if (!base) return "";
    return daemonAuth.mode === "broker" ? brokerBaseFromProxy(base) : base;
  }, [daemonAuth.mode, effectiveBase]);
  const runWatchPrefsClientId = React.useMemo(() => String(clientId || "webui"), [clientId]);
  const runWatchCanUse = React.useMemo(() => {
    if (!runWatchPrefsBase || !runWatchPrefsClientId) return false;
    if (daemonAuth.mode !== "broker") return true;
    return connection.brokerCookieAuth || String(brokerAuthToken || "").trim().length > 0;
  }, [brokerAuthToken, connection.brokerCookieAuth, daemonAuth.mode, runWatchPrefsBase, runWatchPrefsClientId]);
  const topbarRef = React.useRef<HTMLElement | null>(null);
  const promptbarRef = React.useRef<HTMLDivElement | null>(null);
  const [topbarHeightPx, setTopbarHeightPx] = React.useState<number>(56);
  const [promptbarHeightPx, setPromptbarHeightPx] = React.useState<number>(220);
  const sessionScopeKey = React.useMemo(
    () =>
      buildSessionScopeKey({
        profileId,
        base: effectiveBase,
        mode: connection.mode,
        deploymentId: connection.brokerDeploymentId,
      }),
    [connection.brokerDeploymentId, connection.mode, effectiveBase, profileId],
  );
  // Session selection is scoped by profile id (or base URL as fallback), with deployment id included when set.
  const [sessionByScopeJson, setSessionByScopeJson] = useLocalStorageState("agentui.sessionByScope", "{}");
  const [sessionByBaseJson] = useLocalStorageState("agentui.sessionByBase", "{}");
  const sessionId = React.useMemo(() => {
    const m = (loadJson(sessionByScopeJson) as Record<string, any>) || {};
    const sid = typeof m?.[sessionScopeKey] === "string" ? String(m[sessionScopeKey]) : "";
    return sid.trim().length > 0 ? sid.trim() : "default";
  }, [sessionByScopeJson, sessionScopeKey]);

  const [advancedPage, setAdvancedPage] = useLocalStorageState<string>(
    `agentui.advancedPage:${sessionScopeKey}`,
    "",
  );
  const legacyTraceLookupCheckedRef = React.useRef<boolean>(false);
  const legacyWorkflowPanelCheckedRef = React.useRef<boolean>(false);
  const [toolsCollapsed, setToolsCollapsed] = useLocalStorageState<boolean>(
    `agentui.toolsCollapsed:${sessionScopeKey}`,
    false,
  );
  const advancedPages = React.useMemo(() => {
    const pages = [
      { id: "memory", label: "Memory" },
      { id: "trace", label: "Trace" },
      { id: "workflows", label: "Workflows" },
      { id: "approvals", label: "Approvals" },
      { id: "run-diff", label: "Run Diff" },
    ];
    if (connectionMode === "broker") pages.push({ id: "broker", label: "Broker Console" });
    return pages;
  }, [connectionMode]);
  const advancedPageIds = React.useMemo(() => new Set(advancedPages.map((p) => p.id)), [advancedPages]);
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
  const [runQueue, setRunQueue] = useLocalStorageState<QueuedRun[]>(runQueueKey, []);
  const [showAllHistoryEntries, setShowAllHistoryEntries] = useLocalStorageState<boolean>(`${historyUiKey}:showAll`, false);
  const [showHistoryMessages, setShowHistoryMessages] = useLocalStorageState<boolean>(`${historyUiKey}:showMessages`, false);
  const [historyExpandedByKey, setHistoryExpandedByKey] = useLocalStorageState<Record<string, boolean>>(
    `${historyUiKey}:expandedByKey`,
    {},
  );
  const [sceneCollapsed, setSceneCollapsed] = useLocalStorageState<boolean>(`${historyUiKey}:sceneCollapsed`, false);
  const [focusAdvancedPanel, setFocusAdvancedPanel] = useLocalStorageState<boolean>("agentui.focusAdvancedPanel", false);
  const [inlineTeamSetupOpen, setInlineTeamSetupOpen] = useLocalStorageState<boolean>(
    "agentui.inlineTeamSetupOpen",
    false,
  );
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
  const setSessionId = React.useCallback(
    (sid: string) => {
      const nextSid = String(sid || "").trim() || "default";
      setSessionByScopeJson((prevRaw) => {
        const prev = (loadJson(String(prevRaw || "")) as Record<string, any>) || {};
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
  const jobStoreKey = scopedSessionKey;

  // Migration: older UI versions stored sessions by base URL or a single global session id.
  // If present, use them as initial values for the current scope.
  React.useEffect(() => {
    if (typeof window === "undefined" || !window.localStorage) return;
    const scopeMap = (loadJson(String(sessionByScopeJson || "")) as Record<string, any>) || {};
    const have = typeof scopeMap?.[sessionScopeKey] === "string" && String(scopeMap[sessionScopeKey]).trim().length > 0;
    if (have) return;

    const baseMap = (loadJson(String(sessionByBaseJson || "")) as Record<string, any>) || {};
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
      const v = JSON.parse(raw);
      if (typeof v === "string") legacy = v;
    } catch {
      // ignore
    }
    const legacyTrim = String(legacy || "").trim();
    if (!legacyTrim) return;
    setSessionId(legacyTrim);
  }, [effectiveBase, sessionByBaseJson, sessionByScopeJson, sessionScopeKey, setSessionId]);

  // Keep layout CSS vars in sync with actual measured bars (so Scene can truly fill the viewport).
  React.useLayoutEffect(() => {
    const measure = () => {
      try {
        const th = topbarRef.current?.getBoundingClientRect().height;
        const ph = promptbarRef.current?.getBoundingClientRect().height;
        if (typeof th === "number" && Number.isFinite(th) && th > 0) setTopbarHeightPx(Math.round(th));
        if (typeof ph === "number" && Number.isFinite(ph) && ph > 0) setPromptbarHeightPx(Math.round(ph));
      } catch {
        // ignore
      }
    };

    measure();

    let ro: ResizeObserver | null = null;
    if (typeof ResizeObserver !== "undefined") {
      ro = new ResizeObserver(() => measure());
      if (topbarRef.current) ro.observe(topbarRef.current);
      if (promptbarRef.current) ro.observe(promptbarRef.current);
    }

    window.addEventListener("resize", measure);
    return () => {
      window.removeEventListener("resize", measure);
      try {
        ro?.disconnect();
      } catch {
        // ignore
      }
    };
  }, []);

  const onMainScroll = React.useCallback(() => {
    const el = mainScrollRef.current;
    if (!el) return;
    if (mainScrollSaveRafRef.current) return;
    mainScrollSaveRafRef.current = requestAnimationFrame(() => {
      mainScrollSaveRafRef.current = 0;
      const cur = el.scrollTop;
      if (!Number.isFinite(cur)) return;
      if (Math.abs(cur - mainScrollLastSavedRef.current) < 2) return;
      mainScrollLastSavedRef.current = cur;
      setMainScrollTop(cur);
    });
  }, [setMainScrollTop]);

  const [result, setResult] = React.useState<RunResponse | undefined>(undefined);
  // Incremented when a run successfully starts/completes; used to clear "next run only" UI state (e.g. attachments).
  const [composerTaskNonce, setComposerTaskNonce] = React.useState<number>(0);
  const lastChatTargetRef = React.useRef<string>("");
  React.useEffect(() => {
    if (lastChatTargetRef.current === chatTarget) return;
    lastChatTargetRef.current = chatTarget;
    setComposerTaskNonce((n) => n + 1);
  }, [chatTarget]);
  React.useEffect(() => {
    if (chatTarget === "team" && teamAction === "goal") {
      setComposerTaskNonce((n) => n + 1);
    }
  }, [chatTarget, teamAction]);

  const {
    audit, auditRefetch, clearAllSessions, clearAllSessionsError, clearDaemonApiKey, daemonConfig, dbClientEvents,
    dbMessages, dbRunDetailsById, dbRuns, dbUiActions, deleteSession, deleteSessionError, health, historyEntriesDesc,
    isLocalDaemonBase, missingBrokerAuthToken, missingDaemonAuthToken, newSession, saveDaemonApiKey, saveDaemonDefaults,
    sessionsRefetch, sessionsUnauthorized, sessionArtifacts, sessionList, sessionScene, updateDaemonDefaults,
  } = useAppDataPlane({
    activeJobId,
    allowClientEffects,
    allowClientRpcs,
    apiKey,
    authKey,
    baseUrl,
    brokerAuthToken,
    brokerCookieAuth: connection.brokerCookieAuth,
    connectionMode,
    daemonAuth,
    daemonAuthToken,
    effectiveBase,
    jobStatus,
    jobUpdatedMs,
    lastRunPrompt,
    lastRunPromptRef,
    liveEvents,
    model,
    proxyUrl,
    selectedSessionId: sessionId,
    setActiveJobId,
    setJobError,
    setJobStatus,
    setJobUpdatedMs,
    setLastCompletedPrompt,
    setLastRunPrompt,
    setLiveEvents,
    setPrompt,
    setResult,
    setSessionId,
    summaryMaxChars,
    summaryModel,
    timeoutMs,
    cursorRef,
  });

  const { applySceneOps, runWatchMode, sceneEntities, writeJobsBySession } = useRuntimePlane({
    activeJobId,
    allowClientEffects,
    allowClientRpcs,
    authKey,
    client,
    cursorRef,
    daemonAuth,
    effectiveBase,
    jobStoreKey,
    runWatchCanUse,
    runWatchPrefsBase,
    runWatchPrefsClientId,
    sessionId,
    sessionScopeKey,
    setActiveJobId,
    setJobError,
    setJobStatus,
    setJobUpdatedMs,
    setLiveEvents,
    yolo,
    dbClientEventsData: dbClientEvents.data,
    dbUiActionsData: dbUiActions.data,
    sessionArtifactsData: sessionArtifacts.data,
    sessionSceneData: sessionScene.data,
  });

  React.useEffect(() => {
    const el = mainScrollRef.current;
    if (!el) return;
    const restoreKey = `${effectiveBase}::${String(sessionId || "").trim()}`;
    if (mainScrollRestoredKeyRef.current === restoreKey) return;
    if (!audit.isSuccess && !audit.isError) return;
    mainScrollRestoredKeyRef.current = restoreKey;
    const target = typeof mainScrollTop === "number" && Number.isFinite(mainScrollTop) ? mainScrollTop : 0;
    if (target <= 0) return;
    try {
      requestAnimationFrame(() => {
        try {
          const maxTop = Math.max(0, el.scrollHeight - el.clientHeight);
          el.scrollTop = Math.min(target, maxTop);
        } catch {
          // ignore
        }
      });
    } catch {
      // ignore
    }
  }, [audit.isError, audit.isSuccess, effectiveBase, mainScrollTop, sessionId]);

  const {
    clearTeamQueue, handleTeamRunRequest, latestTeamRunCreatedMs, latestTeamRunId, openTeamPanel,
    teamConversationCacheUpdatedMs, teamConversationItems, teamConversationUsingCache, teamConversationWarnings,
    teamQueue, teamQueueCount, teamQueueNeedsRun, teamRecentActivity, teamRunCreate, teamStatus,
  } = useTeamChatOrchestration({
    authKey,
    brokerAgentId: String(connection.brokerAgentId || "").trim(),
    brokerBase: connection.brokerBase,
    brokerChatAvailable,
    connectionMode,
    daemonAuth,
    selectedTeamId: selectedTeamIdTrimmed,
    setAdvancedPage,
    setComposerTaskNonce,
    setJobNotice,
    setPrompt,
  });

  const { handleDirectRunRequest, run } = useRunExecution({
    activeJobId,
    apiKey,
    automationProfileValue,
    baseUrl,
    client,
    cursorRef,
    daemonAuth,
    effectiveBase,
    effectiveUseAsync,
    hostPolicy,
    jobStoreKey,
    keepLast,
    lastRunPromptRef,
    maxCaptureBytes,
    maxChars,
    maxRepeatedToolCalls,
    maxSteps,
    maxToolCallsPerTool,
    maxToolCallsTotal,
    memoryContextMode,
    memoryDailyDays,
    memoryIncludeCore,
    memoryIncludeDaily,
    memoryIncludeSession,
    memoryIncludeStructured,
    memorySearchCaseSensitive,
    memorySearchContextLines,
    memorySearchFallbackToFiles,
    memorySearchMaxResults,
    memorySearchMaxSnippetChars,
    memorySearchOrder,
    memorySearchQuery,
    memorySearchUseIndex,
    memoryTotalCap,
    model,
    proxyUrl,
    runQueue,
    sessionId,
    setActiveJobId,
    setComposerTaskNonce,
    setJobError,
    setJobNotice,
    setJobStatus,
    setJobUpdatedMs,
    setLastCompletedPrompt,
    setLastRunPrompt,
    setLiveEvents,
    setPrompt,
    setResult,
    setRunQueue,
    streamAssistant,
    summaryMaxChars,
    summaryModel,
    timeoutMs,
    toolCallLimits,
    tools,
    trace,
    verbose,
    writeJobsBySession,
    yolo,
    auditRefetch,
    sessionsRefetch,
  });

  const isTeamTarget = brokerChatAvailable && chatTarget === "team";
  const teamActionNormalized =
    teamAction === "guidance" || teamAction === "goal" ? (teamAction as "guidance" | "goal") : "run";
  const handleRunRequest = React.useCallback(
    async (vars: { prompt: string; attachments: Attachment[] }) => {
      if (isTeamTarget) {
        await handleTeamRunRequest(vars, teamActionNormalized);
        return;
      }
      await handleDirectRunRequest(vars);
    },
    [
      handleDirectRunRequest,
      handleTeamRunRequest,
      isTeamTarget,
      teamActionNormalized,
    ],
  );

  const { clearTraceLookup, traceLookup, traceLookupAgentd, traceLookupBroker, traceLookupError } = useTraceLookup({
    brokerBase: connection.brokerBase,
    connectionMode,
    daemonAuth,
    effectiveBase,
  });

  useJobStreaming({
    activeJobId,
    connectionMode,
    daemonAuth,
    daemonAuthToken,
    effectiveBase,
    effectiveSseBase,
    sessionId,
    jobStoreKey,
    cursorRef,
    lastRunPromptRef,
    setActiveJobId,
    setJobStatus,
    setJobError,
    setJobNotice,
    setJobUpdatedMs,
    setLiveEvents,
    setResult,
    setLastCompletedPrompt,
    writeJobsBySession,
    auditRefetch,
    sessionsRefetch,
  });

  const showMainColumn = !advancedPage || !focusAdvancedPanel;
  const advancedGridCols = focusAdvancedPanel
    ? "lg:grid-cols-[var(--tools-col)_minmax(0,1fr)]"
    : advancedPage === "broker"
      ? "lg:grid-cols-[var(--tools-col)_minmax(260px,1fr)_minmax(0,1.4fr)]"
      : "lg:grid-cols-[var(--tools-col)_minmax(0,1fr)_minmax(0,1fr)]";

  React.useEffect(() => {
    if (!advancedPage && focusAdvancedPanel) {
      setFocusAdvancedPanel(false);
    }
  }, [advancedPage, focusAdvancedPanel, setFocusAdvancedPanel]);

  return (
    <div
      className="h-screen w-full bg-slate-950 text-white"
      style={{
        ["--topbar-h" as any]: `${topbarHeightPx}px`,
        ["--promptbar-h" as any]: `${promptbarHeightPx}px`,
      }}
    >
      <AppHeader
        topbarRef={topbarRef}
        profiles={connection.profiles}
        activeProfileId={connection.activeProfileId}
        profileName={profileName}
        onProfileChange={connection.setActiveProfileId}
        runOverridesEnabled={runSettings.profileOverridesEnabled}
        effectiveBase={effectiveBase}
        healthState={health.isSuccess ? "ok" : health.isFetching ? "checking" : "offline"}
        healthService={health.data?.service}
        healthVersion={health.data?.version}
        onRecheck={() => health.refetch()}
        onShowSettings={() => setShowSettings(true)}
      />

      <AppConnectionBanner
        healthError={health.isError}
        sessionsUnauthorized={sessionsUnauthorized}
        connectionMode={connectionMode}
        missingBrokerAuthToken={missingBrokerAuthToken}
        brokerCookieAuth={connection.brokerCookieAuth}
        effectiveBase={effectiveBase}
        webOrigin={webOrigin}
        missingDaemonAuthToken={missingDaemonAuthToken}
        isLocalDaemonBase={isLocalDaemonBase}
        onShowSettings={() => setShowSettings(true)}
        onUseDevToken={() => connection.setDaemonAuthToken("dev-agentd-token")}
      />

      <main
        ref={(el) => {
          mainScrollRef.current = el;
        }}
        onScroll={onMainScroll}
        className="h-[calc(100vh-var(--topbar-h))] overflow-y-auto px-3 py-3 pb-[var(--promptbar-h)]"
      >
        <div className="mx-auto w-full max-w-none">
            <div
              className={`mt-4 grid gap-4 ${
                advancedPage
                  ? advancedGridCols
                  : "lg:grid-cols-[var(--tools-col)_minmax(0,1fr)]"
              }`}
              style={{
                ["--tools-col" as any]: toolsCollapsed ? "72px" : "minmax(140px,12vw)",
              }}
            >
              <AppToolsSidebar
                advancedPages={advancedPages}
                advancedPage={advancedPage}
                toolsCollapsed={toolsCollapsed}
                setToolsCollapsed={setToolsCollapsed}
                setAdvancedPage={setAdvancedPage}
              />
              {showMainColumn ? (
              <section className="min-w-0">
                <div
                  className={`rounded-lg border border-white/10 bg-black/20 p-3 ${
                    sceneCollapsed ? "" : "flex h-[calc(100vh-var(--topbar-h)-var(--promptbar-h)-56px)] min-h-0 flex-col"
                  }`}
                >
                  <div className="flex items-center justify-between gap-2">
                    <div className="text-sm font-semibold text-white/80">Scene</div>
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/70 hover:bg-black/40"
                      type="button"
                      onClick={() => setSceneCollapsed((prev) => !prev)}
                    >
                      {sceneCollapsed ? "Show scene" : "Collapse"}
                    </button>
                  </div>
                  {sceneCollapsed ? (
                    <div className="mt-2 text-xs text-white/50">Scene is hidden to save space.</div>
                  ) : (
                    <div className="mt-3 min-h-0 flex-1">
                      <SceneView
                        baseUrl={effectiveBase}
                        yolo={yolo}
                        allowAutoplay={allowAutoplay}
                        client={client}
                        daemonAuth={daemonAuth}
                        sessionId={sessionId}
                        entities={sceneEntities}
                        className="h-full"
                      />
                    </div>
                  )}
                </div>
                {brokerChatAvailable ? (
                  <TeamHubCard
                    selectedTeamId={selectedTeamIdTrimmed}
                    latestTeamRunId={latestTeamRunId}
                    teamStatus={teamStatus}
                    inlineTeamSetupOpen={inlineTeamSetupOpen}
                    onToggleInlineSetup={() => setInlineTeamSetupOpen((prev) => !prev)}
                    onViewChat={() => {
                      const el = document.getElementById("team-chat");
                      if (el) {
                        el.scrollIntoView({ behavior: "smooth", block: "start" });
                      }
                    }}
                    onOpenFullSetup={() => {
                      openTeamPanel("setup");
                      setFocusAdvancedPanel(true);
                    }}
                    onOpenMembers={() => openTeamPanel("members")}
                    onOpenRuns={() => openTeamPanel("run")}
                    onRunTeam={() => {
                      setChatTarget("team");
                      setTeamAction("run");
                      if (promptbarRef.current) {
                        promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                      }
                    }}
                    onSendGuidance={() => {
                      setChatTarget("team");
                      setTeamAction("guidance");
                      if (promptbarRef.current) {
                        promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                      }
                    }}
                    onSetGoal={() => {
                      setChatTarget("team");
                      setTeamAction("goal");
                      if (promptbarRef.current) {
                        promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                      }
                    }}
                    teamConversationUsingCache={teamConversationUsingCache}
                    teamConversationCacheUpdatedMs={teamConversationCacheUpdatedMs}
                    teamQueueCount={teamQueueCount}
                    teamQueue={teamQueue}
                    teamQueueNeedsRun={teamQueueNeedsRun}
                    onClearQueue={clearTeamQueue}
                    onStartQueuedRun={() => {
                      setChatTarget("team");
                      setTeamAction("run");
                      if (promptbarRef.current) {
                        promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                      }
                    }}
                    recentActivity={teamRecentActivity}
                  />
                ) : null}
                {brokerChatAvailable && inlineTeamSetupOpen && advancedPage !== "broker" ? (
                  <details
                    className="mt-3 rounded-lg border border-white/10 bg-black/20 p-3"
                    open={inlineTeamSetupOpen}
                    onToggle={(event) =>
                      setInlineTeamSetupOpen((event.currentTarget as HTMLDetailsElement).open)
                    }
                  >
                    <summary className="cursor-pointer select-none text-xs font-semibold text-white/80">
                      Inline team setup
                    </summary>
                    <div className="mt-2 text-[11px] text-white/60">
                      Configure the team without leaving the chat. Attachments and prompts below are shared once you run.
                    </div>
                    <div className="mt-3">
                      <BrokerTeamConsole
                        mode="inline"
                        forcedTab="setup"
                        base={connection.brokerBase}
                        auth={daemonAuth}
                        authKey={authKey}
                        clientId={client.id}
                      />
                    </div>
                  </details>
                ) : null}
                <div className="mt-4">
                  <HistoryPanel
                    entries={historyEntriesDesc}
                    showAllEntries={showAllHistoryEntries}
                    setShowAllEntries={setShowAllHistoryEntries}
                    showMessages={showHistoryMessages}
                    setShowMessages={setShowHistoryMessages}
                    historyExpandedByKey={historyExpandedByKey}
                    setHistoryExpandedByKey={setHistoryExpandedByKey}
                    dbMessages={dbMessages.data?.ok && Array.isArray(dbMessages.data?.messages) ? dbMessages.data.messages : []}
                    dbRuns={dbRuns.data?.ok && Array.isArray(dbRuns.data?.runs) ? dbRuns.data.runs : []}
                    dbRunDetailsById={dbRunDetailsById}
                    sessionArtifacts={
                      sessionArtifacts.data?.ok && Array.isArray(sessionArtifacts.data?.artifacts)
                        ? sessionArtifacts.data.artifacts
                        : []
                    }
                    effectiveBase={effectiveBase}
                    yolo={yolo}
                    sessionId={sessionId}
                    client={client}
                    daemonAuth={daemonAuth}
                    showDebugInConversation={showDebugInConversation}
                    allowAutoplay={allowAutoplay}
                    allowClientRpcs={allowClientRpcs}
                    allowClientEffects={allowClientEffects}
                    allowUnsafePageEval={allowUnsafePageEval}
                    sceneEntities={sceneEntities}
                    onSceneApply={(ops) => applySceneOps(String(sessionId || "").trim(), ops)}
                    onTraceIdClick={(traceId) => {
                      setTraceLookupId(traceId);
                      setAdvancedPage("trace");
                      void traceLookup.mutateAsync(traceId).catch(() => {});
                    }}
                    teamConversationItems={teamConversationItems}
                    teamId={selectedTeamIdTrimmed}
                    teamRunId={latestTeamRunId}
                    teamRunCreatedMs={latestTeamRunCreatedMs}
                    teamRunStatus={teamStatus}
                    teamConversationWarnings={teamConversationWarnings}
                  />
                </div>
              </section>
              ) : null}
              <AppAdvancedPanel
                advancedPage={advancedPage}
                focusAdvancedPanel={focusAdvancedPanel}
                setFocusAdvancedPanel={setFocusAdvancedPanel}
                setAdvancedPage={setAdvancedPage}
                tracePanel={{
                  traceId: traceLookupId,
                  onTraceIdChange: setTraceLookupId,
                  onLoad: (id) => void traceLookup.mutateAsync(id).catch(() => {}),
                  onClear: clearTraceLookup,
                  loading: traceLookup.isPending,
                  error: traceLookupError,
                  connectionMode,
                  baseUrl: effectiveBase,
                  yolo,
                  agentdTrace: traceLookupAgentd,
                  brokerTrace: traceLookupBroker,
                }}
                runDiffPanel={{ baseUrl: effectiveBase, auth: daemonAuth }}
                memoryPanel={{ baseUrl: effectiveBase, auth: daemonAuth }}
                approvalsPanel={{ baseUrl: effectiveBase, auth: daemonAuth }}
                workflowPanel={{
                  baseUrl: effectiveBase,
                  auth: daemonAuth,
                  authKey,
                  clientId: client.id,
                  workflowDefaults,
                  workflowTargets,
                  workflowBearerEnv,
                  onTraceIdClick: (traceId) => {
                    setTraceLookupId(traceId);
                    setAdvancedPage("trace");
                    void traceLookup.mutateAsync(traceId).catch(() => {});
                  },
                }}
                brokerPanel={{
                  enabled: connectionMode === "broker",
                  brokerBase: connection.brokerBase,
                  brokerAgentId: connection.brokerAgentId,
                  setBrokerAgentId: connection.setBrokerAgentId,
                  auth: daemonAuth,
                  authKey,
                  clientId: client.id,
                }}
              />
          </div>
        </div>
      </main>

      <PromptBar
        ref={promptbarRef}
        effectiveBase={effectiveBase}
        sessionId={sessionId}
        tools={tools}
        activeJobId={activeJobId}
        jobStatus={jobStatus}
        jobProgressLabel={jobProgressLabel}
        runWatchMode={runWatchMode}
        daemonAuth={daemonAuth}
        prompt={prompt}
        setPrompt={setPrompt}
        runDisabled={isTeamTarget ? !brokerChatAvailable : false}
        runLabel={
          isTeamTarget
            ? teamRunCreate.isPending
              ? "Sending…"
              : teamActionNormalized === "goal"
                ? "Update goal"
                : teamActionNormalized === "guidance"
                  ? "Send guidance"
                  : "Run team"
            : run.isPending || activeJobId
              ? "Queue"
              : "Run"
        }
        queueCount={
          isTeamTarget ? (Array.isArray(teamQueue) ? teamQueue.length : 0) : Array.isArray(runQueue) ? runQueue.length : 0
        }
        onRun={handleRunRequest}
        setJobNotice={setJobNotice}
        jobNotice={jobNotice}
        jobError={jobError}
        runError={
          isTeamTarget ? (teamRunCreate.isError ? String(teamRunCreate.error) : null) : run.isError ? String(run.error) : null
        }
        resultError={!result?.ok && result?.error ? result.error : null}
        clearAttachmentsNonce={composerTaskNonce}
        uploadsEnabled={uploadsEnabled && (!isTeamTarget || teamActionNormalized !== "goal")}
        uploadMaxBytes={uploadMaxBytes}
        uploadsDisabledReason={
          isTeamTarget ? (teamActionNormalized === "goal" ? "Goal updates do not accept attachments." : "") : ""
        }
        chatTarget={isTeamTarget ? "team" : "session"}
        teamId={selectedTeamIdTrimmed}
        teamAvailable={brokerChatAvailable}
        onChatTargetChange={(next) => setChatTarget(next)}
        uploadMode={isTeamTarget ? "team" : "session"}
        teamAction={teamActionNormalized}
        onTeamActionChange={(next) => setTeamAction(next)}
      />

      <SettingsDrawer
        open={showSettings}
        onClose={() => setShowSettings(false)}
        connection={connection}
        run={runSettings}
        client={clientSettings}
        session={{
          id: sessionId,
          setId: setSessionId,
          sessions: sessionList,
          refresh: () => sessionsRefetch(),
          newSession: () => newSession.mutate(),
          newSessionPending: newSession.isPending,
          deleteSession: (sid) => void deleteSession.mutateAsync(sid).catch(() => {}),
          deletePending: deleteSession.isPending,
          deleteError: deleteSessionError,
          clearAll: () => void clearAllSessions.mutateAsync().catch(() => {}),
          clearAllPending: clearAllSessions.isPending,
          clearAllError: clearAllSessionsError,
        }}
        daemonConfig={{
          data: daemonConfig.data,
          isFetching: daemonConfig.isFetching,
          refresh: () => daemonConfig.refetch(),
        }}
        updateDaemonDefaults={{
          pending: updateDaemonDefaults.isPending,
          error: updateDaemonDefaults.isError ? String(updateDaemonDefaults.error) : null,
          success: updateDaemonDefaults.isSuccess,
          saveDefaults: saveDaemonDefaults,
          saveApiKey: saveDaemonApiKey,
          clearApiKey: clearDaemonApiKey,
        }}
        caps={{
          data: capsData,
          source: capsSource,
          updatedMs: capsUpdatedMs,
          isFetching: capsQuery.isFetching,
          error: capsError,
          refresh: () => void capsQuery.refetch(),
        }}
      />
    </div>
  );
}
