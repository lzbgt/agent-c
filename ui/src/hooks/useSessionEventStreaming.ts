import React from "react";
import { daemonFetchInit, type AgentEvent, type ApiAuth, buildSessionEventsStreamUrl } from "../api";
import { readSseStream, type SseEvent } from "../sse";
import { sleep } from "../timeUtils";

const SESSION_STREAM_MAX_EVENTS = 200;
const SESSION_STREAM_SCHEMA_VERSION = 1;

type SessionStreamStatus = "disabled" | "idle" | "connecting" | "live" | "reconnecting" | "error";

type PersistedSessionStreamEvent = {
  id?: string;
  event: string;
  data: string;
  receivedAtMs: number;
};

type PersistedSessionStreamState = {
  version: number;
  lastEventId: string;
  lastEventAtMs: number | null;
  updatedMs: number | null;
  events: PersistedSessionStreamEvent[];
};

export type SessionEventStreamState = {
  status: SessionStreamStatus;
  lastEventId: string;
  lastEventAtMs: number | null;
  updatedMs: number | null;
  bufferedCount: number;
  error: string | null;
};

type SessionEventStreamingArgs = {
  activeJobId: string | null;
  connectionMode: string;
  daemonAuth: ApiAuth;
  effectiveBase: string;
  sessionId: string;
  sessionScopeKey: string;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
};

const EMPTY_PERSISTED_STATE: PersistedSessionStreamState = {
  version: SESSION_STREAM_SCHEMA_VERSION,
  lastEventId: "",
  lastEventAtMs: null,
  updatedMs: null,
  events: [],
};

function readPersistedSessionStream(key: string): PersistedSessionStreamState {
  if (typeof window === "undefined" || !window.localStorage || !key) return EMPTY_PERSISTED_STATE;
  try {
    const raw = window.localStorage.getItem(key);
    if (!raw) return EMPTY_PERSISTED_STATE;
    const parsed = JSON.parse(raw) as Partial<PersistedSessionStreamState>;
    const rawEvents = Array.isArray(parsed?.events) ? parsed.events : [];
    const events = rawEvents
      .filter((entry) => entry && typeof entry === "object")
      .map((entry: any) => ({
        id: typeof entry.id === "string" && entry.id.trim() ? entry.id.trim() : undefined,
        event: typeof entry.event === "string" && entry.event.trim() ? entry.event.trim() : "message",
        data: typeof entry.data === "string" ? entry.data : "",
        receivedAtMs:
          typeof entry.receivedAtMs === "number" && Number.isFinite(entry.receivedAtMs) && entry.receivedAtMs > 0
            ? entry.receivedAtMs
            : Date.now(),
      }));
    return {
      version: SESSION_STREAM_SCHEMA_VERSION,
      lastEventId: typeof parsed?.lastEventId === "string" ? parsed.lastEventId.trim() : "",
      lastEventAtMs:
        typeof parsed?.lastEventAtMs === "number" && Number.isFinite(parsed.lastEventAtMs) ? parsed.lastEventAtMs : null,
      updatedMs:
        typeof parsed?.updatedMs === "number" && Number.isFinite(parsed.updatedMs) ? parsed.updatedMs : null,
      events: events.slice(-SESSION_STREAM_MAX_EVENTS),
    };
  } catch {
    return EMPTY_PERSISTED_STATE;
  }
}

function writePersistedSessionStream(key: string, state: PersistedSessionStreamState) {
  if (typeof window === "undefined" || !window.localStorage || !key) return;
  try {
    window.localStorage.setItem(key, JSON.stringify(state));
  } catch {
    // ignore storage quota/private mode failures
  }
}

function parseEventData(raw: string): any {
  if (!raw) return {};
  try {
    return JSON.parse(raw);
  } catch {
    return raw;
  }
}

function materializeAgentEvents(state: PersistedSessionStreamState): AgentEvent[] {
  return state.events.map((entry) => {
    const parsed = parseEventData(entry.data);
    if (parsed && typeof parsed === "object" && typeof parsed.type === "string") {
      if (Object.prototype.hasOwnProperty.call(parsed, "data")) {
        return parsed as AgentEvent;
      }
      const record = parsed as Record<string, any>;
      const { type, trace_id, ...rest } = record;
      return {
        type: String(type),
        trace_id: typeof trace_id === "string" ? trace_id : undefined,
        data: rest,
      } as AgentEvent;
    }
    return {
      type: entry.event || "message",
      data: parsed,
    } as AgentEvent;
  });
}

