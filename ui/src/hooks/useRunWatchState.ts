import React from "react";
import {
  apiBrokerGetClientPrefs, apiBrokerPostClientPrefs, apiGetClientPrefs, apiPostClientPrefs,
  type AgentEvent, type ApiAuth,
} from "../api";
import { readJobWatches, writeJobWatches, UNVERIFIED_JOB_STATUS } from "../jobStore";
import {
  extractRunWatchByScope, mergeRunWatchByScope, parseRunWatchByScope,
  runWatchHints, runWatchMapsEqual, type RunWatchByScope,
} from "../runWatchPrefs";
import type { JobStoreWriter } from "./runtimePlaneTypes";

const PREFS_KIND = "run_watch";
const PERSIST_INTERVAL_MS = 5000;
const REQUEST_DEADLINE_MS = 5000;

type Args = {
  activeJobId: string | null; authKey: string; cursorRef: React.MutableRefObject<number>;
  daemonAuth: ApiAuth; effectiveBase: string; jobStoreKey: string;
  runWatchCanUse: boolean; runWatchPrefsBase: string; runWatchPrefsClientId: string; sessionId: string;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
};
type Sync = {
  key: string; disposed: boolean; pending: boolean; lastSentAt: number;
  timer: ReturnType<typeof setTimeout> | null; push: AbortController | null;
  schedule: () => void;
};

