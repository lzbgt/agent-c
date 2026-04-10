import React from "react";
import { useQuery } from "@tanstack/react-query";
import {
  apiGetCaps,
  RunResponse,
  type Caps,
  type AgentEvent,
} from "./api";
import { safeObject } from "./jsonUtils";
import { brokerBaseFromProxy } from "./utils/brokerBase";
import HistoryPanel from "./components/HistoryPanel";
import SceneView from "./components/SceneView";
import PromptBar, { type Attachment } from "./components/PromptBar";
import SettingsDrawer from "./components/SettingsDrawer";
import BrokerTeamConsole from "./components/broker/BrokerTeamConsole";
import AppMainColumn from "./components/app/AppMainColumn";
import AppConnectionBanner from "./components/app/AppConnectionBanner";
import AppAdvancedPanel from "./components/app/AppAdvancedPanel";
import AppHeader from "./components/app/AppHeader";
import AppToolsSidebar from "./components/app/AppToolsSidebar";
import useLocalStorageState from "./hooks/useLocalStorageState";
import useAppDataPlane from "./hooks/useAppDataPlane";
import useAppShellState from "./hooks/useAppShellState";
import useJobStreaming from "./hooks/useJobStreaming";
import useRunExecution, { type QueuedRun } from "./hooks/useRunExecution";
import useRuntimePlane from "./hooks/useRuntimePlane";
import useSessionEventStreaming from "./hooks/useSessionEventStreaming";
import useTeamChatOrchestration from "./hooks/useTeamChatOrchestration";
import useTraceLookup from "./hooks/useTraceLookup";
import useUiSettings from "./hooks/useUiSettings";
import { buildWorkflowDefaults } from "./workflowDefaults";

type AppShellStyle = React.CSSProperties & {
  "--topbar-h": string;
  "--promptbar-h": string;
};