function appendSessionStreamEvent(
  state: PersistedSessionStreamState,
  event: SseEvent,
): PersistedSessionStreamState | null {
  const nextId = typeof event.id === "string" ? event.id.trim() : "";
  if (nextId && state.events.some((existing) => existing.id === nextId)) {
    return null;
  }
  const nextEvents = state.events.concat({
    id: nextId || undefined,
    event: event.event || "message",
    data: String(event.data || ""),
    receivedAtMs: Date.now(),
  });
  return {
    version: SESSION_STREAM_SCHEMA_VERSION,
    lastEventId: nextId || state.lastEventId,
    lastEventAtMs: Date.now(),
    updatedMs: Date.now(),
    events: nextEvents.slice(-SESSION_STREAM_MAX_EVENTS),
  };
}

export default function useSessionEventStreaming(args: SessionEventStreamingArgs): SessionEventStreamState {
  const { activeJobId, connectionMode, daemonAuth, effectiveBase, sessionId, sessionScopeKey, setLiveEvents } = args;
  const sessionIdTrimmed = String(sessionId || "").trim();
  const streamUrl = React.useMemo(
    () => buildSessionEventsStreamUrl(effectiveBase, sessionIdTrimmed),
    [effectiveBase, sessionIdTrimmed],
  );
  const persistKey = React.useMemo(
    () => (sessionIdTrimmed ? `agentui.sessionEventStream:${sessionScopeKey}::${sessionIdTrimmed}` : ""),
    [sessionIdTrimmed, sessionScopeKey],
  );
  const enabled = connectionMode === "broker" && !activeJobId && sessionIdTrimmed.length > 0 && !!streamUrl;

  const [status, setStatus] = React.useState<SessionStreamStatus>("disabled");
  const [lastEventId, setLastEventId] = React.useState<string>("");
  const [lastEventAtMs, setLastEventAtMs] = React.useState<number | null>(null);
  const [updatedMs, setUpdatedMs] = React.useState<number | null>(null);
  const [bufferedCount, setBufferedCount] = React.useState<number>(0);
  const [error, setError] = React.useState<string | null>(null);
  const persistedRef = React.useRef<PersistedSessionStreamState>(EMPTY_PERSISTED_STATE);

  React.useEffect(() => {
    if (!persistKey) return;
    const persisted = readPersistedSessionStream(persistKey);
    persistedRef.current = persisted;
    setLastEventId(persisted.lastEventId);
    setLastEventAtMs(persisted.lastEventAtMs);
    setUpdatedMs(persisted.updatedMs);
    setBufferedCount(persisted.events.length);
    if (enabled) {
      setLiveEvents(materializeAgentEvents(persisted));
    } else if (!activeJobId) {
      setLiveEvents([]);
    }
  }, [activeJobId, enabled, persistKey, setLiveEvents]);

  React.useEffect(() => {
    if (!enabled || !streamUrl || !persistKey) {
      setStatus(sessionIdTrimmed ? "idle" : "disabled");
      setError(null);
      return;
    }

    let cancelled = false;
    let controller: AbortController | null = null;
    let reconnectAttempt = 0;

    const streamLoop = async () => {
      while (!cancelled) {
        const replayCursor = persistedRef.current.lastEventId;
        controller = new AbortController();
        setStatus(reconnectAttempt > 0 ? "reconnecting" : "connecting");
        try {
          const extraHeaders = replayCursor ? { "Last-Event-ID": replayCursor } : undefined;
          const response = await fetch(streamUrl, daemonFetchInit(daemonAuth, { signal: controller.signal }, extraHeaders));
          if (!response.ok) {
            throw new Error(`session stream failed: HTTP ${response.status}`);
          }
          setError(null);
          setStatus("live");
          await readSseStream(response, (event) => {
            if (cancelled) return;
            const nextState = appendSessionStreamEvent(persistedRef.current, event);
            if (!nextState) return;
            persistedRef.current = nextState;
            writePersistedSessionStream(persistKey, nextState);
            setLastEventId(nextState.lastEventId);
            setLastEventAtMs(nextState.lastEventAtMs);
            setUpdatedMs(nextState.updatedMs);
            setBufferedCount(nextState.events.length);
            setLiveEvents(materializeAgentEvents(nextState));
          });
          if (cancelled) return;
          reconnectAttempt += 1;
          setStatus("reconnecting");
        } catch (streamError) {
          if (cancelled || controller.signal.aborted) return;
          reconnectAttempt += 1;
          setError(String(streamError));
          setStatus("error");
        }
        if (cancelled) return;
        const delayMs = Math.min(5000, 1000 * reconnectAttempt);
        await sleep(delayMs);
      }
    };

    void streamLoop();
    return () => {
      cancelled = true;
      try {
        controller?.abort();
      } catch {
        // ignore abort failures
      }
    };
  }, [daemonAuth, enabled, persistKey, setLiveEvents, streamUrl, sessionIdTrimmed]);

  return {
    status,
    lastEventId,
    lastEventAtMs,
    updatedMs,
    bufferedCount,
    error,
  };
}