export function useRunWatchState(args: Args) {
  const [revision, refresh] = React.useReducer(value => value + 1, 0);
  const [serverStatus, setServerStatus] = React.useState<"idle" | "loading" | "ready" | "error">("idle");
  const context = JSON.stringify([args.runWatchCanUse, args.runWatchPrefsBase,
    args.runWatchPrefsClientId, args.authKey, args.daemonAuth]);
  const currentContext = React.useRef(context);
  currentContext.current = context;
  const sync = React.useRef<Sync | null>(null);
  const schedule = React.useCallback(() => {
    const state = sync.current;
    if (!state || state.disposed || state.key !== currentContext.current) return;
    state.pending = true;
    state.schedule();
  }, []);

  // The checked local write is the authority. React and preference replies are
  // projections; neither may replace it using a captured pre-request snapshot.
  const writeJobsBySession = React.useCallback<JobStoreWriter>(mutate => {
    const previous = readJobWatches();
    const next = mutate(previous);
    if (next === previous) return;
    if (!runWatchMapsEqual(previous, next)) writeJobWatches(next);
    refresh();
    schedule();
  }, [schedule]);

  React.useEffect(() => {
    const changed = () => refresh();
    const onStorage = (event: StorageEvent) => {
      if (event.key === null || event.key === "agentui.jobsBySession") changed();
    };
    window.addEventListener("storage", onStorage);
    return () => window.removeEventListener("storage", onStorage);
  }, []);

  React.useEffect(() => {
    if (!args.runWatchCanUse) { setServerStatus("idle");return; }
    const state: Sync = { key: context, disposed: false, pending: false, lastSentAt: -Infinity,
      timer: null, push: null, schedule: () => {} };
    sync.current = state;
    const valid = () => !state.disposed && currentContext.current === context;
    const flush = async () => {
      state.timer = null;
      if (!valid() || !state.pending || state.push) return;
      state.pending = false;
      const controller = new AbortController();
      state.push = controller;
      state.lastSentAt = performance.now(); // clock changes cannot stall metadata persistence
      const deadline = setTimeout(() => controller.abort(), REQUEST_DEADLINE_MS);
      try {
        const payload = { client_id: args.runWatchPrefsClientId, client_kind: PREFS_KIND,
          prefs: { run_watch: { version: 1, by_scope: runWatchHints(readJobWatches()) } } };
        const response = args.daemonAuth.mode === "broker"
          ? await apiBrokerPostClientPrefs(args.runWatchPrefsBase, payload, args.daemonAuth, controller.signal)
          : await apiPostClientPrefs(args.runWatchPrefsBase, payload, args.daemonAuth, controller.signal);
        if (!response.ok) throw new Error("Run watch preferences rejected");
        if (valid()) setServerStatus("ready");
      } catch {
        if (valid()) { setServerStatus("error");state.pending = false; }
      } finally {
        clearTimeout(deadline);
        state.push = null;
        if (valid()) state.schedule();
      }
    };
    state.schedule = () => {
      if (!valid() || !state.pending || state.push || state.timer) return;
      const delay = Math.max(PERSIST_INTERVAL_MS - (performance.now() - state.lastSentAt), 0);
      state.timer = setTimeout(() => { void flush(); }, delay);
    };

    const controller = new AbortController();
    const deadline = setTimeout(() => controller.abort(), REQUEST_DEADLINE_MS);
    setServerStatus("loading");
    void (async () => {
      try {
        const response = args.daemonAuth.mode === "broker"
          ? await apiBrokerGetClientPrefs(args.runWatchPrefsBase, args.runWatchPrefsClientId, PREFS_KIND, args.daemonAuth, controller.signal)
          : await apiGetClientPrefs(args.runWatchPrefsBase, args.runWatchPrefsClientId, PREFS_KIND, args.daemonAuth, controller.signal);
        if (!valid() || controller.signal.aborted) return;
        if (!response.ok) throw new Error("Run watch preferences unavailable");
        const remote = parseRunWatchByScope(extractRunWatchByScope(response.prefs));
        const local = readJobWatches(); // read after the response; acceptance may have happened meanwhile
        const merged = mergeRunWatchByScope(local, remote);
        if (!runWatchMapsEqual(merged, local)) { writeJobWatches(merged);refresh(); }
        if (!runWatchMapsEqual(runWatchHints(merged), remote)) schedule();
        setServerStatus("ready");
      } catch { if (valid()) setServerStatus("error"); }
      finally { clearTimeout(deadline); }
    })();
    return () => {
      state.disposed = true;
      controller.abort();state.push?.abort();
      clearTimeout(deadline);
      if (state.timer) clearTimeout(state.timer);
      state.timer = null;state.pending = false;
      if (sync.current === state) sync.current = null;
    };
    // The context includes every transport/authorization input. Session changes
    // share one authority's preferences and do not retarget a pending request.
  }, [context, schedule]);

  const selectedScope = React.useRef(args.jobStoreKey);
  React.useLayoutEffect(() => {
    if (selectedScope.current === args.jobStoreKey) return;
    selectedScope.current = args.jobStoreKey;
    args.cursorRef.current = 0;
    args.setActiveJobId(null);args.setJobStatus(null);args.setJobError(null);
    args.setJobUpdatedMs(null);args.setLiveEvents([]);
  }, [args.jobStoreKey]);

  React.useEffect(() => {
    if (args.activeJobId || !args.effectiveBase.trim() || !args.sessionId.trim()) return;
    try {
      const watches = readJobWatches();
      const entry = watches[args.jobStoreKey] || watches[args.sessionId];
      if (!entry?.job_id || entry.local_state === "retired") return;
      if (!watches[args.jobStoreKey] && watches[args.sessionId]) {
        writeJobsBySession(previous => {
          if (previous[args.jobStoreKey]) return previous;
          const next = { ...previous, [args.jobStoreKey]: previous[args.sessionId] };
          delete next[args.sessionId];return next;
        });
      }
      args.cursorRef.current = 0;
      args.setJobError(null);args.setJobStatus(UNVERIFIED_JOB_STATUS);
      args.setJobUpdatedMs(null);args.setLiveEvents([]);args.setActiveJobId(entry.job_id);
    } catch { setServerStatus("error"); } // preserve unavailable/invalid local state
  }, [args.activeJobId, args.effectiveBase, args.jobStoreKey, args.sessionId, revision, writeJobsBySession]);

  React.useEffect(() => {
    if (!args.activeJobId || !args.sessionId.trim()) return;
    const job = args.activeJobId;
    const timer = setInterval(() => {
      try {
        writeJobsBySession(previous => {
          const entry = previous[args.jobStoreKey];
          if (!entry || entry.job_id !== job || entry.local_state === "retired" || entry.cursor === args.cursorRef.current) return previous;
          return { ...previous, [args.jobStoreKey]: { ...entry, cursor: args.cursorRef.current, updated_unix_ms: Date.now() } };
        });
      } catch { setServerStatus("error"); }
    }, 1000);
    return () => clearInterval(timer);
  }, [args.activeJobId, args.jobStoreKey, args.sessionId, writeJobsBySession]);

  return { runWatchMode: !args.runWatchCanUse || serverStatus === "error" ? "local"
    : serverStatus === "loading" ? "server:loading" : "server", writeJobsBySession };
}