type AppGridStyle = React.CSSProperties & {
  "--tools-col": string;
};

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
  const [prompt, setPrompt] = useLocalStorageState("agentui.prompt", "");
  const [capsCache, setCapsCache] = useLocalStorageState<Record<string, { caps: Caps; ts: number }>>(
    "agentui.capsByBase",
    {},
  );
  const shell = useAppShellState({
    profileId,
    effectiveBase,
    connectionMode,
    brokerDeploymentId: connection.brokerDeploymentId,
    brokerPanelOpen,
  });
  const {
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
    mainScrollRef,
    mainScrollRestoredKeyRef,
    onMainScroll,
  } = shell;

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
      const cryptoApi = typeof globalThis === "object" ? globalThis.crypto : undefined;
      if (cryptoApi && typeof cryptoApi.randomUUID === "function") {
        return `tab-${cryptoApi.randomUUID()}`;
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
  const capsFeatures = safeObject(capsData?.features);
  const capsJobs = safeObject(capsFeatures.jobs);
  const capsLimits = safeObject(capsData?.limits);
  const jobsEnabled = capsJobs.enabled !== false;
  const uploadMaxBytesRaw = capsLimits.upload_max_bytes;
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
      const ev = liveEvents[i];
      if (ev.type === "heartbeat") {
        return ev.data ?? null;
      }
    }
    return null;
  }, [liveEvents]);

  const jobProgressLabel = React.useMemo(() => {
    const hb = safeObject(lastHeartbeat);
    const phase = typeof hb.phase === "number" ? hb.phase : null;
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
  const [runQueue, setRunQueue] = useLocalStorageState<QueuedRun[]>(runQueueKey, []);
  const jobStoreKey = scopedSessionKey;

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
    audit, auditRefetch, clearAllSessions, clearAllSessionsError, clearDaemonApiKey, daemonConfig, dbClientEventRows,
    dbMessageRows, dbRunDetailsById, dbRunRows, dbUiActionRows, deleteSession, deleteSessionError, health, historyEntriesDesc,
    isLocalDaemonBase, missingBrokerAuthToken, missingDaemonAuthToken, newSession, attachSession, attachSessionError,
    renewSessionAttachment, renewSessionAttachmentError, releaseSessionAttachment, releaseSessionAttachmentError,
    sessionInfoData, sessionLeaseConflict, setSessionLeaseConflict, saveDaemonApiKey, saveDaemonDefaults, sessionsRefetch,
    sessionsUnauthorized, sessionArtifactRows, sessionArtifactsUnsupported, sessionList, sessionSceneSnapshot, updateDaemonDefaults,
  } = useAppDataPlane({
    activeJobId,
    allowClientEffects,
    allowClientRpcs,
    apiKey,
    authKey,
    baseUrl,
    brokerAuthToken,
    brokerCookieAuth: connection.brokerCookieAuth,
    clientId,
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
    sessionLeaseSeconds,
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
    artifactCatalogSupported: !sessionArtifactsUnsupported,
    dbClientEventRows,
    dbUiActionRows,
    sessionArtifactRows,
    sessionSceneSnapshot,
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

  const sessionEventStream = useSessionEventStreaming({
    activeJobId,
    connectionMode,
    daemonAuth,
    effectiveBase,
    sessionId,
    sessionScopeKey,
    setLiveEvents,
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

  const appShellStyle: AppShellStyle = {
    "--topbar-h": `${topbarHeightPx}px`,
    "--promptbar-h": `${promptbarHeightPx}px`,
  };
  const appGridStyle: AppGridStyle = {
    "--tools-col": toolsCollapsed ? "72px" : "minmax(140px,12vw)",
  };

  return (
    <div className="h-screen w-full bg-slate-950 text-white" style={appShellStyle}>
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
              style={appGridStyle}
            >
              <AppToolsSidebar
                advancedPages={advancedPages}
                advancedPage={advancedPage}
                toolsCollapsed={toolsCollapsed}
                setToolsCollapsed={setToolsCollapsed}
                setAdvancedPage={setAdvancedPage}
              />
              {showMainColumn ? (
              <AppMainColumn
                sceneCollapsed={sceneCollapsed}
                setSceneCollapsed={setSceneCollapsed}
                effectiveBase={effectiveBase}
                yolo={yolo}
                allowAutoplay={allowAutoplay}
                client={client}
                daemonAuth={daemonAuth}
                sessionId={sessionId}
                sceneEntities={sceneEntities}
                onSceneApply={(ops) => applySceneOps(String(sessionId || "").trim(), ops)}
                brokerChatAvailable={brokerChatAvailable}
                teamHubProps={{
                  selectedTeamId: selectedTeamIdTrimmed,
                  latestTeamRunId,
                  teamStatus,
                  inlineTeamSetupOpen,
                  onToggleInlineSetup: () => setInlineTeamSetupOpen((prev) => !prev),
                  onViewChat: () => {
                    const el = document.getElementById("team-chat");
                    if (el) {
                      el.scrollIntoView({ behavior: "smooth", block: "start" });
                    }
                  },
                  onOpenFullSetup: () => {
                    openTeamPanel("setup");
                    setFocusAdvancedPanel(true);
                  },
                  onOpenMembers: () => openTeamPanel("members"),
                  onOpenRuns: () => openTeamPanel("run"),
                  onRunTeam: () => {
                    setChatTarget("team");
                    setTeamAction("run");
                    if (promptbarRef.current) {
                      promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                    }
                  },
                  onSendGuidance: () => {
                    setChatTarget("team");
                    setTeamAction("guidance");
                    if (promptbarRef.current) {
                      promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                    }
                  },
                  onSetGoal: () => {
                    setChatTarget("team");
                    setTeamAction("goal");
                    if (promptbarRef.current) {
                      promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                    }
                  },
                  teamConversationUsingCache,
                  teamConversationCacheUpdatedMs,
                  teamQueueCount,
                  teamQueue,
                  teamQueueNeedsRun,
                  onClearQueue: clearTeamQueue,
                  onStartQueuedRun: () => {
                    setChatTarget("team");
                    setTeamAction("run");
                    if (promptbarRef.current) {
                      promptbarRef.current.scrollIntoView({ behavior: "smooth", block: "center" });
                    }
                  },
                  recentActivity: teamRecentActivity,
                }}
                inlineTeamSetupOpen={inlineTeamSetupOpen}
                setInlineTeamSetupOpen={setInlineTeamSetupOpen}
                advancedPage={advancedPage}
                inlineTeamConsoleProps={{
                  mode: "inline",
                  forcedTab: "setup",
                  base: connection.brokerBase,
                  daemonBase: effectiveBase,
                  auth: daemonAuth,
                  authKey,
                  clientId: client.id,
                }}
                promptbarRef={promptbarRef}
                historyPanelProps={{
                  entries: historyEntriesDesc,
                  showAllEntries: showAllHistoryEntries,
                  setShowAllEntries: setShowAllHistoryEntries,
                  showMessages: showHistoryMessages,
                  setShowMessages: setShowHistoryMessages,
                  historyExpandedByKey,
                  setHistoryExpandedByKey,
                  dbMessages: dbMessageRows,
                  dbRuns: dbRunRows,
                  dbRunDetailsById,
                  sessionArtifacts: !sessionArtifactsUnsupported ? sessionArtifactRows : [],
                  artifactCatalogMode:
                    sessionArtifactsUnsupported ? "unsupported" : connectionMode === "broker" ? "broker_reference" : "direct",
                  effectiveBase,
                  yolo,
                  sessionId,
                  client,
                  daemonAuth,
                  showDebugInConversation,
                  allowAutoplay,
                  allowClientRpcs,
                  allowClientEffects,
                  allowUnsafePageEval,
                  sceneEntities,
                  onSceneApply: (ops) => applySceneOps(String(sessionId || "").trim(), ops),
                  onTraceIdClick: (traceId) => {
                    setTraceLookupId(traceId);
                    setAdvancedPage("trace");
                    void traceLookup.mutateAsync(traceId).catch(() => {});
                  },
                  teamConversationItems,
                  teamId: selectedTeamIdTrimmed,
                  teamRunId: latestTeamRunId,
                  teamRunCreatedMs: latestTeamRunCreatedMs,
                  teamRunStatus: teamStatus,
                  teamConversationWarnings,
                }}
              />
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
                voicePanel={{ baseUrl: effectiveBase, auth: daemonAuth, sessionId }}
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
                  daemonBase: effectiveBase,
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
          leaseSeconds: sessionLeaseSeconds,
          setLeaseSeconds: setSessionLeaseSeconds,
          info: sessionInfoData,
          leaseConflict: sessionLeaseConflict,
          clearLeaseConflict: () => setSessionLeaseConflict(null),
          attach: () => void attachSession.mutateAsync(sessionId).catch(() => {}),
          attachPending: attachSession.isPending,
          attachError: attachSessionError,
          renewAttachment: () => void renewSessionAttachment.mutateAsync(sessionId).catch(() => {}),
          renewPending: renewSessionAttachment.isPending,
          renewError: renewSessionAttachmentError,
          releaseAttachment: () => void releaseSessionAttachment.mutateAsync(sessionId).catch(() => {}),
          releasePending: releaseSessionAttachment.isPending,
          releaseError: releaseSessionAttachmentError,
          streamStatus: sessionEventStream.status,
          streamLastEventId: sessionEventStream.lastEventId,
          streamLastEventAtMs: sessionEventStream.lastEventAtMs,
          streamUpdatedMs: sessionEventStream.updatedMs,
          streamBufferedCount: sessionEventStream.bufferedCount,
          streamError: sessionEventStream.error,
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
