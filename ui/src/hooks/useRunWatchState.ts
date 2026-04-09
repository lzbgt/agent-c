import React from "react";
import {
  apiBrokerGetClientPrefs,
  apiBrokerPostClientPrefs,
  apiGetClientPrefs,
  apiGetJob,
  apiPostClientPrefs,
  type AgentEvent,
  type ApiAuth,
} from "../api";
import { loadJson } from "../jsonUtils";
import { pruneJobsBySession } from "../jobStore";
import {
  extractRunWatchByScope,
  mergeRunWatchByScope,
  runWatchMapsEqual,
  type RunWatchByScope,
} from "../runWatchPrefs";
import useLocalStorageState from "./useLocalStorageState";
import type { JobStoreWriter } from "./runtimePlaneTypes";

const RUN_WATCH_PREFS_KIND = "run_watch";
const RUN_WATCH_PREFS_VERSION = 1;
const RUN_WATCH_PERSIST_MIN_INTERVAL_MS = 5000;

type UseRunWatchStateArgs = {
  activeJobId: string | null;
  authKey: string;
  cursorRef: React.MutableRefObject<number>;
  daemonAuth: ApiAuth;
  effectiveBase: string;
  jobStoreKey: string;
  runWatchCanUse: boolean;
  runWatchPrefsBase: string;
  runWatchPrefsClientId: string;
  sessionId: string;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
};

export function useRunWatchState(args: UseRunWatchStateArgs) {
  const {
    activeJobId,
    authKey,
    cursorRef,
    daemonAuth,
    effectiveBase,
    jobStoreKey,
    runWatchCanUse,
    runWatchPrefsBase,
    runWatchPrefsClientId,
    sessionId,
    setActiveJobId,
    setJobError,
    setJobStatus,
    setJobUpdatedMs,
    setLiveEvents,
  } = args;

  const [jobsBySessionJson, setJobsBySessionJson] = useLocalStorageState("agentui.jobsBySession", "{}");
  const jobsBySessionJsonRef = React.useRef(jobsBySessionJson);
  React.useEffect(() => {
    jobsBySessionJsonRef.current = jobsBySessionJson;
  }, [jobsBySessionJson]);

  const [runWatchServerStatus, setRunWatchServerStatus] = React.useState<"idle" | "loading" | "ready" | "error">("idle");
  const runWatchPersistRef = React.useRef<{
    timer: ReturnType<typeof setTimeout> | null;
    pending: RunWatchByScope | null;
    lastSentAt: number;
  }>({ timer: null, pending: null, lastSentAt: 0 });
  const runWatchLoadKeyRef = React.useRef<string>("");

  const runWatchMode = React.useMemo(() => {
    if (!runWatchCanUse) return "local";
    if (runWatchServerStatus === "error") return "local";
    if (runWatchServerStatus === "loading") return "server:loading";
    if (runWatchServerStatus === "ready") return "server";
    return "server";
  }, [runWatchCanUse, runWatchServerStatus]);

  const parseJobsBySession = React.useCallback(() => {
    const value = loadJson(jobsBySessionJsonRef.current);
    const jobs = value && typeof value === "object" && !Array.isArray(value) ? (value as RunWatchByScope) : {};
    const pruned = pruneJobsBySession(Date.now(), jobs);
    if (pruned.changed) {
      try {
        setJobsBySessionJson(JSON.stringify(pruned.next));
      } catch {
        // ignore
      }
    }
    return pruned.next;
  }, [setJobsBySessionJson]);

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
      const remoteMap = pruneJobsBySession(Date.now(), extractRunWatchByScope(resp.prefs)).next;
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
  const loadServerRunWatchRef = React.useRef(loadServerRunWatch);

  React.useEffect(() => {
    if (!runWatchCanUse) return;
    const nextKey = `${runWatchPrefsBase}::${runWatchPrefsClientId}::${daemonAuth.mode}::${authKey}`;
    if (runWatchLoadKeyRef.current === nextKey) return;
    runWatchLoadKeyRef.current = nextKey;
    void loadServerRunWatchRef.current();
  }, [authKey, daemonAuth.mode, runWatchCanUse, runWatchPrefsBase, runWatchPrefsClientId]);

  React.useEffect(() => {
    loadServerRunWatchRef.current = loadServerRunWatch;
  }, [loadServerRunWatch]);

  React.useEffect(() => {
    if (runWatchCanUse) return;
    runWatchLoadKeyRef.current = "";
  }, [runWatchCanUse]);

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

  const writeJobsBySession = React.useCallback<JobStoreWriter>(
    (mutate) => {
      setJobsBySessionJson((prevRaw) => {
        const parsed = loadJson(String(prevRaw || ""));
        const prev = parsed && typeof parsed === "object" && !Array.isArray(parsed) ? (parsed as RunWatchByScope) : {};
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
        const status = typeof job.status === "string" ? job.status : "";
        if (status === "queued" || status === "running") {
          cursorRef.current = cursor;
          setJobError(null);
          setJobStatus(status);
          setJobUpdatedMs(typeof job.updated_unix_ms === "number" ? job.updated_unix_ms : null);
          setLiveEvents([]);
          setActiveJobId(jobId);
          return;
        }
        writeJobsBySession((prev) => {
          const next = { ...prev };
          delete next[jobStoreKey];
          return next;
        });
      } catch {
        // ignore
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [
    activeJobId,
    cursorRef,
    daemonAuth,
    effectiveBase,
    jobStoreKey,
    parseJobsBySession,
    sessionId,
    setActiveJobId,
    setJobError,
    setJobStatus,
    setJobUpdatedMs,
    setLiveEvents,
    writeJobsBySession,
  ]);

  React.useEffect(() => {
    if (!activeJobId) return;
    const sid = String(sessionId || "").trim();
    if (!sid) return;
    const jobId = activeJobId;
    const timer = window.setInterval(() => {
      const cursor = cursorRef.current;
      writeJobsBySession((prev) => {
        const current = prev[jobStoreKey];
        if (!current || current.job_id !== jobId) return prev;
        if (current.cursor === cursor) return prev;
        return { ...prev, [jobStoreKey]: { ...current, cursor, updated_unix_ms: Date.now() } };
      });
    }, 1000);
    return () => {
      try {
        window.clearInterval(timer);
      } catch {
        // ignore
      }
    };
  }, [activeJobId, cursorRef, jobStoreKey, sessionId, writeJobsBySession]);

  return {
    runWatchMode,
    writeJobsBySession,
  };
}
