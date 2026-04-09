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
import {
  getDbRunNumericId,
  normalizeDbMessageRows,
  normalizeDbRunDetailRow,
  normalizeDbRunSummaryRows,
  normalizeHistoryEntries,
  normalizeSessionArtifactRows,
  type DbRunDetailRow,
  type HistoryEntry,
} from "../history/historyPanelData";
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
    String(extractSessionErrorMessage(sessions.data) || "").toLowerCase() === "unauthorized";

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
    const entries = audit.data?.ok ? normalizeHistoryEntries(audit.data.entries) : [];
    entries.sort((a, b) => b.ts_unix_ms - a.ts_unix_ms);
    return entries;
  }, [audit.data]);

  const historyEntriesDesc = React.useMemo(() => {
    if (!activeJobId && liveEvents.length === 0) return auditEntriesDesc;
    const ts = typeof jobUpdatedMs === "number" && jobUpdatedMs > 0 ? jobUpdatedMs : Date.now();
    const live: HistoryEntry = {
      ts_unix_ms: ts,
      prompt: activeJobId ? lastRunPromptRef.current || lastRunPrompt || "" : "Live session event stream",
      assistant_text: "",
      events: liveEvents,
      ok: undefined,
      job_id: activeJobId || undefined,
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
    const status = typeof payload.status === "number" ? payload.status : 0;
    const code = String(payload.code || "").trim().toLowerCase();
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

  const dbMessageRows = React.useMemo(
    () => (dbMessages.data?.ok ? normalizeDbMessageRows(dbMessages.data.messages) : []),
    [dbMessages.data],
  );

  const dbRunRows = React.useMemo(
    () => (dbRuns.data?.ok ? normalizeDbRunSummaryRows(dbRuns.data.runs) : []),
    [dbRuns.data],
  );

  const [dbRunDetailsById, setDbRunDetailsById] = React.useState<Record<number, DbRunDetailRow>>({});
  const dbRunDetailsByIdRef = React.useRef<Record<number, DbRunDetailRow>>({});
  const dbRunDetailsLoadingRef = React.useRef<Record<number, boolean>>({});

  React.useEffect(() => {
    dbRunDetailsByIdRef.current = dbRunDetailsById || {};
  }, [dbRunDetailsById]);

  React.useEffect(() => {
    const runs = dbRunRows;
    if (runs.length === 0) return;
    const candidates: number[] = [];
    for (const run of runs.slice(0, 12)) {
      const runId = getDbRunNumericId(run);
      if (runId === null) continue;
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
          const detail = normalizeDbRunDetailRow(resp);
          if (detail?.ok) {
            setDbRunDetailsById((prev) => ({ ...(prev || {}), [runId]: detail }));
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
  }, [dbRunRows, daemonAuth, effectiveBase]);

  const sessionArtifactRows = React.useMemo(
    () => (sessionArtifacts.data?.ok ? normalizeSessionArtifactRows(sessionArtifacts.data.artifacts) : []),
    [sessionArtifacts.data],
  );

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
    dbMessageRows,
    dbRunDetailsById,
    dbRuns,
    dbRunRows,
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
    sessionArtifactRows,
    sessionArtifactsUnsupported,
    sessionClientEvents,
    sessionList,
    sessionScene,
  };
}
