import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiGetAudit,
  apiGetConfig,
  apiGetDbRun,
  apiGetDbRuns,
  apiGetDbUiActions,
  apiGetDbMessages,
  apiGetDbClientEvents,
  apiGetSessionClientEvents,
  apiGetHealth,
  apiGetJob,
  apiGetJobProgress,
  apiGetOpenRouterModels,
  apiGetSessionArtifacts,
  apiGetSessionScene,
  apiGetTools,
  apiUpdateDaemonConfig,
  apiDeleteSession,
  apiListSessions,
  apiNewSession,
  apiPostSessionSceneApply,
  apiPostSessionUiEvent,
  apiRun,
  apiRunAsync,
  apiAgentdTrace,
  apiBrokerTrace,
  apiBrokerListAgents,
  daemonHeaders,
  RunRequest,
  RunResponse,
  type ApiAuth,
  type AgentEvent,
} from "./api";
import TraceView from "./components/TraceView";
import EventTimeline from "./components/EventTimeline";
import TraceIdTimelineView from "./components/TraceIdTimelineView";
import Markdown from "./components/Markdown";
import ConversationView from "./components/ConversationView";
import ArtifactView from "./components/ArtifactView";
import DbRunsView from "./components/DbRunsView";
import DbUiActionsView from "./components/DbUiActionsView";
import DbMessagesView from "./components/DbMessagesView";
import DbClientEventsView from "./components/DbClientEventsView";
import SceneView, { type SceneEntity } from "./components/SceneView";
import PromptBar, { type Attachment } from "./components/PromptBar";
import useAutoplayUnlock from "./hooks/useAutoplayUnlock";
import useLocalStorageState from "./hooks/useLocalStorageState";
import { readSseStream } from "./sse";

function Label({ children }: { children: React.ReactNode }) {
  return <div className="text-xs font-medium text-white/70">{children}</div>;
}

