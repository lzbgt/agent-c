import React from "react";
import { useQuery } from "@tanstack/react-query";
import {
  apiGetAudit,
  apiGetConfig,
  apiGetDbClientEvents,
  apiGetDbMessages,
  apiGetDbRun,
  apiGetDbRuns,
  apiGetDbUiActions,
  apiGetHealth,
  apiGetSession,
  apiGetSessionArtifacts,
  apiGetSessionClientEvents,
  apiGetSessionScene,
  apiListSessions,
  extractSessionErrorMessage,
  extractSessionIds,
  extractSessionInfo,
} from "../api";
import type { AppDataPlaneArgs } from "./appDataPlaneTypes";

export function useAppSessionQueries(args: AppDataPlaneArgs) {
  const {
    activeJobId,
    allowClientEffects,
    allowClientRpcs,
    authKey,
    brokerAuthToken,
    brokerCookieAuth,
    connectionMode,
    daemonAuth,
    daemonAuthToken,
    effectiveBase,
    jobStatus,
    jobUpdatedMs,
    lastRunPrompt,
    lastRunPromptRef,
    liveEvents,
    selectedSessionId,
  } = args;

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

  const missingBrokerAuthToken =
    connectionMode === "broker" && !brokerCookieAuth && String(brokerAuthToken || "").trim().length === 0;
  const missingDaemonAuthToken = String(daemonAuthToken || "").trim().length === 0;

  const isLocalDaemonBase = React.useMemo(() => {
    try {
      const url = new URL(effectiveBase);
      const host = String(url.hostname || "").toLowerCase();
      return host === "127.0.0.1" || host === "localhost" || host === "0.0.0.0";
    } catch {
      return false;
    }
  }, [effectiveBase]);

  const audit = useQuery({
    queryKey: ["audit", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetAudit(effectiveBase, selectedSessionId, daemonAuth),
    enabled: !!selectedSessionId,
    retry: 1,
  });

  const sessionInfo = useQuery({
    queryKey: ["session_info", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetSession(effectiveBase, selectedSessionId, daemonAuth),
    enabled: !!selectedSessionId,
    retry: 1,
  });

  const sessionInfoData = React.useMemo(() => extractSessionInfo(sessionInfo.data), [sessionInfo.data]);

  const auditEntriesDesc = React.useMemo(() => {
    const raw = audit.data?.ok && Array.isArray(audit.data?.entries) ? (audit.data.entries as any[]) : [];
    const entries = raw.filter((entry) => entry && typeof entry === "object");
    entries.sort((a: any, b: any) => {
      const ta = typeof a?.ts_unix_ms === "number" ? a.ts_unix_ms : 0;
      const tb = typeof b?.ts_unix_ms === "number" ? b.ts_unix_ms : 0;
      return tb - ta;
    });
    return entries;
  }, [audit.data]);

  const historyEntriesDesc = React.useMemo(() => {
    if (!activeJobId && liveEvents.length === 0) return auditEntriesDesc;
    const ts = typeof jobUpdatedMs === "number" && jobUpdatedMs > 0 ? jobUpdatedMs : Date.now();
    const live = {
      ts_unix_ms: ts,
      prompt: activeJobId ? lastRunPromptRef.current || lastRunPrompt || "" : "Live session event stream",
      assistant_text: "",
      events: liveEvents,
      ok: undefined,
      job_id: activeJobId,
      job_status: activeJobId ? jobStatus ?? "running" : "live_session",
      live: true,
    };
    return [live, ...auditEntriesDesc];
  }, [activeJobId, auditEntriesDesc, jobStatus, jobUpdatedMs, lastRunPrompt, liveEvents, lastRunPromptRef]);

  const sessionClientEvents = useQuery({
    queryKey: ["session_client_events", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetSessionClientEvents(effectiveBase, selectedSessionId, daemonAuth, { maxBytes: 1024 * 1024 }),
    enabled: !!selectedSessionId,
    retry: 1,
  });

  const sessionArtifacts = useQuery({
    queryKey: ["session_artifacts", effectiveBase, authKey, selectedSessionId],
    queryFn: () =>
      apiGetSessionArtifacts(effectiveBase, selectedSessionId, daemonAuth, {
        maxBytes: 2 * 1024 * 1024,
        maxArtifacts: 64,
      }),
    enabled: !!selectedSessionId,
    retry: 1,
  });

  const sessionArtifactsUnsupported = React.useMemo(() => {
    if (connectionMode !== "broker") return false;
    const payload = sessionArtifacts.data;
    if (!payload || typeof payload !== "object") return false;
    const status = typeof (payload as any).status === "number" ? (payload as any).status : 0;
    const code = String((payload as any).code || "").trim().toLowerCase();
    const message = String(extractSessionErrorMessage(payload) || "").trim().toLowerCase();
    return (
      status === 404 ||
      status === 405 ||
      code === "not_found" ||
      code === "method_not_allowed" ||
      message === "not found" ||
      message === "method not allowed"
    );
  }, [connectionMode, sessionArtifacts.data]);

  const sessionScene = useQuery({
    queryKey: ["session_scene", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetSessionScene(effectiveBase, selectedSessionId, daemonAuth),
    enabled: !!selectedSessionId,
    refetchInterval: activeJobId ? 750 : 2500,
    retry: 1,
  });

  const dbMessages = useQuery({
    queryKey: ["db_messages", effectiveBase, authKey, selectedSessionId],
    queryFn: () =>
      apiGetDbMessages(effectiveBase, selectedSessionId, daemonAuth, {
        limit: 200,
        offset: 0,
        maxContentBytes: 64 * 1024,
        maxMmBytes: 2 * 1024 * 1024,
      }),
    enabled: !!selectedSessionId,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const dbRuns = useQuery({
    queryKey: ["db_runs", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetDbRuns(effectiveBase, selectedSessionId, daemonAuth, { limit: 50, offset: 0 }),
    enabled: !!selectedSessionId,
    refetchInterval: activeJobId ? 2000 : 8000,
    retry: 1,
  });

  const [dbRunDetailsById, setDbRunDetailsById] = React.useState<Record<number, any>>({});
  const dbRunDetailsByIdRef = React.useRef<Record<number, any>>({});
  const dbRunDetailsLoadingRef = React.useRef<Record<number, boolean>>({});

  React.useEffect(() => {
    dbRunDetailsByIdRef.current = dbRunDetailsById || {};
  }, [dbRunDetailsById]);

  React.useEffect(() => {
    const runs = dbRuns.data?.ok && Array.isArray(dbRuns.data?.runs) ? (dbRuns.data.runs as any[]) : [];
    if (runs.length === 0) return;
    const candidates: number[] = [];
    for (const run of runs.slice(0, 12)) {
      const runId = typeof run?.run_id === "number" ? run.run_id : Number(run?.run_id ?? run?.id ?? NaN);
      if (!Number.isFinite(runId)) continue;
      candidates.push(runId);
    }
    if (candidates.length === 0) return;

    let cancelled = false;
    const fetchMissing = async () => {
      for (const runId of candidates) {
        if (cancelled) return;
        if (dbRunDetailsByIdRef.current[runId]) continue;
        if (dbRunDetailsLoadingRef.current[runId]) continue;
        dbRunDetailsLoadingRef.current[runId] = true;
        try {
          const resp = await apiGetDbRun(effectiveBase, runId, daemonAuth, {
            includeTools: true,
            includeEvents: false,
            includeArtifacts: false,
            includeUiActions: false,
          });
          if (resp.ok && resp.run) {
            setDbRunDetailsById((prev) => ({ ...(prev || {}), [runId]: resp }));
          }
        } catch {
          // ignore fetch failures; UI will fallback to run summary only
        } finally {
          delete dbRunDetailsLoadingRef.current[runId];
        }
      }
    };
    void fetchMissing();
    return () => {
      cancelled = true;
    };
  }, [dbRuns.data, daemonAuth, effectiveBase]);

  const dbUiActions = useQuery({
    queryKey: ["db_ui_actions", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetDbUiActions(effectiveBase, selectedSessionId, daemonAuth, { limit: 100, offset: 0 }),
    enabled: !!selectedSessionId && allowClientRpcs && allowClientEffects,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const dbClientEvents = useQuery({
    queryKey: ["db_client_events", effectiveBase, authKey, selectedSessionId],
    queryFn: () => apiGetDbClientEvents(effectiveBase, selectedSessionId, daemonAuth, { limit: 100, offset: 0 }),
    enabled: !!selectedSessionId && allowClientRpcs && allowClientEffects,
    refetchInterval: activeJobId ? 1500 : 5000,
    retry: 1,
  });

  const auditRefetch = audit.refetch;
  const sessionsRefetch = sessions.refetch;
  const sessionList = React.useMemo(() => extractSessionIds(sessions.data), [sessions.data]);

  return {
    audit,
    auditEntriesDesc,
    auditRefetch,
    daemonConfig,
    dbClientEvents,
    dbMessages,
    dbRunDetailsById,
    dbRuns,
    dbUiActions,
    health,
    historyEntriesDesc,
    isLocalDaemonBase,
    missingBrokerAuthToken,
    missingDaemonAuthToken,
    sessionInfo,
    sessionInfoData,
    sessions,
    sessionsRefetch,
    sessionsUnauthorized,
    sessionArtifacts,
    sessionArtifactsUnsupported,
    sessionClientEvents,
    sessionList,
    sessionScene,
  };
}
