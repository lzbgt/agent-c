import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiGetAudit,
  apiGetCaps,
  apiGetClientPrefs,
  apiGetConfig,
  apiGetDbUiActions,
  apiGetDbClientEvents,
  apiGetSessionClientEvents,
  apiGetHealth,
  apiGetJob,
  apiGetSessionArtifacts,
  apiGetSessionScene,
  apiGetTools,
  apiUpdateDaemonConfig,
  apiDeleteSession,
  apiListSessions,
  apiNewSession,
  apiPostSessionSceneApply,
  apiPostSessionUiEvent,
  apiPostClientPrefs,
  apiRun,
  apiRunAsync,
  apiAgentdTrace,
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiBrokerTrace,
  daemonHeaders,
  RunRequest,
  RunResponse,
  type Caps,
  type AgentEvent,
} from "./api";
import { loadJson } from "./jsonUtils";
import { pruneJobsBySession } from "./jobStore";
import { SCENE_STORE_MAX, touchSceneStoreKey } from "./sceneCache";
import { buildScopedSessionKey, buildSessionScopeKey } from "./sessionScope";
import { brokerBaseFromProxy } from "./utils/brokerBase";
import HistoryPanel from "./components/HistoryPanel";
import SceneView, { type SceneEntity } from "./components/SceneView";
import PromptBar, { type Attachment } from "./components/PromptBar";
import SettingsDrawer from "./components/SettingsDrawer";
import TraceLookupPanel from "./components/TraceLookupPanel";
import BrokerPanel from "./components/BrokerPanel";
import MemoryPanel from "./components/MemoryPanel";
import RunDiffPanel from "./components/RunDiffPanel";
import ApprovalQueuePanel from "./components/ApprovalQueuePanel";
import WorkflowPanel from "./components/WorkflowPanel";
import useLocalStorageState from "./hooks/useLocalStorageState";
import useJobStreaming from "./hooks/useJobStreaming";
import useUiSettings from "./hooks/useUiSettings";
import { buildWorkflowDefaults } from "./workflowDefaults";

const RUN_WATCH_PREFS_KIND = "run_watch";
const RUN_WATCH_PREFS_VERSION = 1;
const RUN_WATCH_PERSIST_MIN_INTERVAL_MS = 5000;

type RunWatchByScope = Record<string, any>;

const extractRunWatchByScope = (prefs: any): RunWatchByScope => {
  const root = prefs && typeof prefs === "object" ? prefs.run_watch : null;
  const byScope = root && typeof root === "object" ? root.by_scope : null;
  return byScope && typeof byScope === "object" ? (byScope as RunWatchByScope) : {};
};

const runWatchTs = (value: any): number => {
  const updated = typeof value?.updated_unix_ms === "number" ? value.updated_unix_ms : 0;
  if (Number.isFinite(updated) && updated > 0) return updated;
  const started = typeof value?.started_unix_ms === "number" ? value.started_unix_ms : 0;
  return Number.isFinite(started) ? started : 0;
};

const mergeRunWatchByScope = (local: RunWatchByScope, remote: RunWatchByScope): RunWatchByScope => {
  const next: RunWatchByScope = { ...(local || {}) };
  for (const [key, value] of Object.entries(remote || {})) {
    if (!value || typeof value !== "object") continue;
    const cur = next[key];
    if (!cur || runWatchTs(value) >= runWatchTs(cur)) {
      next[key] = value;
    }
  }
  return next;
};

const runWatchMapsEqual = (a: RunWatchByScope, b: RunWatchByScope): boolean => {
  const keysA = Object.keys(a || {});
  const keysB = Object.keys(b || {});
  if (keysA.length !== keysB.length) return false;
  for (const key of keysA) {
    const av = a[key];
    const bv = b[key];
    if (!bv) return false;
    if ((av?.job_id || "") !== (bv?.job_id || "")) return false;
    if ((av?.cursor || 0) !== (bv?.cursor || 0)) return false;
    if (runWatchTs(av) !== runWatchTs(bv)) return false;
  }
  return true;
};