function sleep(ms: number) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export default function App() {
  const [showSettings, setShowSettings] = useLocalStorageState("agentui.showSettings", false);
  const [clearAllArmed, setClearAllArmed] = React.useState<boolean>(false);
  const clearAllArmTimeoutRef = React.useRef<number>(0);

  const [connectionMode, setConnectionMode] = useLocalStorageState<"direct" | "broker">("agentui.connectionMode", "direct");

  // Direct mode: browser talks to agentd directly.
  const [base, setBase] = useLocalStorageState("agentui.base", "http://127.0.0.1:8123");

  // Broker mode: browser talks to broker, which proxies to a specific agent_id.
  const [brokerBase, setBrokerBase] = useLocalStorageState("agentui.brokerBase", "https://127.0.0.1:8443");
  const [brokerAgentId, setBrokerAgentId] = useLocalStorageState("agentui.brokerAgentId", "agent1");
  const [brokerAuthToken, setBrokerAuthToken] = useLocalStorageState("agentui.brokerAuthToken", "");

  const webOrigin = React.useMemo(() => {
    try {
      // Used for actionable connectivity hints (CORS).
      return typeof window !== "undefined" ? String(window.location.origin || "") : "";
    } catch {
      return "";
    }
  }, []);

  const [daemonAuthToken, setDaemonAuthToken] = useLocalStorageState("agentui.daemonAuthToken", "");
  const [prompt, setPrompt] = useLocalStorageState("agentui.prompt", "");

  const effectiveBase = React.useMemo(() => {
    const normalizeHttpBase = (raw: string, fallback: string, defaultScheme: "http" | "https") => {
      const b = String(raw || "").trim();
      if (b.length === 0) return fallback;
      const withScheme = /^https?:\/\//i.test(b) ? b : `${defaultScheme}://${b}`;
      return withScheme.replace(/\/+$/, "");
    };

    if (connectionMode === "broker") {
      const bb = normalizeHttpBase(brokerBase, "https://127.0.0.1:8443", "https");
      const aid = String(brokerAgentId || "").trim();
      if (!aid) return bb;
      return `${bb}/v1/agents/${encodeURIComponent(aid)}/proxy`;
    }

    return normalizeHttpBase(base, "http://127.0.0.1:8123", "http");
  }, [base, brokerAgentId, brokerBase, connectionMode]);

  const effectiveSseBase = React.useMemo(() => {
    if (connectionMode !== "broker") return effectiveBase;
    const bb = String(brokerBase || "").trim().replace(/\/+$/, "");
    const withScheme = /^https?:\/\//i.test(bb) ? bb : `https://${bb}`;
    const aid = String(brokerAgentId || "").trim();
    if (!aid) return withScheme;
    return `${withScheme}/v1/agents/${encodeURIComponent(aid)}/proxy_sse`;
  }, [brokerAgentId, brokerBase, connectionMode, effectiveBase]);

  const daemonAuth = React.useMemo<ApiAuth>(() => {
    if (connectionMode === "broker") {
      return { mode: "broker", token: brokerAuthToken, agentdToken: daemonAuthToken };
    }
    return { mode: "direct", token: daemonAuthToken };
  }, [brokerAuthToken, connectionMode, daemonAuthToken]);

  const authKey = React.useMemo(() => {
    const mode = daemonAuth.mode;
    const t = typeof daemonAuth.token === "string" ? daemonAuth.token.trim() : "";
    const at = daemonAuth.mode === "broker" && typeof daemonAuth.agentdToken === "string" ? daemonAuth.agentdToken.trim() : "";
    // Keep stable and non-secret-ish in query keys: include token lengths only (not the raw token).
    return mode === "broker" ? `broker:tlen=${t.length}:alen=${at.length}` : `direct:tlen=${t.length}`;
  }, [daemonAuth]);

  const [brokerAgentsBusy, setBrokerAgentsBusy] = React.useState<boolean>(false);
  const [brokerAgentsError, setBrokerAgentsError] = React.useState<string | null>(null);
  const [brokerAgents, setBrokerAgents] = React.useState<any[] | null>(null);

  const [clientId] = useLocalStorageState(
    "agentui.clientId",
    (() => {
      try {
        // Stable per-browser-profile id (shared across tabs); used as client.id.
        // A per-tab instance id is generated separately.
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const g: any = typeof globalThis !== "undefined" ? globalThis : {};
        if (g.crypto && typeof g.crypto.randomUUID === "function") {
          return `webui-${g.crypto.randomUUID()}`;
        }
      } catch {
        // ignore
      }
      return `webui-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    })(),
  );
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
  const [tools, setTools] = useLocalStorageState<"host" | "basic" | "none">("agentui.tools", "host");
  const [yolo, setYolo] = useLocalStorageState("agentui.yolo", true);
  const [hostPolicy, setHostPolicy] = useLocalStorageState<"full" | "readonly">("agentui.hostPolicy", "full");
  // Production UX: tool visibility should be on by default so users can audit shell/proc commands and tool outputs.
  const [verbose, setVerbose] = useLocalStorageState("agentui.verbose", true);
  const [model, setModel] = useLocalStorageState("agentui.model", "deepseek-chat");
  const [summaryModel, setSummaryModel] = useLocalStorageState("agentui.summaryModel", "");
  const [summaryMaxChars, setSummaryMaxChars] = useLocalStorageState("agentui.summaryMaxChars", "1200");
  const [baseUrl, setBaseUrl] = useLocalStorageState("agentui.baseUrl", "https://api.deepseek.com");
  const [apiKey, setApiKey] = useLocalStorageState("agentui.apiKey", "");
  // Many dev environments require a local HTTP proxy for outbound HTTPS (and this repo's test scripts assume it).
  // Users can clear this if their environment does not need a proxy.
  const [proxyUrl, setProxyUrl] = useLocalStorageState("agentui.proxyUrl", "http://localhost:8120");
  const [timeoutMs, setTimeoutMs] = useLocalStorageState("agentui.timeoutMs", "60000");
  const [maxCaptureBytes, setMaxCaptureBytes] = useLocalStorageState("agentui.maxCaptureBytes", "65536");
  const [streamAssistant, setStreamAssistant] = useLocalStorageState("agentui.streamAssistant", false);
  const [orMinTotal, setOrMinTotal] = useLocalStorageState("agentui.orMinTotal", "0.01");
  const [orMaxTotal, setOrMaxTotal] = useLocalStorageState("agentui.orMaxTotal", "0.50");
  const [orRequireMultimodal, setOrRequireMultimodal] = useLocalStorageState("agentui.orRequireMultimodal", true);
  const [orRequireTools, setOrRequireTools] = useLocalStorageState("agentui.orRequireTools", true);
  const [orLimit, setOrLimit] = useLocalStorageState("agentui.orLimit", "50");
  // Blank means "use daemon default" (which may be unlimited). If you want a hard stop, set an explicit limit.
  // Explicit `0` is still supported and means unlimited.
  const [maxSteps, setMaxSteps] = useLocalStorageState("agentui.maxSteps", "");
  const [maxStepsUserSet, setMaxStepsUserSet] = useLocalStorageState("agentui.maxStepsUserSet", false);
  const [maxRepeatedToolCalls, setMaxRepeatedToolCalls] = useLocalStorageState("agentui.maxRepeatedToolCalls", "0");
  const [maxToolCallsTotal, setMaxToolCallsTotal] = useLocalStorageState("agentui.maxToolCallsTotal", "");
  const [maxToolCallsPerTool, setMaxToolCallsPerTool] = useLocalStorageState("agentui.maxToolCallsPerTool", "");
  const [toolCallLimits, setToolCallLimits] = useLocalStorageState("agentui.toolCallLimits", "");
  const [maxChars, setMaxChars] = useLocalStorageState("agentui.maxChars", "20000");
  const [keepLast, setKeepLast] = useLocalStorageState("agentui.keepLast", "16");
  const [trace, setTrace] = useLocalStorageState("agentui.trace", true);
  const [useAsync, setUseAsync] = useLocalStorageState("agentui.useAsync", true);
  const [showDebugInConversation, setShowDebugInConversation] = useLocalStorageState(
    "agentui.showDebugInConversation",
    true,
  );
  const [traceLookupId, setTraceLookupId] = useLocalStorageState("agentui.traceLookupId", "");
  const [traceLookupOpen, setTraceLookupOpen] = useLocalStorageState("agentui.traceLookupOpen", false);
  const [allowAutoplay, setAllowAutoplay] = useLocalStorageState("agentui.allowAutoplay", true);
  useAutoplayUnlock(allowAutoplay);
  // This project treats the Web UI as a collaboration surface (a “scene”) where the agent is expected
  // to act with side-effects by default. Users can still disable these via Settings (persisted).
  const [allowClientRpcs, setAllowClientRpcs] = useLocalStorageState("agentui.allowClientRpcs", true);
  const [allowClientEffects, setAllowClientEffects] = useLocalStorageState("agentui.allowClientEffects", true);
  const [allowUnsafePageEval, setAllowUnsafePageEval] = useLocalStorageState("agentui.allowUnsafePageEval", true);
  const [dbRunsOnlyErrors, setDbRunsOnlyErrors] = useLocalStorageState("agentui.dbRunsOnlyErrors", true);
  const [dbRunsStopReason, setDbRunsStopReason] = useLocalStorageState("agentui.dbRunsStopReason", "");
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

  const topbarRef = React.useRef<HTMLElement | null>(null);
  const promptbarRef = React.useRef<HTMLDivElement | null>(null);
  const [topbarHeightPx, setTopbarHeightPx] = React.useState<number>(56);
  const [promptbarHeightPx, setPromptbarHeightPx] = React.useState<number>(220);

  // Session-scoped client-side scene entities (collaboration surface objects).
  const sceneBySessionRef = React.useRef<Record<string, Record<string, SceneEntity>>>({});
  const [sceneVersion, setSceneVersion] = React.useState<number>(0);

  const loadJson = (raw: string): any => {
    try {
      const v = JSON.parse(String(raw || ""));
      return v && typeof v === "object" ? v : null;
    } catch {
      return null;
    }
  };

  // Session selection is scoped by daemon base URL.
  // This avoids "lost session" issues when switching between multiple local agentd instances (different ports).
  const [sessionByBaseJson, setSessionByBaseJson] = useLocalStorageState("agentui.sessionByBase", "{}");
  const sessionId = React.useMemo(() => {
    const m = (loadJson(sessionByBaseJson) as Record<string, any>) || {};
    const sid = typeof m?.[effectiveBase] === "string" ? String(m[effectiveBase]) : "";
    return sid.trim().length > 0 ? sid.trim() : "default";
  }, [effectiveBase, sessionByBaseJson]);

  const historyUiKey = React.useMemo(() => {
    const sid = String(sessionId || "").trim();
    return `agentui.historyUi:${effectiveBase}::${sid}`;
  }, [effectiveBase, sessionId]);
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
      setSessionByBaseJson((prevRaw) => {
        const prev = (loadJson(String(prevRaw || "")) as Record<string, any>) || {};
        const next = { ...prev, [effectiveBase]: nextSid };
        try {
          return JSON.stringify(next);
        } catch {
          return JSON.stringify(prev);
        }
      });
    },
    [effectiveBase, setSessionByBaseJson],
  );

  const sceneEntities = React.useMemo(() => {
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    const m = (sid && sceneBySessionRef.current[sid]) || {};
    return Object.values(m);
  }, [sceneVersion, sessionId]);

  // Migration: older UI versions stored a single global `agentui.sessionId` not scoped by base URL.
  // If present, use it as the initial session for the current daemon base.
  React.useEffect(() => {
    if (typeof window === "undefined" || !window.localStorage) return;
    const key = "agentui.sessionId";
    const raw = window.localStorage.getItem(key);
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
    let parsed: Record<string, any> = {};
    try {
      const v = JSON.parse(String(sessionByBaseJson || ""));
      parsed = v && typeof v === "object" ? (v as any) : {};
    } catch {
      parsed = {};
    }
    const m = parsed;
    const have = typeof m?.[effectiveBase] === "string" && String(m[effectiveBase]).trim().length > 0;
    if (have) return;
    setSessionId(legacyTrim);
  }, [effectiveBase, sessionByBaseJson, setSessionId]);

  const parseJobsBySession = React.useCallback(() => {
    const v = loadJson(jobsBySessionJson);
    return v && typeof v === "object" ? (v as Record<string, any>) : {};
  }, [jobsBySessionJson]);

  const writeJobsBySession = React.useCallback(
    (mutate: (prev: Record<string, any>) => Record<string, any>) => {
      setJobsBySessionJson((prevRaw) => {
        const prev = (loadJson(String(prevRaw || "")) as Record<string, any>) || {};
        const next = mutate(prev);
        try {
          return JSON.stringify(next);
        } catch {
          return JSON.stringify(prev);
        }
      });
    },
    [setJobsBySessionJson],
  );

  // Track the last observed daemon-updated scene timestamp per session so refresh/polling is stable.
  const lastSceneUpdatedMsRef = React.useRef<Record<string, number>>({});

  const applySceneOps = React.useCallback(
    (sid: string, ops: any[]) => {
      const sessionKey = String(sid || "").trim();
      if (!sessionKey) throw new Error("missing session_id for scene ops");
      if (!sceneBySessionRef.current[sessionKey]) sceneBySessionRef.current[sessionKey] = {};
      const store = sceneBySessionRef.current[sessionKey];

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
        void apiPostSessionSceneApply(effectiveBase, { session_id: sessionKey, ops: persistOps }, daemonAuth)
          .then((r) => {
            if (!r || r.ok !== true) return;
            const updated = typeof r.updated_unix_ms === "number" ? r.updated_unix_ms : 0;
            const key = `${effectiveBase}::${sessionKey}`;
            if (updated > 0) lastSceneUpdatedMsRef.current[key] = Math.max(lastSceneUpdatedMsRef.current[key] || 0, updated);
            // If the daemon returns a scene snapshot, prefer it (authoritative).
            const scene = r.scene && typeof r.scene === "object" && !Array.isArray(r.scene) ? (r.scene as any) : null;
            if (scene) {
              sceneBySessionRef.current[sessionKey] = scene;
              setSceneVersion((v) => v + 1);
            }
          })
          .catch(() => {});
      }
      return { ok: true, results, count: Object.keys(store).length };
    },
    [daemonAuth, effectiveBase, setSceneVersion],
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
            { kind: "navigate", side_effects: true, description: "Navigate the browser to a new URL (likely reloads the app)." },
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
    const rec = jobs[sid];
    const jobId = typeof rec?.job_id === "string" ? rec.job_id : "";
    if (!jobId) return;
    const cursor = typeof rec?.cursor === "number" && Number.isFinite(rec.cursor) && rec.cursor >= 0 ? Math.floor(rec.cursor) : 0;

    let cancelled = false;
    (async () => {
      try {
        const job = await apiGetJob(effectiveBase, jobId, daemonAuth);
        if (cancelled) return;
        if (!job.ok) {
          writeJobsBySession((prev) => {
            const next = { ...prev };
            delete next[sid];
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
          delete next[sid];
          return next;
        });
      } catch {
        // ignore; user can retry by reloading or the daemon UI.
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [activeJobId, daemonAuth, effectiveBase, parseJobsBySession, sessionId, writeJobsBySession]);

  const health = useQuery({
    queryKey: ["health", effectiveBase, authKey],
    queryFn: () => apiGetHealth(effectiveBase, daemonAuth),
    retry: 1,
  });

  // Migration: older UI versions defaulted `maxSteps` to "0" (unlimited).
  // If the user never explicitly set a value, move to blank so daemon defaults apply.
  React.useEffect(() => {
    if (maxStepsUserSet) return;
    if (String(maxSteps) === "0") {
      setMaxSteps("");
    }
  }, [maxSteps, maxStepsUserSet, setMaxSteps]);

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

  React.useEffect(() => {
    if (!showSettings) setClearAllArmed(false);
  }, [showSettings]);

  React.useEffect(() => {
    return () => {
      if (clearAllArmTimeoutRef.current) {
        try {
          window.clearTimeout(clearAllArmTimeoutRef.current);
        } catch {
          // ignore
        }
      }
      clearAllArmTimeoutRef.current = 0;
    };
  }, []);

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
    const key = `${effectiveBase}::${sid}`;
    const hasPrev = Object.prototype.hasOwnProperty.call(lastSceneUpdatedMsRef.current, key);
    const prev = hasPrev ? lastSceneUpdatedMsRef.current[key] || 0 : -1;
    if (hasPrev && updated <= prev) return;

    const scene = (sessionScene.data as any)?.scene;
    if (!scene || typeof scene !== "object" || Array.isArray(scene)) return;
    sceneBySessionRef.current[sid] = scene as any;
    lastSceneUpdatedMsRef.current[key] = updated;
    setSceneVersion((v) => v + 1);
  }, [effectiveBase, sessionId, sessionScene.data]);

  const dbRuns = useQuery({
    queryKey: ["db_runs", effectiveBase, authKey, sessionId, dbRunsOnlyErrors, dbRunsStopReason],
    queryFn: () =>
      apiGetDbRuns(effectiveBase, sessionId, daemonAuth, {
        limit: 50,
        offset: 0,
        onlyErrors: dbRunsOnlyErrors,
        stopReason: dbRunsStopReason,
      }),
    enabled: false,
    retry: 1,
  });

  const dbUiActions = useQuery({
    queryKey: ["db_ui_actions", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetDbUiActions(effectiveBase, sessionId, daemonAuth, { limit: 100, offset: 0 }),
    enabled: !!sessionId && allowClientRpcs && allowClientEffects,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const dbMessages = useQuery({
    queryKey: ["db_messages", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetDbMessages(effectiveBase, sessionId, daemonAuth, { limit: 80, offset: 0, maxContentBytes: 8192 }),
    enabled: false,
    retry: 1,
  });

  const dbClientEvents = useQuery({
    queryKey: ["db_client_events", effectiveBase, authKey, sessionId],
    queryFn: () => apiGetDbClientEvents(effectiveBase, sessionId, daemonAuth, { limit: 100, offset: 0 }),
    enabled: !!sessionId && allowClientRpcs && allowClientEffects,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const [selectedDbRunId, setSelectedDbRunId] = React.useState<number | null>(null);
  const dbRunDetail = useQuery({
    queryKey: ["db_run", effectiveBase, authKey, selectedDbRunId],
    queryFn: () =>
      selectedDbRunId
        ? apiGetDbRun(effectiveBase, selectedDbRunId, daemonAuth, {
            includeEvents: true,
            includeTools: true,
            includeArtifacts: true,
            includeUiActions: true,
          })
        : Promise.resolve({ ok: false, error: "no run selected" }),
    enabled: selectedDbRunId !== null,
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
        void dbMessages.refetch();
        void dbClientEvents.refetch();
        setSelectedDbRunId(null);
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
  const [openrouterModels, setOpenrouterModels] = React.useState<any | null>(null);
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
      if (useAsync) {
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
          [sid]: { job_id: v.job.job_id, cursor: 0, started_unix_ms: Date.now() },
        }));
      }
    },
    onError: (e) => {
      // Keep the last conversation visible when a run cannot be started.
      setJobError(`run failed: ${String(e)}`);
      setJobNotice(null);
    },
  });

  const fetchOpenRouterModels = useMutation({
    mutationFn: async () => {
      const minTotal = Number(orMinTotal);
      const maxTotal = Number(orMaxTotal);
      const limit = Number(orLimit);
      return apiGetOpenRouterModels(effectiveBase, {
        daemonAuth: daemonAuth,
        apiKey: apiKey || undefined,
        openrouterBaseUrl: "https://openrouter.ai/api/v1",
        minTotal: Number.isFinite(minTotal) ? minTotal : 0.01,
        maxTotal: Number.isFinite(maxTotal) ? maxTotal : 0.5,
        requireMultimodalInput: orRequireMultimodal,
        requireTools: orRequireTools,
        includeFree: false,
        limit: Number.isFinite(limit) ? limit : 50,
        refresh: true,
      });
    },
    onSuccess: (v) => {
      setOpenrouterModels(v);
      if (v.ok && v.recommended_model && typeof v.recommended_model === "string" && v.recommended_model.length > 0) {
        // Convenience: prime the model field with the recommended choice.
        setModel(v.recommended_model);
        setBaseUrl("https://openrouter.ai/api/v1");
      }
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
        const bb = String(brokerBase || "").trim();
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
        const cur = prev[sid];
        if (!cur || cur.job_id !== jobId) return prev;
        if (cur.cursor === cursor) return prev;
        return { ...prev, [sid]: { ...cur, cursor, updated_unix_ms: Date.now() } };
      });
    }, 1000);
    return () => {
      try {
        window.clearInterval(t);
      } catch {
        // ignore
      }
    };
  }, [activeJobId, sessionId, writeJobsBySession]);

  // Execute any persisted entity_apply RPCs from DB ui_actions that have not yet been acknowledged.
  // This makes client RPC execution reliable across refreshes and SSE dropouts.
  const appliedUiActionIdsRef = React.useRef<Record<string, boolean>>({});
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
        appliedUiActionIdsRef.current[key] = true;
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
      appliedUiActionIdsRef.current[key] = true;
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

  React.useEffect(() => {
    if (!activeJobId) return;
    let cancelled = false;
    const jobId = activeJobId;
    let watchdogTimer: any = null;

    const startPolling = () => {
      (async () => {
        // Poll job progress + stream events via cursor.
        for (;;) {
          if (cancelled) return;
          let job: any;
          try {
            job = await apiGetJobProgress(effectiveBase, jobId, daemonAuth, { cursor: cursorRef.current, maxEvents: 256 });
          } catch (e) {
            // Transient fetch failures should not invalidate the visible conversation.
            // Keep the current liveEvents and keep the job active; retry with backoff.
            setJobNotice(`job fetch failed (retrying): ${String(e)}`);
            await sleep(1000);
            continue;
          }

          if (cancelled) return;
          setJobStatus(job.status ?? null);
          setJobError(job.status === "error" || job.status === "interrupted" ? (job.error ?? null) : null);
          setJobNotice(null);
          setJobUpdatedMs(typeof job.updated_unix_ms === "number" ? job.updated_unix_ms : null);

          const ev = Array.isArray(job.events) ? job.events : [];
          const next = typeof job.events_cursor_next === "number" ? job.events_cursor_next : cursorRef.current + ev.length;
          if (ev.length > 0) {
            if (job.events_reset) {
              setLiveEvents(ev);
            } else {
              setLiveEvents((prev) => prev.concat(ev));
            }
            cursorRef.current = next;
          }

          if (job.status === "done" || job.status === "error" || job.status === "cancelled" || job.status === "interrupted") {
            if (job.result) {
              setResult(job.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
            } else {
              setJobError("job completed but missing result");
            }
            setJobNotice(null);
            setActiveJobId(null);
            const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[sid];
                return nextm;
              });
            }
            void auditRefetch();
            void sessionsRefetch();
            return;
          }

          await sleep(500);
        }
      })().catch((e) => {
        if (cancelled) return;
        // Keep job state visible; do not invalidate the conversation on unexpected polling loop errors.
        setJobNotice(`polling loop failed (retrying): ${String(e)}`);
      });
    };

    // Prefer SSE streaming when available. Fall back to polling.
    let es: EventSource | null = null;
    let fetchAbort: AbortController | null = null;
    let fetchFinished = false;
    const canUseEventSource =
      typeof EventSource !== "undefined" &&
      connectionMode === "direct" &&
      (!daemonAuthToken || daemonAuthToken.trim().length === 0) &&
      typeof effectiveSseBase === "string" &&
      (effectiveSseBase.startsWith("http://") || effectiveSseBase.startsWith("https://"));
    const canUseFetchSse =
      typeof fetch !== "undefined" &&
      typeof effectiveSseBase === "string" &&
      (effectiveSseBase.startsWith("http://") || effectiveSseBase.startsWith("https://"));

    let fallbackStarted = false;
    const fallbackToPolling = () => {
      if (fallbackStarted) return;
      fallbackStarted = true;
      startPolling();
    };

    // Watchdog: even if SSE/streaming is flaky, ensure we eventually observe terminal state.
    // This avoids "stuck Running…" UIs when the stream drops before a job_done event.
    watchdogTimer = setInterval(() => {
      if (cancelled) return;
      void (async () => {
        try {
          const job = await apiGetJob(effectiveBase, jobId, daemonAuth);
          if (!job?.ok) return;
          if (job.status === "done" || job.status === "error" || job.status === "cancelled" || job.status === "interrupted") {
            // Trigger the same completion path as streaming.
            if (job.result) {
              setResult(job.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
              setJobStatus(job.status);
              setJobError(job.status === "error" || job.status === "interrupted" ? (job.error ?? null) : null);
              setJobNotice(null);
            } else {
              setJobError("job completed but missing result");
              setJobNotice(null);
            }
            setActiveJobId(null);
            const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[sid];
                return nextm;
              });
            }
            void auditRefetch();
            void sessionsRefetch();
          }
        } catch {
          // ignore; watchdog is best-effort
        }
      })();
    }, 3000);

    const startFetchSse = () => {
      const url = `${effectiveSseBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
        String(cursorRef.current),
      )}`;
      const controller = new AbortController();
      fetchAbort = controller;
      setJobStatus("running");
      setJobUpdatedMs(null);

      (async () => {
        const resp = await fetch(url, {
          headers: daemonHeaders(daemonAuth),
          signal: controller.signal,
        });
        if (!resp.ok) {
          throw new Error(`SSE fetch failed: HTTP ${resp.status}`);
        }
        await readSseStream(resp, (evt) => {
          if (cancelled) return;
          if (evt.id && evt.id.length > 0) {
            const n = Number(evt.id);
            if (Number.isFinite(n)) cursorRef.current = n + 1;
          }
          if (evt.event === "reset") {
            try {
              const data = JSON.parse(String(evt.data || "{}"));
              if (typeof data?.cursor_base === "number") {
                cursorRef.current = data.cursor_base;
              }
            } catch {
              // ignore
            }
            setLiveEvents([]);
          } else if (evt.event === "agent_event") {
            try {
              const ev = JSON.parse(String(evt.data || "{}"));
              setLiveEvents((prev) => prev.concat(ev));
            } catch {
              // ignore malformed
            }
          } else if (evt.event === "job_done") {
            if (cancelled) return;
            try {
              const data = JSON.parse(String(evt.data || "{}"));
              setJobStatus(typeof data?.status === "string" ? data.status : "done");
              setJobError(
                typeof data?.status === "string" && (data.status === "error" || data.status === "interrupted")
                  ? (typeof data?.error === "string" ? data.error : null)
                  : null,
              );
              setJobNotice(null);
              if (data?.result) {
                setResult(data.result);
                setLastCompletedPrompt(lastRunPromptRef.current);
              }
            } catch {
              setJobNotice("failed to parse job_done event (falling back to polling)");
            }
            fetchFinished = true;
            setActiveJobId(null);
            const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[sid];
                return nextm;
              });
            }
            void auditRefetch();
            void sessionsRefetch();
          }
        });
        if (!cancelled && !fetchFinished) {
          // Stream ended without a terminal job_done event; fall back to polling so the UI still progresses.
          fallbackToPolling();
        }
      })().catch((e) => {
        if (cancelled || fetchFinished) return;
        setJobNotice(`job stream dropped (retrying via polling): ${String(e)}`);
        fallbackToPolling();
      });
    };

    if (canUseEventSource) {
      try {
        const url = `${effectiveSseBase}/api/v1/job/stream?job_id=${encodeURIComponent(jobId)}&cursor=${encodeURIComponent(
          String(cursorRef.current),
        )}`;
        es = new EventSource(url);
        setJobStatus("running");
        setJobUpdatedMs(null);
        es.onopen = () => {
          // ok
        };
        es.addEventListener("reset", (evt: any) => {
          if (cancelled) return;
          try {
            const data = JSON.parse(String(evt.data || "{}"));
            if (typeof data?.cursor_base === "number") {
              cursorRef.current = data.cursor_base;
            }
          } catch {
            // ignore
          }
          setLiveEvents([]);
        });
        es.addEventListener("agent_event", (evt: any) => {
          if (cancelled) return;
          try {
            const ev = JSON.parse(String(evt.data || "{}"));
            if (evt && typeof evt.lastEventId === "string" && evt.lastEventId.length > 0) {
              const n = Number(evt.lastEventId);
              if (Number.isFinite(n)) cursorRef.current = n + 1;
            }
            setLiveEvents((prev) => prev.concat(ev));
          } catch {
            // ignore malformed
          }
        });
        es.addEventListener("job_done", (evt: any) => {
          if (cancelled) return;
          try {
            const data = JSON.parse(String(evt.data || "{}"));
            setJobStatus(typeof data?.status === "string" ? data.status : "done");
            setJobError(
              typeof data?.status === "string" && (data.status === "error" || data.status === "interrupted")
                ? (typeof data?.error === "string" ? data.error : null)
                : null,
            );
            setJobNotice(null);
            if (data?.result) {
              setResult(data.result);
              setLastCompletedPrompt(lastRunPromptRef.current);
            }
          } catch {
            setJobNotice("failed to parse job_done event (falling back to polling)");
          }
          setActiveJobId(null);
          const sid = String(sessionId || "").trim();
            if (sid) {
              writeJobsBySession((prev) => {
                const nextm = { ...prev };
                delete nextm[sid];
                return nextm;
              });
            }
            try {
              es?.close();
            } catch {
              // ignore
            }
            void auditRefetch();
            void sessionsRefetch();
          });
        es.onerror = () => {
          if (cancelled) return;
          try {
            es?.close();
          } catch {
            // ignore
          }
          // Any SSE failure should fall back to polling (cursor-based) so the UI keeps progressing.
          // Do not clear liveEvents; keep the conversation visible.
          setJobNotice("job stream dropped (retrying via polling)");
          fallbackToPolling();
        };
      } catch {
        fallbackToPolling();
      }
    } else if (canUseFetchSse) {
      startFetchSse();
    } else {
      fallbackToPolling();
    }

    return () => {
      cancelled = true;
      try {
        if (watchdogTimer) clearInterval(watchdogTimer);
      } catch {
        // ignore
      }
      try {
        es?.close();
      } catch {
        // ignore
      }
      try {
        fetchAbort?.abort();
      } catch {
        // ignore
      }
    };
  }, [activeJobId, auditRefetch, connectionMode, daemonAuth, daemonAuthToken, effectiveBase, effectiveSseBase, sessionId, sessionsRefetch]);

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
              daemon:{" "}
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
                onClick={() => setDaemonAuthToken("dev-agentd-token")}
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
            <details
              className="mb-4 rounded-lg border border-white/10 bg-white/5 px-3 py-2"
              open={!!traceLookupOpen}
              onToggle={(ev) => setTraceLookupOpen((ev.currentTarget as HTMLDetailsElement).open)}
            >
              <summary className="cursor-pointer select-none text-xs text-white/80">
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <div className="font-semibold text-white/80">Trace lookup</div>
                  <div className="text-[11px] text-white/50">
                    Debug a distributed run by <code className="text-white/60">trace_id</code>
                  </div>
                </div>
              </summary>
              <div className="mt-3 grid gap-3">
                <div className="flex flex-wrap items-center gap-2">
                  <input
                    className="min-w-[260px] flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/90 placeholder:text-white/40"
                    placeholder="trace_id (e.g. trace_...)"
                    value={traceLookupId}
                    onChange={(e) => setTraceLookupId(e.target.value)}
                  />
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    disabled={traceLookup.isPending || String(traceLookupId || "").trim().length === 0}
                    onClick={() => void traceLookup.mutateAsync(traceLookupId)}
                    title={
                      connectionMode === "broker"
                        ? "Calls broker /v1/trace (includes relay audits + agent fanout)."
                        : "Calls agentd /api/v1/trace (audit record search)."
                    }
                  >
                    {traceLookup.isPending ? "Loading…" : "Load"}
                  </button>
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    disabled={traceLookup.isPending}
                    onClick={() => {
                      setTraceLookupError(null);
                      setTraceLookupAgentd(null);
                      setTraceLookupBroker(null);
                    }}
                  >
                    Clear
                  </button>
                </div>

                {traceLookupError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                    {traceLookupError}
                  </div>
                ) : null}

                {String(traceLookupId || "").trim() && (traceLookupAgentd || traceLookupBroker) ? (
                  <TraceIdTimelineView
                    mode={connectionMode}
                    baseUrl={effectiveBase}
                    yolo={yolo}
                    traceId={String(traceLookupId || "").trim()}
                    agentdTrace={traceLookupAgentd}
                    brokerTrace={traceLookupBroker}
                  />
                ) : null}
              </div>
            </details>

            <div className="mb-2 flex items-center justify-between gap-2">
              <div className="text-sm font-semibold text-white/80">History</div>
              <div className="flex items-center gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                  type="button"
                  onClick={() => setShowHistoryMessages((v) => !v)}
                  title={showHistoryMessages ? "Hide per-run event transcript" : "Show per-run event transcript"}
                >
                  {showHistoryMessages ? "Hide messages" : "Show messages"}
                </button>
                {historyEntriesDesc.length > 1 ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() => setShowAllHistoryEntries((v) => !v)}
                    title={showAllHistoryEntries ? "Hide older history entries" : "Show older history entries"}
                  >
                    {showAllHistoryEntries ? `Hide history (${historyEntriesDesc.length - 1})` : `Show history (${historyEntriesDesc.length - 1})`}
                  </button>
                ) : null}
              </div>
            </div>
            {historyEntriesDesc.length === 0 ? (
              <div className="rounded-md border border-white/10 bg-black/20 px-3 py-3 text-xs text-white/60">
                No history yet. Run a prompt to populate the timeline.
              </div>
            ) : (
              <div className="grid gap-2">
                {(showAllHistoryEntries ? historyEntriesDesc : historyEntriesDesc.slice(0, 1)).map((e: any, idx: number) => {
                  const ts = typeof e?.ts_unix_ms === "number" ? e.ts_unix_ms : 0;
                  const when = ts ? new Date(ts).toLocaleString() : "";
                  const promptText = typeof e?.prompt === "string" ? e.prompt : "";
                  const assistantText = typeof e?.assistant_text === "string" ? e.assistant_text : "";
                  const evs = Array.isArray(e?.events) ? (e.events as any[]) : [];
                  const ok = typeof e?.ok === "boolean" ? e.ok : undefined;
                  const isLive = e?.live === true;
                  const jobId = typeof e?.job_id === "string" ? e.job_id : "";
                  const jobSt = typeof e?.job_status === "string" ? e.job_status : "";
                  const traceId = typeof e?.trace_id === "string" ? e.trace_id : "";
                  const status = ok === true ? "ok" : ok === false ? "error" : "";
                  const summary = promptText.trim().length > 0 ? promptText.trim().slice(0, 200) : "(no prompt)";
                  const entryKey = isLive && jobId ? `job:${jobId}` : `ts:${String(ts || 0)}`;
                  const expanded =
                    Object.prototype.hasOwnProperty.call(historyExpandedByKey, entryKey) ? !!historyExpandedByKey[entryKey] : idx === 0;

                  return (
                    <details
                      key={entryKey}
                      open={expanded}
                      onToggle={(ev) => {
                        const open = (ev.currentTarget as HTMLDetailsElement).open;
                        setHistoryExpandedByKey((prev) => ({ ...(prev || {}), [entryKey]: open }));
                      }}
                      className="rounded-lg border border-white/10 bg-white/5 px-3 py-2"
                    >
                      <summary className="cursor-pointer select-none text-xs text-white/80">
                        <div className="flex min-w-0 flex-wrap items-baseline gap-x-2 gap-y-1">
                          <span className="shrink-0 text-white/60">{when}</span>
                          {isLive ? (
                            <span className="shrink-0 text-indigo-300">
                              running{jobSt ? ` (${jobSt})` : ""}
                              {jobId ? (
                                <>
                                  {" "}
                                  <code className="inline-block max-w-[50vw] truncate align-bottom text-indigo-200/80" title={jobId}>
                                    {jobId}
                                  </code>
                                </>
                              ) : null}
                            </span>
                          ) : null}
                          {status ? (
                            <span className={`${status === "ok" ? "text-emerald-300" : "text-rose-300"}`}>{status}</span>
                          ) : null}
                          {traceId ? (
                            <button
                              className="shrink-0 rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                              type="button"
                              title="Open trace lookup for this run"
                              onClick={(ev) => {
                                ev.preventDefault();
                                ev.stopPropagation();
                                setTraceLookupId(traceId);
                                setTraceLookupOpen(true);
                                void traceLookup.mutateAsync(traceId).catch(() => {});
                              }}
                            >
                              Trace
                            </button>
                          ) : null}
                          <span className="min-w-0 flex-1">{summary}</span>
                          <span className="shrink-0 text-white/40">({evs.length} events)</span>
                        </div>
                      </summary>
                      <div className="mt-3 grid gap-3">
                        {assistantText ? (
                          <div className="rounded-md border border-white/10 bg-black/20 px-3 py-2">
                            <div className="flex items-start gap-2">
                              <div className="shrink-0 text-[11px] font-semibold text-white/60">Assistant</div>
                              <div className="min-w-0 flex-1">
                                <Markdown text={assistantText} />
                              </div>
                            </div>
                          </div>
                        ) : null}
                        {evs.length > 0 && (isLive || showHistoryMessages) ? (
                          <ConversationView
                            baseUrl={effectiveBase}
                            yolo={yolo}
                            sessionId={sessionId}
                            client={client}
                            daemonAuth={daemonAuth}
                            prompt={promptText}
                            events={evs as any}
                            showDebugEvents={showDebugInConversation}
                            allowAutoplay={allowAutoplay}
                            allowClientRpcs={allowClientRpcs}
                            allowClientEffects={allowClientEffects}
                            allowUnsafePageEval={allowUnsafePageEval}
                            reverseOrder={true}
                            disableAutoClientRpcs={idx !== 0}
                            sceneEntities={sceneEntities}
                            onSceneApply={(ops) => applySceneOps(String(sessionId || "").trim(), ops)}
                          />
                        ) : null}
                      </div>
                    </details>
                  );
                })}
              </div>
            )}
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
      />

      {showSettings ? (
        <div className="fixed inset-0 z-40">
          <div
            className="absolute inset-0 bg-black/60"
            onClick={() => setShowSettings(false)}
            role="button"
            tabIndex={0}
          />
          <div className="absolute right-0 top-0 h-full w-[440px] max-w-[92vw] overflow-auto border-l border-white/10 bg-slate-950 p-4">
            <div className="flex items-center justify-between gap-3">
              <div className="text-sm font-semibold">Settings</div>
              <button
                className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                onClick={() => setShowSettings(false)}
                type="button"
              >
                Close
              </button>
            </div>

            <div className="mt-4">
              <Label>Connection</Label>
              <select
                className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                value={connectionMode}
                onChange={(e) => setConnectionMode(e.target.value as any)}
              >
                <option value="direct">direct (agentd)</option>
                <option value="broker">broker (OIDC + agent_id)</option>
              </select>
              <div className="mt-2 text-[11px] text-white/60">
                {connectionMode === "direct"
                  ? "Direct: the browser calls agentd over HTTP."
                  : "Broker: the browser calls a broker (OIDC), which proxies to a connected agent by id."}
              </div>
            </div>

            {connectionMode === "direct" ? (
              <>
                <div className="mt-4">
                  <Label>Daemon base URL</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    data-testid="daemon-base"
                    value={base}
                    onChange={(e) => setBase(e.target.value)}
                  />
                </div>

                <div className="mt-4">
                  <Label>Daemon auth token (optional)</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    data-testid="daemon-auth-token"
                    placeholder='Bearer token (e.g. "dev-agentd-token" in docker-compose)'
                    value={daemonAuthToken}
                    onChange={(e) => setDaemonAuthToken(e.target.value)}
                  />
                </div>
              </>
            ) : (
              <>
                <div className="mt-4">
                  <Label>Broker base URL</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={brokerBase}
                    onChange={(e) => setBrokerBase(e.target.value)}
                    placeholder='e.g. "https://broker.example.com" (or "https://127.0.0.1:8443" in docker-compose)'
                  />
                </div>

                <div className="mt-4">
                  <Label>Broker auth token (OIDC)</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    placeholder="Authorization bearer token for broker (OIDC JWT)"
                    value={brokerAuthToken}
                    onChange={(e) => setBrokerAuthToken(e.target.value)}
                  />
                  <div className="mt-2 text-[11px] text-white/60">
                    Uses <code className="font-mono">Authorization: Bearer &lt;jwt&gt;</code> to call broker endpoints.
                  </div>
                </div>

                <div className="mt-4">
                  <Label>Agent id</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    value={brokerAgentId}
                    onChange={(e) => setBrokerAgentId(e.target.value)}
                    placeholder='e.g. "agent1"'
                  />
                  <div className="mt-2 flex items-center gap-2">
                    <button
                      className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                      type="button"
                      disabled={brokerAgentsBusy || String(brokerAuthToken || "").trim().length === 0}
                      onClick={async () => {
                        setBrokerAgentsError(null);
                        setBrokerAgentsBusy(true);
                        try {
                          const bb = String(brokerBase || "").trim().replace(/\/+$/, "");
                          const withScheme = /^https?:\/\//i.test(bb) ? bb : `https://${bb}`;
                          const r = await apiBrokerListAgents(withScheme, { mode: "broker", token: brokerAuthToken });
                          const agents = Array.isArray((r as any)?.agents) ? ((r as any).agents as any[]) : [];
                          setBrokerAgents(agents);
                          // Best-effort: pick first connected agent if none selected.
                          if (!String(brokerAgentId || "").trim()) {
                            const connected = agents.find((a) => a && a.connected === true);
                            if (connected && typeof connected.agent_id === "string") setBrokerAgentId(connected.agent_id);
                          }
                        } catch (e) {
                          setBrokerAgentsError(String(e));
                          setBrokerAgents(null);
                        } finally {
                          setBrokerAgentsBusy(false);
                        }
                      }}
                      title="Fetches /v1/agents from the broker (OIDC required)."
                    >
                      {brokerAgentsBusy ? "Listing…" : "List agents"}
                    </button>
                    <div className="text-[11px] text-white/60">
                      Proxy base:{" "}
                      <code className="font-mono text-white/70">
                        {String(effectiveBase || "").trim()}
                      </code>
                    </div>
                  </div>
                  {brokerAgentsError ? (
                    <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                      List agents failed: {brokerAgentsError}
                    </div>
                  ) : null}
                  {brokerAgents && brokerAgents.length > 0 ? (
                    <div className="mt-2 max-h-40 overflow-auto rounded-md border border-white/10 bg-black/20">
                      {brokerAgents.map((a: any) => {
                        const id = typeof a?.agent_id === "string" ? a.agent_id : "";
                        if (!id) return null;
                        const connected = a?.connected === true;
                        const lastSeen = typeof a?.last_seen_unix_ms === "number" ? a.last_seen_unix_ms : 0;
                        const selected = String(brokerAgentId || "").trim() === id;
                        return (
                          <button
                            key={id}
                            type="button"
                            className={[
                              "flex w-full items-center justify-between gap-2 px-3 py-2 text-left text-[11px] hover:bg-white/5",
                              selected ? "bg-white/10" : "",
                            ].join(" ")}
                            onClick={() => setBrokerAgentId(id)}
                            title={a?.remote_addr ? `remote=${String(a.remote_addr)}` : ""}
                          >
                            <span className="font-mono text-white/80">{id}</span>
                            <span className="text-white/60">
                              {connected ? (
                                <span className="text-emerald-300">connected</span>
                              ) : (
                                <span className="text-white/40">disconnected</span>
                              )}
                              {lastSeen ? ` · last_seen=${new Date(lastSeen).toLocaleString()}` : ""}
                            </span>
                          </button>
                        );
                      })}
                    </div>
                  ) : null}
                </div>

                <div className="mt-4">
                  <Label>Agentd auth token (pass-through)</Label>
                  <input
                    className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                    placeholder='Bearer token forwarded to agentd as "X-Agentd-Authorization" (e.g. "dev-agentd-token")'
                    value={daemonAuthToken}
                    onChange={(e) => setDaemonAuthToken(e.target.value)}
                  />
                  <div className="mt-2 text-[11px] text-white/60">
                    Uses <code className="font-mono">X-Agentd-Authorization: Bearer &lt;token&gt;</code> on proxied requests.
                  </div>
                </div>
              </>
            )}

            <div className="mt-4 grid grid-cols-2 gap-3">
              <div className="col-span-2">
                <Label>Session</Label>
                <input
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={sessionId}
                  onChange={(e) => setSessionId(e.target.value)}
                />
              </div>
              <div>
                <Label>Tools</Label>
                <select
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={tools}
                  onChange={(e) => setTools(e.target.value as any)}
                >
                  <option value="host">host</option>
                  <option value="basic">basic</option>
                  <option value="none">none</option>
                </select>
              </div>
              <div>
                <Label>Host policy</Label>
                <select
                  className="mt-1 w-full rounded-md border border-white/10 bg-black/30 px-3 py-2 text-sm"
                  value={hostPolicy}
                  onChange={(e) => setHostPolicy(e.target.value as any)}
                  disabled={tools !== "host"}
                >
                  <option value="full">full</option>
                  <option value="readonly">readonly</option>
                </select>
              </div>
            </div>

            <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
              <div className="flex items-center justify-between gap-3">
                <div className="text-xs font-semibold text-white/70">Client</div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                  onClick={() => daemonConfig.refetch()}
                  type="button"
                  disabled={daemonConfig.isFetching}
                >
                  Refresh config
                </button>
              </div>
              <div className="mt-2 grid gap-2 text-[11px] text-white/70">
                <label className="flex items-center justify-between gap-2">
                  <span>Allow audio autoplay</span>
                  <input type="checkbox" checked={allowAutoplay} onChange={(e) => setAllowAutoplay(e.target.checked)} />
                </label>
                <label className="flex items-center justify-between gap-2">
                  <span>Allow client RPCs</span>
                  <input type="checkbox" checked={allowClientRpcs} onChange={(e) => setAllowClientRpcs(e.target.checked)} />
                </label>
                <label className="flex items-center justify-between gap-2">
                  <span>Allow client RPC side effects</span>
                  <input
                    type="checkbox"
                    checked={allowClientEffects}
                    onChange={(e) => setAllowClientEffects(e.target.checked)}
                    disabled={!allowClientRpcs}
                  />
                </label>
              </div>
            </div>

            <div className="mt-4 rounded-md border border-white/10 bg-black/20 p-3">
              <div className="flex items-center justify-between gap-3">
                <div className="text-xs font-semibold text-white/70">Daemon Defaults (persisted)</div>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40 disabled:opacity-50"
                  onClick={() => daemonConfig.refetch()}
                  type="button"
                  disabled={daemonConfig.isFetching}
                >
                  Refresh
                </button>
              </div>
              <div className="mt-2 text-[11px] text-white/60">
                Saves to daemon state (server-side). This avoids keeping provider keys in browser storage.
              </div>
              <div className="mt-3 grid gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                  type="button"
                  disabled={updateDaemonDefaults.isPending}
                  onClick={() => {
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
                  }}
                >
                  Save model/base_url/proxy/timeout to daemon
                </button>
                <div className="flex items-center gap-2">
                  <button
                    className="flex-1 rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                    type="button"
                    disabled={updateDaemonDefaults.isPending}
                    onClick={() => {
                      const b = String(baseUrl || "").toLowerCase();
                      const provider = b.includes("deepseek") ? "deepseek" : b.includes("openrouter") ? "openrouter" : "openai";
                      void updateDaemonDefaults
                        .mutateAsync({
                          provider,
                          api_key: String(apiKey || "").trim(),
                        })
                        .catch(() => {});
                    }}
                    title="Stores the provider key on the daemon host (in state_dir/runtime_secrets.env)."
                  >
                    Save API key to daemon (current provider)
                  </button>
                  <button
                    className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
                    type="button"
                    disabled={updateDaemonDefaults.isPending}
                    onClick={() => {
                      const b = String(baseUrl || "").toLowerCase();
                      const provider = b.includes("deepseek") ? "deepseek" : b.includes("openrouter") ? "openrouter" : "openai";
                      if (!confirm(`Clear daemon-stored key for provider '${provider}'?`)) return;
                      void updateDaemonDefaults
                        .mutateAsync({
                          provider,
                          api_key: "",
                        })
                        .catch(() => {});
                    }}
                  >
                    Clear key
                  </button>
                </div>
                {updateDaemonDefaults.isError ? (
                  <div className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                    Save failed: {String(updateDaemonDefaults.error)}
                  </div>
                ) : null}
                {updateDaemonDefaults.isSuccess ? (
                  <div className="rounded-md border border-emerald-500/30 bg-emerald-500/10 px-3 py-2 text-xs text-emerald-100">
                    Saved.
                  </div>
                ) : null}
              </div>
            </div>

            <div className="mt-4 grid gap-2 text-[11px] text-white/70">
              <label className="flex items-center justify-between gap-2">
                <span>YOLO (no tool restrictions)</span>
                <input type="checkbox" checked={yolo} onChange={(e) => setYolo(e.target.checked)} />
              </label>
              <label className="flex items-center justify-between gap-2">
                <span>Verbose</span>
                <input type="checkbox" checked={verbose} onChange={(e) => setVerbose(e.target.checked)} />
              </label>
              <label className="flex items-center justify-between gap-2">
                <span>Async run</span>
                <input type="checkbox" checked={useAsync} onChange={(e) => setUseAsync(e.target.checked)} />
              </label>
              <label className="flex items-center justify-between gap-2">
                <span>Show debug in conversation</span>
                <input
                  type="checkbox"
                  checked={showDebugInConversation}
                  onChange={(e) => setShowDebugInConversation(e.target.checked)}
                />
              </label>
            </div>

            <div className="mt-4">
              <div className="text-xs font-semibold text-white/70">Sessions</div>
              <div className="mt-2 flex flex-wrap gap-2">
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                  onClick={() => sessions.refetch()}
                  type="button"
                >
                  Refresh
                </button>
                <button
                  className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40 disabled:opacity-50"
                  onClick={() => newSession.mutate()}
                  type="button"
                  disabled={newSession.isPending}
                >
                  New session
                </button>
                <button
                  className="rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
                  onClick={() => {
                    const ids = sessions.data?.sessions ?? [];
                    const n = ids.length;
                    if (n === 0) return;
                    if (!clearAllArmed) {
                      setClearAllArmed(true);
                      try {
                        if (clearAllArmTimeoutRef.current) window.clearTimeout(clearAllArmTimeoutRef.current);
                      } catch {
                        // ignore
                      }
                      clearAllArmTimeoutRef.current = window.setTimeout(() => setClearAllArmed(false), 8000);
                      return;
                    }
                    setClearAllArmed(false);
                    void clearAllSessions.mutateAsync().catch(() => {});
                  }}
                  type="button"
                  disabled={clearAllSessions.isPending}
                  title="Danger: deletes all sessions on the daemon."
                >
                  {clearAllArmed ? `Confirm clear all (${(sessions.data?.sessions ?? []).length})` : "Clear all"}
                </button>
                {clearAllArmed ? (
                  <button
                    className="rounded-md border border-white/10 bg-black/30 px-3 py-2 text-xs text-white/80 hover:bg-black/40"
                    type="button"
                    onClick={() => setClearAllArmed(false)}
                  >
                    Cancel
                  </button>
                ) : null}
              </div>
              <div className="mt-2 max-h-64 overflow-auto rounded-md border border-white/10 bg-black/20">
                {(sessions.data?.sessions ?? []).map((sid) => {
                  const selected = sid === sessionId;
                  return (
                    <div
                      key={sid}
                      className={`flex items-center justify-between gap-2 px-3 py-2 text-xs ${selected ? "bg-white/10" : ""}`}
                    >
                      <button className="flex-1 text-left hover:underline" onClick={() => setSessionId(sid)} type="button">
                        {sid}
                      </button>
                      <button
                        className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200 hover:bg-rose-500/15 disabled:opacity-50"
                        type="button"
                        disabled={deleteSession.isPending}
                        onClick={() => {
                          if (!confirm(`Delete session '${sid}'?`)) return;
                          void deleteSession.mutateAsync(sid).catch(() => {});
                        }}
                      >
                        Delete
                      </button>
                    </div>
                  );
                })}
              </div>
              {deleteSession.isError ? (
                <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                  Delete failed: {String(deleteSession.error)}
                </div>
              ) : null}
              {clearAllSessions.isError ? (
                <div className="mt-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
                  Clear all failed: {String(clearAllSessions.error)}
                </div>
              ) : null}
            </div>
          </div>
        </div>
      ) : null}
    </div>
  );
}