export default function App() {
  const ui = useUiSettings();
  const { connection, run: runSettings, client: clientSettings, brokerPanelOpen, setBrokerPanelOpen } = ui;
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
  const [traceLookupOpen, setTraceLookupOpen] = useLocalStorageState("agentui.traceLookupOpen", false);
  const [memoryPanelOpen, setMemoryPanelOpen] = useLocalStorageState("agentui.memoryPanelOpen", false);
  const [runDiffPanelOpen, setRunDiffPanelOpen] = useLocalStorageState("agentui.runDiffPanelOpen", false);
  const [approvalPanelOpen, setApprovalPanelOpen] = useLocalStorageState("agentui.approvalPanelOpen", false);
  const [workflowPanelOpen, setWorkflowPanelOpen] = useLocalStorageState("agentui.workflowPanelOpen", false);
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

  const [traceLookupError, setTraceLookupError] = React.useState<string | null>(null);
  const [traceLookupAgentd, setTraceLookupAgentd] = React.useState<any | null>(null);
  const [traceLookupBroker, setTraceLookupBroker] = React.useState<any | null>(null);
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
  // Persist job_id + cursor so a browser refresh can reliably resume a running session.
  // Stored per session_id (since multiple sessions can exist and the UI allows switching).
  const [jobsBySessionJson, setJobsBySessionJson] = useLocalStorageState("agentui.jobsBySession", "{}");
  const runWatchPrefsBase = React.useMemo(() => {
    const base = String(effectiveBase || "").trim();
    if (!base) return "";
    return daemonAuth.mode === "broker" ? brokerBaseFromProxy(base) : base;
  }, [daemonAuth.mode, effectiveBase]);
  const runWatchPrefsClientId = React.useMemo(() => String(clientId || "webui"), [clientId]);
  const runWatchCanUse = React.useMemo(() => {
    if (!runWatchPrefsBase || !runWatchPrefsClientId) return false;
    if (daemonAuth.mode !== "broker") return true;
    return String(brokerAuthToken || "").trim().length > 0;
  }, [brokerAuthToken, daemonAuth.mode, runWatchPrefsBase, runWatchPrefsClientId]);
  const [runWatchServerStatus, setRunWatchServerStatus] = React.useState<"idle" | "loading" | "ready" | "error">("idle");
  const runWatchPersistRef = React.useRef<{
    timer: ReturnType<typeof setTimeout> | null;
    pending: RunWatchByScope | null;
    lastSentAt: number;
  }>({ timer: null, pending: null, lastSentAt: 0 });
  const topbarRef = React.useRef<HTMLElement | null>(null);
  const promptbarRef = React.useRef<HTMLDivElement | null>(null);
  const [topbarHeightPx, setTopbarHeightPx] = React.useState<number>(56);
  const [promptbarHeightPx, setPromptbarHeightPx] = React.useState<number>(220);
  // Session-scoped client-side scene entities (collaboration surface objects).
  const sceneBySessionRef = React.useRef<Record<string, Record<string, SceneEntity>>>({});
  const [sceneVersion, setSceneVersion] = React.useState<number>(0);
  const sceneStoreOrderRef = React.useRef<string[]>([]);
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

  const historyUiKey = React.useMemo(() => {
    const sid = String(sessionId || "").trim();
    return `agentui.historyUi:${sessionScopeKey}::${sid}`;
  }, [sessionId, sessionScopeKey]);
  const [showAllHistoryEntries, setShowAllHistoryEntries] = useLocalStorageState<boolean>(`${historyUiKey}:showAll`, false);
  const [showHistoryMessages, setShowHistoryMessages] = useLocalStorageState<boolean>(`${historyUiKey}:showMessages`, false);
  const [historyExpandedByKey, setHistoryExpandedByKey] = useLocalStorageState<Record<string, boolean>>(
    `${historyUiKey}:expandedByKey`,
    {},
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
  const sceneStoreKey = scopedSessionKey;
  const jobStoreKey = scopedSessionKey;
  const sceneEntities = React.useMemo(() => {
    const m = sceneBySessionRef.current[sceneStoreKey] || {};
    return Object.values(m);
  }, [sceneStoreKey, sceneVersion]);

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

  const parseJobsBySession = React.useCallback(() => {
    const v = loadJson(jobsBySessionJson);
    const jobs = v && typeof v === "object" ? (v as Record<string, any>) : {};
    const pruned = pruneJobsBySession(Date.now(), jobs);
    if (pruned.changed) {
      try {
        setJobsBySessionJson(JSON.stringify(pruned.next));
      } catch {
        // ignore
      }
    }
    return pruned.next;
  }, [jobsBySessionJson, setJobsBySessionJson]);

  const pushServerRunWatch = React.useCallback(
    async (nextMap: RunWatchByScope) => {
      if (!runWatchCanUse) return;
      const payload = {
        client_id: runWatchPrefsClientId,
        client_kind: RUN_WATCH_PREFS_KIND,
        prefs: { run_watch: { version: RUN_WATCH_PREFS_VERSION, by_scope: nextMap } },
      };
      const resp =
        daemonAuth.mode === "broker"
          ? await apiBrokerPostClientPrefs(runWatchPrefsBase, payload, daemonAuth)
          : await apiPostClientPrefs(runWatchPrefsBase, payload, daemonAuth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "run watch prefs update failed");
      }
      runWatchPersistRef.current.lastSentAt = Date.now();
      setRunWatchServerStatus("ready");
    },
    [daemonAuth, runWatchCanUse, runWatchPrefsBase, runWatchPrefsClientId],
  );

  const scheduleRunWatchPersist = React.useCallback(
    (nextMap: RunWatchByScope) => {
      if (!runWatchCanUse) return;
      if (runWatchServerStatus === "error") return;
      runWatchPersistRef.current.pending = nextMap;
      if (runWatchPersistRef.current.timer) return;
      const now = Date.now();
      const since = now - runWatchPersistRef.current.lastSentAt;
      const delay = Math.max(RUN_WATCH_PERSIST_MIN_INTERVAL_MS - since, 0);
      runWatchPersistRef.current.timer = setTimeout(() => {
        const pending = runWatchPersistRef.current.pending;
        runWatchPersistRef.current.pending = null;
        runWatchPersistRef.current.timer = null;
        if (!pending) return;
        pushServerRunWatch(pending).catch(() => {
          setRunWatchServerStatus("error");
        });
      }, delay);
    },
    [pushServerRunWatch, runWatchCanUse, runWatchServerStatus],
  );

  const loadServerRunWatch = React.useCallback(async () => {
    if (!runWatchCanUse) return;
    setRunWatchServerStatus("loading");
    try {
      const resp =
        daemonAuth.mode === "broker"
          ? await apiBrokerGetClientPrefs(runWatchPrefsBase, runWatchPrefsClientId, RUN_WATCH_PREFS_KIND, daemonAuth)
          : await apiGetClientPrefs(runWatchPrefsBase, runWatchPrefsClientId, RUN_WATCH_PREFS_KIND, daemonAuth);
      if (!resp.ok) {
        throw new Error(resp.error || resp.err || resp.code || "run watch prefs fetch failed");
      }
      const now = Date.now();
      const remoteMap = pruneJobsBySession(now, extractRunWatchByScope(resp.prefs)).next;
      const localMap = parseJobsBySession();
      const merged = mergeRunWatchByScope(localMap, remoteMap);
      if (!runWatchMapsEqual(merged, localMap)) {
        try {
          setJobsBySessionJson(JSON.stringify(merged));
        } catch {
          // ignore
        }
      }
      if (!runWatchMapsEqual(merged, remoteMap)) {
        scheduleRunWatchPersist(merged);
      }
      setRunWatchServerStatus("ready");
    } catch {
      setRunWatchServerStatus("error");
    }
  }, [
    daemonAuth,
    parseJobsBySession,
    runWatchCanUse,
    runWatchPrefsBase,
    runWatchPrefsClientId,
    scheduleRunWatchPersist,
    setJobsBySessionJson,
  ]);

  React.useEffect(() => {
    if (!runWatchCanUse) return;
    void loadServerRunWatch();
  }, [authKey, loadServerRunWatch, runWatchCanUse]);

  React.useEffect(
    () => () => {
      if (runWatchPersistRef.current.timer) {
        try {
          clearTimeout(runWatchPersistRef.current.timer);
        } catch {
          // ignore
        }
      }
      runWatchPersistRef.current.timer = null;
    },
    [],
  );

  const writeJobsBySession = React.useCallback(
    (mutate: (prev: Record<string, any>) => Record<string, any>) => {
      setJobsBySessionJson((prevRaw) => {
        const prev = (loadJson(String(prevRaw || "")) as Record<string, any>) || {};
        const next = mutate(prev);
        scheduleRunWatchPersist(next);
        try {
          return JSON.stringify(next);
        } catch {
          return JSON.stringify(prev);
        }
      });
    },
    [scheduleRunWatchPersist, setJobsBySessionJson],
  );

  // Track the last observed daemon-updated scene timestamp per session so refresh/polling is stable.
  const lastSceneUpdatedMsRef = React.useRef<Record<string, number>>({});
  const touchSceneStore = React.useCallback(
    (key: string) => {
      touchSceneStoreKey(sceneBySessionRef.current, sceneStoreOrderRef.current, lastSceneUpdatedMsRef.current, key, SCENE_STORE_MAX);
    },
    [],
  );

  const applySceneOps = React.useCallback(
    (sid: string, ops: any[]) => {
      const sessionId = String(sid || "").trim();
      if (!sessionId) throw new Error("missing session_id for scene ops");
      const storeKey = `${sessionScopeKey}::${sessionId}`;
      if (!sceneBySessionRef.current[storeKey]) sceneBySessionRef.current[storeKey] = {};
      const store = sceneBySessionRef.current[storeKey];
      touchSceneStore(storeKey);

      const now = Date.now();
      const results: any[] = [];
      const genId = () => `ent-${now}-${Math.random().toString(16).slice(2)}`;

      const getOpKind = (op: any): string => {
        const k = typeof op?.op === "string" ? op.op : typeof op?.kind === "string" ? op.kind : "";
        return String(k || "").trim();
      };

      const getCreateKind = (op: any): string => {
        const k = typeof op?.entity_kind === "string" ? op.entity_kind : typeof op?.entityKind === "string" ? op.entityKind : "";
        return String(k || "").trim();
      };

      // WebUI safety: never persist a "clear" op to the daemon (it would wipe durable DB state).
      const persistOps = (Array.isArray(ops) ? ops : []).filter((op) => getOpKind(op) !== "clear");

      for (const opRaw of (Array.isArray(ops) ? ops : []).slice(0, 100)) {
        try {
          const op = opRaw && typeof opRaw === "object" ? opRaw : {};
          const kind = getOpKind(op);
          if (!kind) throw new Error("missing op");
          if (kind === "create") {
            const id = String(op.id ?? "").trim() || genId();
            const entityKind = getCreateKind(op);
            if (!entityKind) throw new Error("create requires entity_kind");
            const ent: SceneEntity = {
              id,
              kind: entityKind,
              title: typeof op.title === "string" ? op.title : undefined,
              props: op.props ?? {},
              created_ms: now,
              updated_ms: now,
            };
            store[id] = ent;
            results.push({ ok: true, op: "create", id });
            continue;
          }
          if (kind === "update") {
            const id = String(op.id ?? "").trim();
            if (!id) throw new Error("update requires id");
            const existing = store[id];
            if (!existing) throw new Error("entity not found");
            const patch = op.props ?? {};
            existing.props = { ...(existing.props ?? {}), ...(patch ?? {}) };
            existing.updated_ms = now;
            results.push({ ok: true, op: "update", id });
            continue;
          }
          if (kind === "delete" || kind === "remove") {
            const id = String(op.id ?? "").trim();
            if (!id) throw new Error("delete requires id");
            const existed = !!store[id];
            delete store[id];
            results.push({ ok: true, op: "delete", id, existed });
            continue;
          }
          if (kind === "clear") {
            results.push({ ok: false, op: "clear", error: "scene clear is disabled in WebUI" });
            continue;
          }
          if (kind === "action") {
            const id = String(op.id ?? "").trim();
            const action = String(op.action ?? "").trim();
            if (!id) throw new Error("action requires id");
            if (!action) throw new Error("action requires action");
            const existing = store[id];
            if (!existing) throw new Error("entity not found");
            // Intentionally generic: actions are *data*, not hardcoded behavior.
            // If the client wants to interpret actions, it can do so in the renderer (e.g. execute JS script in a canvas entity).
            existing.props = {
              ...(existing.props ?? {}),
              last_action: { name: action, args: op.args ?? {}, ts_unix_ms: now },
            };
            existing.updated_ms = now;
            results.push({ ok: true, op: "action", id, action });
            continue;
          }
          throw new Error(`unsupported op: ${kind}`);
        } catch (e) {
          results.push({ ok: false, error: String(e) });
        }
      }

      setSceneVersion((v) => v + 1);
      // Best-effort: persist to daemon so the Scene is durable across refresh.
      if (persistOps.length > 0) {
        void apiPostSessionSceneApply(effectiveBase, { session_id: sessionId, ops: persistOps }, daemonAuth)
          .then((r) => {
            if (!r || r.ok !== true) return;
            const updated = typeof r.updated_unix_ms === "number" ? r.updated_unix_ms : 0;
            if (updated > 0) lastSceneUpdatedMsRef.current[storeKey] = Math.max(lastSceneUpdatedMsRef.current[storeKey] || 0, updated);
            // If the daemon returns a scene snapshot, prefer it (authoritative).
            const scene = r.scene && typeof r.scene === "object" && !Array.isArray(r.scene) ? (r.scene as any) : null;
            if (scene) {
              sceneBySessionRef.current[storeKey] = scene;
              touchSceneStore(storeKey);
              setSceneVersion((v) => v + 1);
            }
          })
          .catch(() => {});
      }
      return { ok: true, results, count: Object.keys(store).length };
    },
    [daemonAuth, effectiveBase, sessionScopeKey, setSceneVersion, touchSceneStore],
  );

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

  const postedCapsRef = React.useRef<Record<string, boolean>>({});
  React.useEffect(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;
    const key = `${effectiveBase}::${sid}`;
    if (postedCapsRef.current[key]) return;
    postedCapsRef.current[key] = true;
    void apiPostSessionUiEvent(
      effectiveBase,
      {
        session_id: sid,
        type: "client_capabilities",
        client,
        data: {
          rpcs: [
            { kind: "dom_query", side_effects: false, description: "Read-only DOM query (selector + bounded fields)." },
            { kind: "media_snapshot", side_effects: false, description: "Snapshot audio/video elements (paused/currentTime/duration)." },
            { kind: "location", side_effects: false, description: "Browser location (href/origin/path/search; query redacted)." },
            { kind: "state_snapshot", side_effects: false, description: "Combined snapshot (location + media_snapshot)." },
            { kind: "entity_query", side_effects: false, description: "Query client-side entities (scene objects)." },
            { kind: "entity_apply", side_effects: true, description: "Create/update/delete/action client-side entities (scene objects)." },
            { kind: "dom_apply", side_effects: true, description: "Apply a DOM patch (create/edit/delete/dispatch) by selector." },
            { kind: "dom_click", side_effects: true, description: "Click a DOM element by selector (side effects)." },
            { kind: "dom_set_value", side_effects: true, description: "Set input/textarea value by selector (side effects)." },
            { kind: "media_play", side_effects: true, description: "Attempt to play audio/video by selector (browser policies apply)." },
            { kind: "media_observe", side_effects: true, description: "Attach media listeners and emit correlated progress events." },
            { kind: "media_unobserve", side_effects: false, description: "Detach media listeners created by media_observe (by rpc_id or all=true)." },
            { kind: "navigate", side_effects: true, description: "Navigate the browser to a new URL (likely reloads the app)." },
            { kind: "open_url", side_effects: true, description: "Open an external URL in a new tab after explicit user confirmation." },
            {
              kind: "artifact_url",
              side_effects: false,
              description:
                "Resolve a daemon-served artifact path (out/...) to a browser-usable URL. Returns blob: URL when daemon auth is enabled.",
            },
            { kind: "script_eval", side_effects: false, description: "Run agent-provided script code in a killable worker with a DOM/media/location API bridge." },
            { kind: "page_eval", side_effects: true, description: "UNSAFE: run agent-provided JS on the main thread with access to DOM via an API bridge (cooperative async only)." },
          ],
          // Legacy alias (probe-only clients); kept small and read-only.
          probes: [
            { kind: "dom_query", description: "Read-only DOM query (selector + bounded fields)." },
            { kind: "media_snapshot", description: "Snapshot audio/video elements." },
            { kind: "location", description: "Browser location." },
          ],
        },
        append_to_session: false,
      },
      daemonAuth,
    ).catch(() => {});
  }, [client, daemonAuth, effectiveBase, sessionId]);

  // Restore a running job after a browser refresh (best-effort).
  React.useEffect(() => {
    if (activeJobId) return;
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;
    const jobs = parseJobsBySession();
    const rec = jobs[jobStoreKey] || jobs[sid];
    const jobId = typeof rec?.job_id === "string" ? rec.job_id : "";
    if (!jobId) return;
    const cursor = typeof rec?.cursor === "number" && Number.isFinite(rec.cursor) && rec.cursor >= 0 ? Math.floor(rec.cursor) : 0;

    if (!jobs[jobStoreKey] && jobs[sid]) {
      writeJobsBySession((prev) => {
        if (prev[jobStoreKey]) return prev;
        const next = { ...prev, [jobStoreKey]: prev[sid] };
        delete next[sid];
        return next;
      });
    }

    let cancelled = false;
    (async () => {
      try {
        const job = await apiGetJob(effectiveBase, jobId, daemonAuth);
        if (cancelled) return;
        if (!job.ok) {
          writeJobsBySession((prev) => {
            const next = { ...prev };
            delete next[jobStoreKey];
            return next;
          });
          return;
        }
        const st = typeof job.status === "string" ? job.status : "";
        if (st === "queued" || st === "running") {
          cursorRef.current = cursor;
          setJobError(null);
          setJobStatus(st);
          setJobUpdatedMs(typeof job.updated_unix_ms === "number" ? job.updated_unix_ms : null);
          setLiveEvents([]);
          setActiveJobId(jobId);
          return;
        }
        // Job already finished; clear persisted pointer.
        writeJobsBySession((prev) => {
          const next = { ...prev };
          delete next[jobStoreKey];
          return next;
        });
      } catch {
        // ignore; user can retry by reloading or the daemon UI.
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [activeJobId, daemonAuth, effectiveBase, jobStoreKey, parseJobsBySession, sessionId, writeJobsBySession]);

  const health = useQuery({
    queryKey: ["health", effectiveBase, authKey],
    queryFn: () => apiGetHealth(effectiveBase, daemonAuth),
    retry: 1,
  });

  const daemonConfig = useQuery({
    queryKey: ["config", effectiveBase, authKey],
    queryFn: () => apiGetConfig(effectiveBase, daemonAuth),
    retry: 1,
  });

  const sessions = useQuery({
    queryKey: ["sessions", effectiveBase, authKey],
    queryFn: () => apiListSessions(effectiveBase, daemonAuth),
    retry: 1,
  });
  const sessionsUnauthorized =
    sessions.isSuccess &&
    sessions.data &&
    sessions.data.ok === false &&
    String((sessions.data as any).error || "").toLowerCase() === "unauthorized";
  const missingBrokerAuthToken = connectionMode === "broker" && String(brokerAuthToken || "").trim().length === 0;
  const missingDaemonAuthToken = String(daemonAuthToken || "").trim().length === 0;
  const isLocalDaemonBase = React.useMemo(() => {
    try {
      const u = new URL(effectiveBase);
      const host = String(u.hostname || "").toLowerCase();
      return host === "127.0.0.1" || host === "localhost" || host === "0.0.0.0";
    } catch {
      return false;
    }
  }, [effectiveBase]);

  const audit = useQuery({
    queryKey: ["audit", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetAudit(effectiveBase, sessionId, daemonAuth),
    enabled: !!sessionId,
    retry: 1,
  });

  React.useEffect(() => {
    const el = mainScrollRef.current;
    if (!el) return;
    // Restore scroll once per (base, session) after we have initial content.
    // This avoids the "refresh jumps me to a random place" UX for long histories.
    const restoreKey = `${effectiveBase}::${String(sessionId || "").trim()}`;
    if (mainScrollRestoredKeyRef.current === restoreKey) return;
    // Wait until we have at least the audit fetch result (even if empty) so layout is stable-ish.
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

  const auditEntriesDesc = React.useMemo(() => {
    const raw = audit.data?.ok && Array.isArray(audit.data?.entries) ? (audit.data.entries as any[]) : [];
    const entries = raw.filter((e) => e && typeof e === "object");
    entries.sort((a: any, b: any) => {
      const ta = typeof a?.ts_unix_ms === "number" ? a.ts_unix_ms : 0;
      const tb = typeof b?.ts_unix_ms === "number" ? b.ts_unix_ms : 0;
      return tb - ta;
    });
    return entries;
  }, [audit.data]);

  // While an async job is running, surface its live event stream as the top "history" entry so:
  // - the user sees progress immediately (tool calls, streaming deltas, artifacts)
  // - client RPCs (including entity_apply) can update the Scene during the run
  const historyEntriesDesc = React.useMemo(() => {
    if (!activeJobId) return auditEntriesDesc;
    const ts = typeof jobUpdatedMs === "number" && jobUpdatedMs > 0 ? jobUpdatedMs : Date.now();
    const live = {
      ts_unix_ms: ts,
      prompt: lastRunPromptRef.current || lastRunPrompt || "",
      assistant_text: "",
      events: liveEvents,
      ok: undefined,
      job_id: activeJobId,
      job_status: jobStatus ?? "running",
      live: true,
    };
    return [live, ...auditEntriesDesc];
  }, [activeJobId, auditEntriesDesc, jobStatus, jobUpdatedMs, lastRunPrompt, liveEvents]);

  const sessionClientEvents = useQuery({
    queryKey: ["session_client_events", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetSessionClientEvents(effectiveBase, sessionId, daemonAuth, { maxBytes: 1024 * 1024 }),
    enabled: !!sessionId,
    retry: 1,
  });

  const sessionArtifacts = useQuery({
    queryKey: ["session_artifacts", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetSessionArtifacts(effectiveBase, sessionId, daemonAuth, { maxBytes: 2 * 1024 * 1024, maxArtifacts: 64 }),
    enabled: !!sessionId,
    retry: 1,
  });

  const sessionScene = useQuery({
    queryKey: ["session_scene", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetSessionScene(effectiveBase, sessionId, daemonAuth),
    enabled: !!sessionId,
    refetchInterval: activeJobId ? 750 : 2500,
    retry: 1,
  });

  // Hydrate/refresh the local Scene from the daemon-owned durable scene snapshot.
  // This makes the Scene refresh-proof without relying on browser storage.
  React.useEffect(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    if (!sid) return;
    if (!sessionScene.data || sessionScene.data.ok !== true) return;
    const updated = typeof (sessionScene.data as any)?.updated_unix_ms === "number" ? (sessionScene.data as any).updated_unix_ms : 0;
    const key = sceneStoreKey;
    const hasPrev = Object.prototype.hasOwnProperty.call(lastSceneUpdatedMsRef.current, key);
    const prev = hasPrev ? lastSceneUpdatedMsRef.current[key] || 0 : -1;
    if (hasPrev && updated <= prev) return;

    const scene = (sessionScene.data as any)?.scene;
    if (!scene || typeof scene !== "object" || Array.isArray(scene)) return;
    sceneBySessionRef.current[key] = scene as any;
    lastSceneUpdatedMsRef.current[key] = updated;
    touchSceneStore(key);
    setSceneVersion((v) => v + 1);
  }, [sceneStoreKey, sessionId, sessionScene.data, touchSceneStore]);

  const dbUiActions = useQuery({
    queryKey: ["db_ui_actions", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetDbUiActions(effectiveBase, sessionId, daemonAuth, { limit: 100, offset: 0 }),
    enabled: !!sessionId && allowClientRpcs && allowClientEffects,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const dbClientEvents = useQuery({
    queryKey: ["db_client_events", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetDbClientEvents(effectiveBase, sessionId, daemonAuth, { limit: 100, offset: 0 }),
    enabled: !!sessionId && allowClientRpcs && allowClientEffects,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const newSession = useMutation({
    mutationFn: async () => {
      const r = await apiNewSession(effectiveBase, daemonAuth);
      return r;
    },
    onSuccess: (v) => {
      if (v.ok && v.session_id) {
        // New session is expected to be a "clean slate" UX.
        // Clear run UI state and any persisted prompt so the user doesn't see stale history from a prior session.
        setPrompt("");
        lastRunPromptRef.current = "";
        setLastRunPrompt("");
        setLastCompletedPrompt("");
        setResult(undefined);
        setLiveEvents([]);
        setActiveJobId(null);
        setJobStatus(null);
        setJobError(null);
        setJobUpdatedMs(null);
        cursorRef.current = 0;
        setSessionId(v.session_id);
        void sessions.refetch();
        void audit.refetch();
        void sessionClientEvents.refetch();
        void sessionArtifacts.refetch();
        void sessionScene.refetch();
        void dbUiActions.refetch();
        void dbClientEvents.refetch();
      }
    },
  });

  const deleteSession = useMutation({
    mutationFn: async (sid: string) => {
      const s = String(sid || "").trim();
      if (!s) throw new Error("missing session id");
      const r = await apiDeleteSession(effectiveBase, s, daemonAuth);
      if (!r.ok) throw new Error(r.error || "delete failed");
      return { session_id: s };
    },
    onSuccess: async (v) => {
      await sessions.refetch();
      // If we deleted the active session, move to a clean slate.
      if (v.session_id === String(sessionId || "").trim()) {
        await newSession.mutateAsync();
      } else {
        await audit.refetch();
      }
    },
  });

  const updateDaemonDefaults = useMutation({
    mutationFn: async (payload: any) => {
      const r = await apiUpdateDaemonConfig(effectiveBase, payload, daemonAuth);
      if (!r.ok) throw new Error(r.error || "update failed");
      return r;
    },
    onSuccess: async () => {
      await daemonConfig.refetch();
    },
  });

  const inferredProvider = React.useMemo(() => {
    const b = String(baseUrl || "").toLowerCase();
    if (b.includes("deepseek")) return "deepseek";
    if (b.includes("openrouter")) return "openrouter";
    if (b.includes("moonshot") || b.includes("kimi")) return "moonshot";
    return "openai";
  }, [baseUrl]);

  const saveDaemonDefaults = React.useCallback(() => {
    const tms = Number(timeoutMs);
    const smc = Number(summaryMaxChars);
    void updateDaemonDefaults
      .mutateAsync({
        base_url: baseUrl || undefined,
        model: model || undefined,
        summary_model: summaryModel && summaryModel.trim().length > 0 ? summaryModel.trim() : null,
        summary_max_chars: Number.isFinite(smc) && smc >= 0 ? smc : undefined,
        proxy_url: proxyUrl && proxyUrl.trim().length > 0 ? proxyUrl.trim() : null,
        timeout_ms: Number.isFinite(tms) && tms > 0 ? tms : undefined,
      })
      .catch(() => {});
  }, [baseUrl, model, proxyUrl, summaryMaxChars, summaryModel, timeoutMs, updateDaemonDefaults]);

  const saveDaemonApiKey = React.useCallback(() => {
    void updateDaemonDefaults
      .mutateAsync({
        provider: inferredProvider,
        api_key: String(apiKey || "").trim(),
      })
      .catch(() => {});
  }, [apiKey, inferredProvider, updateDaemonDefaults]);

  const clearDaemonApiKey = React.useCallback(() => {
    if (!confirm(`Clear daemon-stored key for provider '${inferredProvider}'?`)) return;
    void updateDaemonDefaults
      .mutateAsync({
        provider: inferredProvider,
        api_key: "",
      })
      .catch(() => {});
  }, [inferredProvider, updateDaemonDefaults]);

  const clearAllSessions = useMutation({
    mutationFn: async () => {
      const r = await apiListSessions(effectiveBase, daemonAuth);
      if (!r.ok) throw new Error(r.error || "failed to list sessions");
      const ids = (r.sessions ?? []).slice();
      // Delete deterministically (serial) to keep daemon load predictable and to make failures clear.
      for (const sid of ids) {
        const d = await apiDeleteSession(effectiveBase, sid, daemonAuth);
        if (!d.ok) throw new Error(d.error || `failed to delete session: ${sid}`);
      }
      return { deleted: ids.length };
    },
    onSuccess: async () => {
      await sessions.refetch();
      await newSession.mutateAsync();
    },
  });

  const autoSessionInitRef = React.useRef(false);
  React.useEffect(() => {
    if (autoSessionInitRef.current) return;
    if (!sessions.isSuccess) return;
    const ids = sessions.data?.sessions ?? [];
    // If the UI is still on the historical "default" placeholder but no such session exists,
    // create a new unique session id to avoid collisions across tabs/clients.
    if (sessionId === "default" && !ids.includes("default")) {
      autoSessionInitRef.current = true;
      void newSession.mutateAsync().catch(() => {
        // keep default; user can retry via button
      });
    }
  }, [sessionId, sessions.isSuccess, sessions.data?.sessions, newSession]);

  // `useQuery` return objects are not stable across renders; depending on them in other effects can
  // cause accidental teardown/reconnect loops (notably for long-lived SSE streams).
  const auditRefetch = audit.refetch;
  const sessionsRefetch = sessions.refetch;
  const sessionList = sessions.data?.sessions ?? [];
  const deleteSessionError = deleteSession.isError ? String(deleteSession.error) : null;
  const clearAllSessionsError = clearAllSessions.isError ? String(clearAllSessions.error) : null;

  const toolsDefs = useQuery({
    queryKey: ["tools", effectiveBase, authKey, tools, yolo, hostPolicy, sessionId],
    queryFn: () =>
      apiGetTools(effectiveBase, daemonAuth, {
        tools,
        yolo,
        hostPolicy: tools === "host" ? hostPolicy : undefined,
        sessionId,
      }),
    retry: 1,
  });

  const [result, setResult] = React.useState<RunResponse | undefined>(undefined);
  // Incremented when a run successfully starts/completes; used to clear "next run only" UI state (e.g. attachments).
  const [composerTaskNonce, setComposerTaskNonce] = React.useState<number>(0);

  const run = useMutation({
    mutationFn: async (vars: { prompt: string; attachments: Attachment[] }) => {
      const maxStepsTrim = String(maxSteps ?? "").trim();
      const parsedMaxSteps =
        maxStepsTrim.length === 0
          ? undefined
          : Number.isFinite(Number(maxStepsTrim)) && Number(maxStepsTrim) >= 0
            ? Number(maxStepsTrim)
            : undefined;
      const maxToolCallsTotalTrim = String(maxToolCallsTotal ?? "").trim();
      const parsedMaxToolCallsTotal =
        maxToolCallsTotalTrim.length === 0
          ? undefined
          : Number.isFinite(Number(maxToolCallsTotalTrim)) && Number(maxToolCallsTotalTrim) >= 0
            ? Number(maxToolCallsTotalTrim)
            : undefined;
      const maxToolCallsPerToolTrim = String(maxToolCallsPerTool ?? "").trim();
      const parsedMaxToolCallsPerTool =
        maxToolCallsPerToolTrim.length === 0
          ? undefined
          : Number.isFinite(Number(maxToolCallsPerToolTrim)) && Number(maxToolCallsPerToolTrim) >= 0
            ? Number(maxToolCallsPerToolTrim)
            : undefined;

      const toolCallLimitsTrim = String(toolCallLimits ?? "").trim();
      let parsedToolCallLimits: { tool: string; max_calls: number }[] | undefined = undefined;
      if (toolCallLimitsTrim.length > 0) {
        const s = toolCallLimitsTrim;
        if (s.startsWith("[") || s.startsWith("{")) {
          try {
            const v: any = JSON.parse(s);
            const arr = Array.isArray(v) ? v : [v];
            parsedToolCallLimits = arr
              .map((item) => {
                const tool = String(item?.tool ?? item?.name ?? "").trim();
                const max_calls = Number(item?.max_calls ?? item?.maxCalls ?? item?.max ?? item?.limit ?? NaN);
                if (!tool) return null;
                if (!Number.isFinite(max_calls) || max_calls < 0) return null;
                return { tool, max_calls: Math.floor(max_calls) };
              })
              .filter(Boolean) as any;
          } catch (e) {
            throw new Error(`Invalid tool call limits JSON: ${String(e)}`);
          }
        } else {
          const parts = s
            .split(/[,\n]+/g)
            .map((x) => x.trim())
            .filter((x) => x.length > 0);
          const out: { tool: string; max_calls: number }[] = [];
          for (const p of parts) {
            const eq = p.indexOf("=");
            if (eq <= 0) throw new Error(`Invalid tool call limit (expected tool=max_calls): ${p}`);
            const tool = p.slice(0, eq).trim();
            const n = Number(p.slice(eq + 1).trim());
            if (!tool) throw new Error(`Invalid tool call limit tool name: ${p}`);
            if (!Number.isFinite(n) || n < 0) throw new Error(`Invalid tool call limit value: ${p}`);
            const max_calls = Math.floor(n);
            const existing = out.find((x) => x.tool === tool);
            if (existing) existing.max_calls = max_calls;
            else out.push({ tool, max_calls });
          }
          parsedToolCallLimits = out;
        }
        if (parsedToolCallLimits && parsedToolCallLimits.length === 0) {
          parsedToolCallLimits = undefined;
        }
      }
      const memMode =
        memoryContextMode === "search" || memoryContextMode === "index" || memoryContextMode === "salience"
          ? memoryContextMode
          : "files";
      const memDailyDaysTrim = String(memoryDailyDays ?? "").trim();
      const parsedMemDailyDays =
        memDailyDaysTrim.length === 0
          ? undefined
          : Number.isFinite(Number(memDailyDaysTrim)) && Number(memDailyDaysTrim) >= 0
            ? Number(memDailyDaysTrim)
            : undefined;
      const memTotalCapTrim = String(memoryTotalCap ?? "").trim();
      const parsedMemTotalCap =
        memTotalCapTrim.length === 0
          ? undefined
          : Number.isFinite(Number(memTotalCapTrim)) && Number(memTotalCapTrim) >= 0
            ? Number(memTotalCapTrim)
            : undefined;
      const memSearchQueryTrim = String(memorySearchQuery ?? "").trim();
      const memSearchMaxResultsTrim = String(memorySearchMaxResults ?? "").trim();
      const parsedMemSearchMaxResults =
        memSearchMaxResultsTrim.length === 0
          ? undefined
          : Number.isFinite(Number(memSearchMaxResultsTrim)) && Number(memSearchMaxResultsTrim) >= 0
            ? Number(memSearchMaxResultsTrim)
            : undefined;
      const memSearchMaxSnippetTrim = String(memorySearchMaxSnippetChars ?? "").trim();
      const parsedMemSearchMaxSnippetChars =
        memSearchMaxSnippetTrim.length === 0
          ? undefined
          : Number.isFinite(Number(memSearchMaxSnippetTrim)) && Number(memSearchMaxSnippetTrim) >= 0
            ? Number(memSearchMaxSnippetTrim)
            : undefined;
      const memSearchContextTrim = String(memorySearchContextLines ?? "").trim();
      const parsedMemSearchContextLines =
        memSearchContextTrim.length === 0
          ? undefined
          : Number.isFinite(Number(memSearchContextTrim)) && Number(memSearchContextTrim) >= 0
            ? Number(memSearchContextTrim)
            : undefined;
      const req: RunRequest = {
        prompt: vars.prompt,
        session_id: sessionId || undefined,
        no_session: false,
        input_files:
          vars.attachments.length > 0
            ? vars.attachments.map((a) => ({
                path: a.path,
                name: a.name,
                mime: a.mime,
                kind: a.kind,
              }))
            : undefined,
        client,
        tools,
        host_policy: tools === "host" ? hostPolicy : undefined,
        automation_profile: automationProfileValue,
        yolo,
        verbose,
        model: model || undefined,
        summary_model: summaryModel && summaryModel.trim().length > 0 ? summaryModel.trim() : undefined,
        summary_max_chars:
          Number.isFinite(Number(summaryMaxChars)) && Number(summaryMaxChars) >= 0 ? Number(summaryMaxChars) : undefined,
        base_url: baseUrl || undefined,
        api_key: apiKey || undefined,
        proxy: proxyUrl && proxyUrl.trim().length > 0 ? proxyUrl.trim() : undefined,
        timeout_ms: Number.isFinite(Number(timeoutMs)) && Number(timeoutMs) > 0 ? Number(timeoutMs) : undefined,
        stream_assistant: streamAssistant,
        max_capture_bytes:
          Number.isFinite(Number(maxCaptureBytes)) && Number(maxCaptureBytes) >= 0 ? Number(maxCaptureBytes) : undefined,
        max_steps: parsedMaxSteps,
        max_repeated_tool_calls:
          Number.isFinite(Number(maxRepeatedToolCalls)) && Number(maxRepeatedToolCalls) >= 0 ? Number(maxRepeatedToolCalls) : undefined,
        max_tool_calls_total: parsedMaxToolCallsTotal,
        max_tool_calls_per_tool: parsedMaxToolCallsPerTool,
        tool_call_limits: parsedToolCallLimits,
        max_chars: Number.isFinite(Number(maxChars)) ? Number(maxChars) : 20000,
        keep_last: Number.isFinite(Number(keepLast)) ? Number(keepLast) : 16,
        trace,
      };
      if (tools === "host") {
        req.memory_context_mode = memMode;
        req.memory_include_structured = memoryIncludeStructured;
        req.memory_include_core = memoryIncludeCore;
        req.memory_include_daily = memoryIncludeDaily;
        req.memory_include_session = memoryIncludeSession;
        req.memory_daily_days = parsedMemDailyDays;
        req.memory_total_cap = parsedMemTotalCap;
        if (memSearchQueryTrim.length > 0) req.memory_search_query = memSearchQueryTrim;
        req.memory_search_use_index = memorySearchUseIndex;
        req.memory_search_case_sensitive = memorySearchCaseSensitive;
        req.memory_search_fallback_to_files = memorySearchFallbackToFiles;
        const memOrder =
          memorySearchOrder === "newest" || memorySearchOrder === "oldest" || memorySearchOrder === "ranked"
            ? (memorySearchOrder as "ranked" | "newest" | "oldest")
            : "ranked";
        if (memOrder !== "ranked") req.memory_search_order = memOrder;
        req.memory_search_max_results = parsedMemSearchMaxResults;
        req.memory_search_max_snippet_chars = parsedMemSearchMaxSnippetChars;
        req.memory_search_context_lines = parsedMemSearchContextLines;
      }
      if (effectiveUseAsync) {
        const job = await apiRunAsync(effectiveBase, req, daemonAuth);
        return { mode: "async" as const, job, req };
      }
      const out = await apiRun(effectiveBase, req, daemonAuth);
      return { mode: "sync" as const, out, req };
    },
    onSuccess: (v) => {
      if (v.mode === "sync") {
        setComposerTaskNonce((n) => n + 1);
        // Only replace history once we have a new result (prevents "fetch failed" from wiping the UI).
        lastRunPromptRef.current = v.req.prompt;
        setLastRunPrompt(v.req.prompt);
        setLastCompletedPrompt(v.req.prompt);
        setResult(v.out);
        setLiveEvents([]);
        setJobError(null);
        setJobNotice(null);
        setJobStatus(null);
        setJobUpdatedMs(null);
        void audit.refetch();
        void sessions.refetch();
        return;
      }
      if (!v.job.ok || !v.job.job_id) {
        setJobError(v.job.error ?? "failed to start async job");
        setJobNotice(null);
        return;
      }
      setComposerTaskNonce((n) => n + 1);
      // Only reset/replace history after the job has been successfully created.
      lastRunPromptRef.current = v.req.prompt;
      setLastRunPrompt(v.req.prompt);
      setJobError(null);
      setJobNotice(null);
      setJobStatus(null);
      setJobUpdatedMs(null);
      setLiveEvents([]);
      cursorRef.current = 0;
      setActiveJobId(v.job.job_id);
      setJobStatus("queued");

      const sid = String(sessionId || "").trim();
      if (sid) {
        writeJobsBySession((prev) => ({
          ...prev,
          [jobStoreKey]: { job_id: v.job.job_id, cursor: 0, started_unix_ms: Date.now() },
        }));
      }
    },
    onError: (e) => {
      // Keep the last conversation visible when a run cannot be started.
      setJobError(`run failed: ${String(e)}`);
      setJobNotice(null);
    },
  });

  const traceLookup = useMutation({
    mutationFn: async (traceIdRaw: string) => {
      const tid = String(traceIdRaw || "").trim();
      if (!tid) throw new Error("missing trace_id");
      setTraceLookupError(null);
      setTraceLookupAgentd(null);
      setTraceLookupBroker(null);

      if (connectionMode === "broker") {
        const bb = String(connection.brokerBase || "").trim();
        if (!bb) throw new Error("missing broker base");
        return { mode: "broker" as const, data: await apiBrokerTrace(bb, tid, daemonAuth) };
      }
      return { mode: "direct" as const, data: await apiAgentdTrace(effectiveBase, tid, daemonAuth) };
    },
    onSuccess: (v) => {
      if (v.mode === "broker") {
        setTraceLookupBroker(v.data);
      } else {
        setTraceLookupAgentd(v.data);
      }
    },
    onError: (e) => {
      setTraceLookupError(String(e));
    },
  });

  // Persist job cursor while running (best-effort). This lets refresh resume from a stable point.
  React.useEffect(() => {
    if (!activeJobId) return;
    const sid = String(sessionId || "").trim();
    if (!sid) return;
    const jobId = activeJobId;

    const t = window.setInterval(() => {
      const cursor = cursorRef.current;
      writeJobsBySession((prev) => {
        const cur = prev[jobStoreKey];
        if (!cur || cur.job_id !== jobId) return prev;
        if (cur.cursor === cursor) return prev;
        return { ...prev, [jobStoreKey]: { ...cur, cursor, updated_unix_ms: Date.now() } };
      });
    }, 1000);
    return () => {
      try {
        window.clearInterval(t);
      } catch {
        // ignore
      }
    };
  }, [activeJobId, jobStoreKey, sessionId, writeJobsBySession]);

  // Execute any persisted entity_apply RPCs from DB ui_actions that have not yet been acknowledged.
  // This makes client RPC execution reliable across refreshes and SSE dropouts.
  const appliedUiActionIdsRef = React.useRef<Record<string, number>>({});
  const appliedUiActionLimit = 2000;
  React.useEffect(() => {
    if (!allowClientRpcs || !allowClientEffects) return;
    const sid = String(sessionId || "").trim();
    if (!sid) return;
    const actionsRaw = dbUiActions.data?.ok && Array.isArray(dbUiActions.data?.ui_actions) ? (dbUiActions.data.ui_actions as any[]) : [];
    const clientEventsRaw =
      dbClientEvents.data?.ok && Array.isArray(dbClientEvents.data?.client_events) ? (dbClientEvents.data.client_events as any[]) : [];

    const ackedRpcIds = new Set<string>();
    for (const ce of clientEventsRaw) {
      const t = typeof ce?.type === "string" ? ce.type : "";
      if (t !== "client_rpc_result") continue;
      const data = ce?.data ?? ce?.data_json ?? {};
      const rpcId = typeof data?.rpc_id === "string" ? data.rpc_id : typeof data?.probe_id === "string" ? data.probe_id : "";
      if (rpcId) ackedRpcIds.add(rpcId);
    }

    // DB endpoint sorts desc; apply older first so patches are deterministic.
    const actions = actionsRaw
      .slice()
      .reverse()
      .filter((a) => a && typeof a === "object");

    const safeObject = (v: any) => (v && typeof v === "object" && !Array.isArray(v) ? v : {});
    const entityApplyArgsToOps = (args: any): any[] => {
      if (Array.isArray(args?.ops)) return args.ops.slice(0, 100);
      if (Array.isArray(args?.operations)) return args.operations.slice(0, 100);
      if (Array.isArray(args?.entities)) {
        const ops: any[] = [];
        for (const ent of (args.entities as any[]).slice(0, 50)) {
          if (!ent || typeof ent !== "object") continue;
          const id = String(ent?.id ?? "").slice(0, 200);
          const entityKind = String(ent?.entity_kind ?? ent?.entityKind ?? ent?.type ?? ent?.kind ?? "").slice(0, 100);
          if (!id || !entityKind) continue;
          const title = typeof ent?.title === "string" ? String(ent.title).slice(0, 200) : undefined;
          const props = safeObject(ent?.props ?? ent ?? {});
          ops.push({ op: "create", id, entity_kind: entityKind, title, props });
        }
        return ops;
      }
      return [];
    };

    const postClientEvent = async (type: string, data: any) => {
      await apiPostSessionUiEvent(
        effectiveBase,
        {
          session_id: sid,
          type,
          client,
          data,
          append_to_session: true,
        },
        daemonAuth,
      );
    };

    const markApplied = (key: string) => {
      appliedUiActionIdsRef.current[key] = Date.now();
      const appliedKeys = Object.keys(appliedUiActionIdsRef.current);
      if (appliedKeys.length <= appliedUiActionLimit) return;
      const items = appliedKeys
        .map((k) => ({ k, ts: appliedUiActionIdsRef.current[k] || 0 }))
        .sort((a, b) => a.ts - b.ts);
      const overflow = items.length - appliedUiActionLimit;
      for (let i = 0; i < overflow; i += 1) {
        delete appliedUiActionIdsRef.current[items[i].k];
      }
    };

    for (const row of actions) {
      const id = typeof row?.id === "number" ? row.id : Number(row?.id ?? NaN);
      if (!Number.isFinite(id)) continue;
      const key = `${sid}::ui_action_id::${id}`;
      if (appliedUiActionIdsRef.current[key]) continue;

      const action = row?.action ?? {};
      const atype = typeof action?.type === "string" ? action.type : "";
      if (atype !== "client_rpc" && atype !== "collab_rpc" && atype !== "client_probe") continue;
      const toolCallId = typeof row?.tool_call_id === "string" ? String(row.tool_call_id) : "";
      const rpcId = String(action?.rpc_id ?? action?.probe_id ?? toolCallId ?? "").trim();
      if (!rpcId) continue;
      if (ackedRpcIds.has(rpcId)) {
        markApplied(key);
        continue;
      }
      const rpc = action?.rpc ?? action?.probe ?? {};
      const rpcKind = String(rpc?.kind ?? "").trim();
      if (rpcKind !== "entity_apply") continue;
      const autoRunRequested =
        typeof action?.auto_run === "boolean" ? action.auto_run : typeof action?.auto === "boolean" ? action.auto : true;
      if (!autoRunRequested) continue;

      const args = typeof rpc?.args === "object" && rpc?.args ? rpc.args : rpc;
      const ops = entityApplyArgsToOps(args);
      if (!Array.isArray(ops) || ops.length === 0) continue;

      // Mark before executing to prevent loops if the apply throws (we still want to send a failure).
      markApplied(key);
      const t0 = Date.now();
      try {
        const result = applySceneOps(sid, ops);
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: true,
          elapsed_ms: Date.now() - t0,
          result,
        }).catch(() => {});
      } catch (e) {
        void postClientEvent("client_rpc_result", {
          rpc_id: rpcId,
          request_tool_call_id: toolCallId,
          rpc_kind: rpcKind,
          ok: false,
          elapsed_ms: Date.now() - t0,
          error: String(e),
        }).catch(() => {});
      }
    }
  }, [
    allowClientEffects,
    allowClientRpcs,
    applySceneOps,
    client,
    daemonAuth,
    dbClientEvents.data,
    dbUiActions.data,
    effectiveBase,
    sessionId,
  ]);

  // Note: artifacts are mirrored into the durable Scene by the daemon itself (server-side),
  // so the WebUI does not need to synthesize them into client-only scene state.

  // Reliability: post artifact_rendered / artifact_render_failed acknowledgements even when the History panel is collapsed.
  //
  // Agents often use `client_wait_event(type="artifact_rendered", data_match={tool_call_id:...})` as a deterministic DoD.
  // If acknowledgements depend on whether an ArtifactView component is mounted (History expanded), runs can time out.
  // This effect ensures new artifacts are fetch-verified and acknowledged promptly based on session_artifacts polling.
  const artifactAckedRef = React.useRef<Record<string, boolean>>({});
  React.useEffect(() => {
    const sid = String(sessionId || "").trim();
    if (!sid) return;
    const rows = sessionArtifacts.data?.ok && Array.isArray(sessionArtifacts.data?.artifacts) ? (sessionArtifacts.data.artifacts as any[]) : [];
    if (rows.length === 0) return;

    const safeString = (v: any) => (typeof v === "string" ? v : "");
    const isAbsoluteLikePath = (p: string): boolean => {
      const s = (p || "").trim();
      if (!s) return false;
      if (s.startsWith("/")) return true;
      if (/^[a-zA-Z]:[\\/]/.test(s)) return true;
      if (s.startsWith("\\\\")) return true;
      return false;
    };

    const post = async (etype: string, data: any) => {
      await apiPostSessionUiEvent(
        effectiveBase,
        {
          session_id: sid,
          type: etype,
          client,
          data,
          append_to_session: false,
        },
        daemonAuth,
      );
    };

    // Keep bounded: only try to ack a small number of newest artifacts per poll cycle.
    const candidates = rows
      .slice(0, 32)
      .map((rec: any) => {
        const data = rec?.data ?? {};
        const artifact = data?.artifact ?? rec?.artifact ?? {};
        const toolCallId = typeof data?.tool_call_id === "string" ? data.tool_call_id : typeof rec?.tool_call_id === "string" ? rec.tool_call_id : "";
        return { artifact, toolCallId };
      })
      .filter((x) => x.toolCallId && x.artifact && typeof x.artifact === "object")
      .slice(0, 8);

    candidates.forEach((c) => {
      const toolCallId = String(c.toolCallId || "").trim();
      if (!toolCallId) return;
      const key = `${effectiveBase}::${sid}::${toolCallId}`;
      if (artifactAckedRef.current[key]) return;
      artifactAckedRef.current[key] = true;

      const artifact: any = c.artifact;
      const path = safeString(artifact?.path);
      const resolvedPath = safeString(artifact?.resolved_path);
      const kind = safeString(artifact?.kind);
      const title = safeString(artifact?.title) || path || "artifact";

      const preferredFetchPath = path && !isAbsoluteLikePath(path) ? path : yolo && resolvedPath ? resolvedPath : path;
      const fallbackFetchPath =
        yolo && preferredFetchPath === path && path && !isAbsoluteLikePath(path) && resolvedPath && isAbsoluteLikePath(resolvedPath) ? resolvedPath : "";

      void (async () => {
        const tryPaths = [preferredFetchPath, fallbackFetchPath].filter((p) => typeof p === "string" && p.trim().length > 0);
        let lastErr: any = null;
        for (const p of tryPaths) {
          const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
          const src = `${effectiveBase}/api/v1/file?path=${encodeURIComponent(p)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
          try {
            const r = await fetch(src, { headers: daemonHeaders(daemonAuth) });
            if (!r.ok) throw new Error(`file fetch failed: ${r.status}`);
            const ct = String(r.headers.get("content-type") || "").trim();
            // Consume bytes to actually verify fetchability (and avoid keeping the response open).
            await r.arrayBuffer();
            await post("artifact_rendered", {
              path,
              resolved_path: resolvedPath || undefined,
              fetch_path: p,
              kind,
              title,
              tool_call_id: toolCallId,
              content_type: ct || undefined,
            });
            return;
          } catch (e) {
            lastErr = e;
          }
        }
        await post("artifact_render_failed", {
          path,
          resolved_path: resolvedPath || undefined,
          fetch_path: preferredFetchPath || undefined,
          kind,
          title,
          tool_call_id: toolCallId,
          error: String(lastErr || "failed"),
        });
      })().catch(() => {});
    });
  }, [client, daemonAuth, effectiveBase, sessionArtifacts.data, sessionId, yolo]);

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

  return (
    <div
      className="h-screen w-full bg-slate-950 text-white"
      style={{
        ["--topbar-h" as any]: `${topbarHeightPx}px`,
        ["--promptbar-h" as any]: `${promptbarHeightPx}px`,
      }}
    >
      <header ref={topbarRef} className="sticky top-0 z-30 border-b border-white/10 bg-slate-950/80 backdrop-blur">
        <div className="flex h-14 min-w-0 items-center justify-between px-4">
          <div className="min-w-0">
            <div className="text-sm font-semibold">agent UI</div>
            <div className="text-[11px] text-white/60">
              profile:{" "}
              <select
                className="ml-1 inline-block max-w-[28vw] truncate rounded-md border border-white/10 bg-black/30 px-2 py-0.5 font-mono text-[11px] text-white/70"
                value={connection.activeProfileId}
                onChange={(e) => connection.setActiveProfileId(e.target.value)}
                title={profileName}
              >
                {connection.profiles.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.name}
                  </option>
                ))}
              </select>{" "}
              {runSettings.profileOverridesEnabled ? (
                <span className="rounded-full border border-emerald-400/40 bg-emerald-500/10 px-2 py-0.5 text-[10px] font-semibold text-emerald-200">
                  run overrides
                </span>
              ) : null}
              · daemon:{" "}
              <span className="inline-block max-w-[60vw] truncate align-bottom font-mono text-[11px] text-white/70" title={effectiveBase}>
                {effectiveBase}
              </span>{" "}
              {health.isSuccess ? (
                <span className="text-emerald-300">
                  ok ({health.data.service ?? "agentd"} {health.data.version ?? ""})
                </span>
              ) : health.isFetching ? (
                <span className="text-white/60">checking…</span>
              ) : (
                <span className="text-rose-300">offline</span>
              )}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
              onClick={() => health.refetch()}
              type="button"
            >
              Recheck
            </button>
            <button
              className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
              onClick={() => setShowSettings(true)}
              type="button"
            >
              Settings
            </button>
          </div>
        </div>
      </header>

      {health.isError ? (
        <div className="border-b border-amber-500/20 bg-amber-500/10 px-4 py-2 text-xs text-amber-100">
          {connectionMode === "broker" && missingBrokerAuthToken ? (
            <>
              <span className="font-semibold text-amber-50/90">Unauthorized:</span> the broker requires an OIDC bearer token.
              Set it in{" "}
              <button className="underline hover:text-white" onClick={() => setShowSettings(true)} type="button">
                Settings
              </button>{" "}
              (<span className="text-amber-50/90">Broker auth token</span>).
            </>
          ) : (
            <>
              Browser cannot reach <code className="text-amber-50/90">{effectiveBase}</code> (network, TLS, or CORS).
              {webOrigin && connectionMode === "direct" ? (
                <>
                  {" "}
                  If <code className="text-amber-50/90">agentd</code> is running, allow this UI origin:{" "}
                  <code className="text-amber-50/90">{webOrigin}</code> (start agentd with{" "}
                  <code className="text-amber-50/90">--cors-origin {webOrigin}</code>).
                </>
              ) : null}
            </>
          )}
        </div>
      ) : sessionsUnauthorized && missingDaemonAuthToken ? (
        <div className="border-b border-amber-500/20 bg-amber-500/10 px-4 py-2 text-xs text-amber-100">
          <span className="font-semibold text-amber-50/90">Unauthorized:</span> the daemon requires a bearer token.
          Set it in <button className="underline hover:text-white" onClick={() => setShowSettings(true)} type="button">Settings</button>{" "}
          (
          <span className="text-amber-50/90">
            {connectionMode === "broker" ? "Agentd auth token (X-Agentd-Authorization)" : "Daemon auth token"}
          </span>
          ).
          <span className="text-amber-50/80">
            {" "}
            If you started via docker-compose, it’s typically <code className="text-amber-50/90">dev-agentd-token</code>.
          </span>
          {isLocalDaemonBase ? (
            <>
              {" "}
              <button
                className="ml-2 rounded-md border border-amber-500/30 bg-amber-500/10 px-2 py-1 text-[11px] text-amber-50/90 hover:bg-amber-500/15"
                type="button"
                onClick={() => connection.setDaemonAuthToken("dev-agentd-token")}
                title="Convenience for local dev. For production, use a real bearer token."
              >
                Use dev token
              </button>
            </>
          ) : null}
        </div>
      ) : null}

      <main
        ref={(el) => {
          mainScrollRef.current = el;
        }}
        onScroll={onMainScroll}
        className="h-[calc(100vh-var(--topbar-h))] overflow-y-auto px-3 py-3 pb-[var(--promptbar-h)]"
      >
        <div className="mx-auto max-w-7xl">
          <div className="min-h-0">
            <div className="h-[calc(100vh-var(--topbar-h)-var(--promptbar-h)-24px)] min-h-0">
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
          </div>

          <div className="mt-4">
            <TraceLookupPanel
              open={!!traceLookupOpen}
              onToggle={(open) => setTraceLookupOpen(open)}
              traceId={traceLookupId}
              onTraceIdChange={(next) => setTraceLookupId(next)}
              onLoad={(id) => void traceLookup.mutateAsync(id).catch(() => {})}
              onClear={() => {
                setTraceLookupError(null);
                setTraceLookupAgentd(null);
                setTraceLookupBroker(null);
              }}
              loading={traceLookup.isPending}
              error={traceLookupError}
              connectionMode={connectionMode}
              baseUrl={effectiveBase}
              yolo={yolo}
              agentdTrace={traceLookupAgentd}
              brokerTrace={traceLookupBroker}
            />
            <RunDiffPanel
              open={!!runDiffPanelOpen}
              onToggle={(open) => setRunDiffPanelOpen(open)}
              baseUrl={effectiveBase}
              auth={daemonAuth}
            />
            <MemoryPanel
              open={!!memoryPanelOpen}
              onToggle={(open) => setMemoryPanelOpen(open)}
              baseUrl={effectiveBase}
              auth={daemonAuth}
            />
            <ApprovalQueuePanel
              open={!!approvalPanelOpen}
              onToggle={(open) => setApprovalPanelOpen(open)}
              baseUrl={effectiveBase}
              auth={daemonAuth}
            />
            <WorkflowPanel
              open={!!workflowPanelOpen}
              onToggle={(open) => setWorkflowPanelOpen(open)}
              baseUrl={effectiveBase}
              auth={daemonAuth}
              authKey={authKey}
              clientId={client.id}
              workflowDefaults={workflowDefaults}
              workflowTargets={workflowTargets}
              workflowBearerEnv={workflowBearerEnv}
              onTraceIdClick={(traceId) => {
                setTraceLookupId(traceId);
                setTraceLookupOpen(true);
                void traceLookup.mutateAsync(traceId).catch(() => {});
              }}
            />
            {connectionMode === "broker" ? (
                <BrokerPanel
                  open={!!brokerPanelOpen}
                  onToggle={(open) => setBrokerPanelOpen(open)}
                  brokerBase={connection.brokerBase}
                  brokerAgentId={connection.brokerAgentId}
                  setBrokerAgentId={connection.setBrokerAgentId}
                  auth={daemonAuth}
                  authKey={authKey}
                  clientId={client.id}
                />
            ) : null}
            <HistoryPanel
              entries={historyEntriesDesc}
              showAllEntries={showAllHistoryEntries}
              setShowAllEntries={setShowAllHistoryEntries}
              showMessages={showHistoryMessages}
              setShowMessages={setShowHistoryMessages}
              historyExpandedByKey={historyExpandedByKey}
              setHistoryExpandedByKey={setHistoryExpandedByKey}
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
                setTraceLookupOpen(true);
                void traceLookup.mutateAsync(traceId).catch(() => {});
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
        daemonAuth={daemonAuth}
        prompt={prompt}
        setPrompt={setPrompt}
        runDisabled={run.isPending || !!activeJobId}
        runLabel={run.isPending || activeJobId ? "Running…" : "Run"}
        onRun={(vars) => run.mutate(vars)}
        setJobNotice={setJobNotice}
        jobNotice={jobNotice}
        jobError={jobError}
        runError={run.isError ? String(run.error) : null}
        resultError={!result?.ok && result?.error ? result.error : null}
        clearAttachmentsNonce={composerTaskNonce}
        uploadsEnabled={uploadsEnabled}
        uploadMaxBytes={uploadMaxBytes}
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
