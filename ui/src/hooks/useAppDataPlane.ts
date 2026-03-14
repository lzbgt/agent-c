import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiAttachSession,
  apiDeleteSession,
  apiGetAudit,
  apiGetConfig,
  apiGetDbClientEvents,
  apiGetDbMessages,
  apiGetDbRun,
  apiGetDbRuns,
  apiGetDbUiActions,
  apiGetHealth,
  apiGetSessionArtifacts,
  apiGetSessionClientEvents,
  apiGetSession,
  apiGetSessionScene,
  apiListSessions,
  apiNewSession,
  apiReleaseSessionAttachment,
  apiRenewSessionAttachment,
  apiUpdateDaemonConfig,
  extractSessionAttachment,
  extractSessionErrorEnvelope,
  extractSessionErrorMessage,
  extractSessionIds,
  extractSessionInfo,
  type AgentEvent,
  type ApiAuth,
  type RunResponse,
  type SessionAttachment,
  type SessionInfo,
} from "../api";

export type AppDataPlaneArgs = {
  activeJobId: string | null;
  allowClientEffects: boolean;
  allowClientRpcs: boolean;
  apiKey: string;
  authKey: string;
  baseUrl: string;
  brokerAuthToken: string;
  brokerCookieAuth: boolean;
  clientId: string;
  connectionMode: string;
  daemonAuth: ApiAuth;
  daemonAuthToken: string;
  effectiveBase: string;
  jobStatus: string | null;
  jobUpdatedMs: number | null;
  lastRunPrompt: string;
  lastRunPromptRef: React.MutableRefObject<string>;
  liveEvents: AgentEvent[];
  model: string;
  proxyUrl: string;
  selectedSessionId: string;
  setActiveJobId: React.Dispatch<React.SetStateAction<string | null>>;
  setJobError: React.Dispatch<React.SetStateAction<string | null>>;
  setJobStatus: React.Dispatch<React.SetStateAction<string | null>>;
  setJobUpdatedMs: React.Dispatch<React.SetStateAction<number | null>>;
  setLastCompletedPrompt: React.Dispatch<React.SetStateAction<string>>;
  setLastRunPrompt: React.Dispatch<React.SetStateAction<string>>;
  setLiveEvents: React.Dispatch<React.SetStateAction<AgentEvent[]>>;
  setPrompt: React.Dispatch<React.SetStateAction<string>>;
  setResult: React.Dispatch<React.SetStateAction<RunResponse | undefined>>;
  setSessionId: (sid: string) => void;
  sessionLeaseSeconds: string;
  summaryMaxChars: string;
  summaryModel: string;
  timeoutMs: string;
  cursorRef: React.MutableRefObject<number>;
};

export type SessionLeaseConflict = {
  requestedClientId: string | null;
  currentAttachment?: SessionAttachment;
  code: string;
  message: string;
  retryable: boolean;
};

function parseLeaseSeconds(raw: string): number {
  const parsed = Number(raw);
  return Number.isFinite(parsed) && parsed > 0 ? Math.floor(parsed) : 90;
}

function buildLeaseConflict(payload: unknown): SessionLeaseConflict | null {
  const errorEnvelope = extractSessionErrorEnvelope(payload);
  const code = String(errorEnvelope?.code || (payload && typeof payload === "object" ? (payload as any).code : "") || "").trim();
  if (code !== "attachment_conflict") return null;
  const details = errorEnvelope?.details && typeof errorEnvelope.details === "object" ? errorEnvelope.details : {};
  return {
    requestedClientId:
      typeof (details as any).requested_client_id === "string"
        ? String((details as any).requested_client_id)
        : (details as any).requested_client_id === null
          ? null
          : null,
    currentAttachment: extractSessionAttachment({ attachment: (details as any).current_attachment }),
    code,
    message: String(errorEnvelope?.message || "active attachment lease blocks this mutation"),
    retryable: errorEnvelope?.retryable !== false,
  };
}

export default function useAppDataPlane(args: AppDataPlaneArgs) {
  const {
    activeJobId,
    allowClientEffects,
    allowClientRpcs,
    apiKey,
    authKey,
    baseUrl,
    brokerAuthToken,
    brokerCookieAuth,
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
    selectedSessionId,
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

  const [sessionLeaseConflict, setSessionLeaseConflict] = React.useState<SessionLeaseConflict | null>(null);
  const sessionInfoData = React.useMemo<SessionInfo | undefined>(() => extractSessionInfo(sessionInfo.data), [sessionInfo.data]);
  React.useEffect(() => {
    setSessionLeaseConflict(null);
  }, [clientId, selectedSessionId]);

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
  }, [dbRuns.data, effectiveBase, daemonAuth]);

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

  const newSession = useMutation({
    mutationFn: async () => {
      return await apiNewSession(effectiveBase, daemonAuth, {
        clientId,
        leaseSeconds: parseLeaseSeconds(sessionLeaseSeconds),
      });
    },
    onSuccess: (resp) => {
      const nextSessionId = String(resp.session?.session_id || resp.session_id || "").trim();
      if (resp.ok && nextSessionId) {
        setSessionLeaseConflict(null);
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
        setSessionId(nextSessionId);
        void sessions.refetch();
        void sessionInfo.refetch();
        void audit.refetch();
        void sessionClientEvents.refetch();
        void sessionArtifacts.refetch();
        void sessionScene.refetch();
        void dbUiActions.refetch();
        void dbClientEvents.refetch();
      }
    },
  });

  const attachSession = useMutation({
    mutationFn: async (sid: string) => {
      const trimmed = String(sid || "").trim();
      if (!trimmed) throw new Error("missing session id");
      const resp = await apiAttachSession(effectiveBase, trimmed, daemonAuth, {
        clientId,
        leaseSeconds: parseLeaseSeconds(sessionLeaseSeconds),
      });
      if (!resp.ok) {
        const conflict = buildLeaseConflict(resp);
        if (conflict) {
          setSessionLeaseConflict(conflict);
          return resp;
        }
        throw new Error(extractSessionErrorMessage(resp) || "attach failed");
      }
      return resp;
    },
    onSuccess: async (resp, sid) => {
      if (!resp?.ok) return;
      setSessionLeaseConflict(null);
      const nextSessionId = String(resp.session?.session_id || resp.session_id || sid || "").trim();
      if (nextSessionId) setSessionId(nextSessionId);
      await sessions.refetch();
      await sessionInfo.refetch();
    },
  });

  const renewSessionAttachment = useMutation({
    mutationFn: async (sid: string) => {
      const trimmed = String(sid || "").trim();
      if (!trimmed) throw new Error("missing session id");
      const resp = await apiRenewSessionAttachment(effectiveBase, trimmed, daemonAuth, {
        clientId,
        leaseSeconds: parseLeaseSeconds(sessionLeaseSeconds),
      });
      if (!resp.ok) {
        const conflict = buildLeaseConflict(resp);
        if (conflict) {
          setSessionLeaseConflict(conflict);
          return resp;
        }
        throw new Error(extractSessionErrorMessage(resp) || "renew failed");
      }
      return resp;
    },
    onSuccess: async (resp) => {
      if (!resp?.ok) return;
      setSessionLeaseConflict(null);
      await sessionInfo.refetch();
    },
  });

  const releaseSessionAttachment = useMutation({
    mutationFn: async (sid: string) => {
      const trimmed = String(sid || "").trim();
      if (!trimmed) throw new Error("missing session id");
      const resp = await apiReleaseSessionAttachment(effectiveBase, trimmed, daemonAuth, {
        clientId,
      });
      if (!resp.ok) {
        const conflict = buildLeaseConflict(resp);
        if (conflict) {
          setSessionLeaseConflict(conflict);
          return resp;
        }
        throw new Error(extractSessionErrorMessage(resp) || "release failed");
      }
      return resp;
    },
    onSuccess: async (resp) => {
      if (!resp?.ok) return;
      setSessionLeaseConflict(null);
      await sessionInfo.refetch();
    },
  });

  const deleteSession = useMutation({
    mutationFn: async (sid: string) => {
      const trimmed = String(sid || "").trim();
      if (!trimmed) throw new Error("missing session id");
      const resp = await apiDeleteSession(effectiveBase, trimmed, daemonAuth);
      if (!resp.ok) throw new Error(extractSessionErrorMessage(resp) || "delete failed");
      return { session_id: trimmed };
    },
    onSuccess: async (resp) => {
      await sessions.refetch();
      if (resp.session_id === String(selectedSessionId || "").trim()) {
        await newSession.mutateAsync();
      } else {
        await audit.refetch();
      }
    },
  });

  const updateDaemonDefaults = useMutation({
    mutationFn: async (payload: any) => {
      const resp = await apiUpdateDaemonConfig(effectiveBase, payload, daemonAuth);
      if (!resp.ok) throw new Error(resp.error || "update failed");
      return resp;
    },
    onSuccess: async () => {
      await daemonConfig.refetch();
    },
  });

  const inferredProvider = React.useMemo(() => {
    const value = String(baseUrl || "").toLowerCase();
    if (value.includes("deepseek")) return "deepseek";
    if (value.includes("openrouter")) return "openrouter";
    if (value.includes("moonshot") || value.includes("kimi")) return "moonshot";
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
      const resp = await apiListSessions(effectiveBase, daemonAuth);
      if (!resp.ok) throw new Error(extractSessionErrorMessage(resp) || "failed to list sessions");
      const ids = extractSessionIds(resp);
      for (const sid of ids) {
        const deletion = await apiDeleteSession(effectiveBase, sid, daemonAuth);
        if (!deletion.ok) throw new Error(extractSessionErrorMessage(deletion) || `failed to delete session: ${sid}`);
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
    const ids = extractSessionIds(sessions.data);
    if (selectedSessionId === "default" && !ids.includes("default")) {
      autoSessionInitRef.current = true;
      void newSession.mutateAsync().catch(() => {});
    }
  }, [selectedSessionId, sessions.data, sessions.isSuccess, newSession]);

  const auditRefetch = audit.refetch;
  const sessionsRefetch = sessions.refetch;
  const sessionList = React.useMemo(() => extractSessionIds(sessions.data), [sessions.data]);
  const deleteSessionError = deleteSession.isError ? String(deleteSession.error) : null;
  const clearAllSessionsError = clearAllSessions.isError ? String(clearAllSessions.error) : null;
  const attachSessionError = attachSession.isError ? String(attachSession.error) : null;
  const renewSessionAttachmentError = renewSessionAttachment.isError ? String(renewSessionAttachment.error) : null;
  const releaseSessionAttachmentError = releaseSessionAttachment.isError ? String(releaseSessionAttachment.error) : null;

  return {
    audit,
    auditEntriesDesc,
    auditRefetch,
    clearAllSessions,
    clearAllSessionsError,
    daemonConfig,
    dbClientEvents,
    dbMessages,
    dbRunDetailsById,
    dbRuns,
    dbUiActions,
    deleteSession,
    deleteSessionError,
    health,
    historyEntriesDesc,
    isLocalDaemonBase,
    missingBrokerAuthToken,
    missingDaemonAuthToken,
    sessionInfo,
    sessionInfoData,
    sessionLeaseConflict,
    setSessionLeaseConflict,
    newSession,
    attachSession,
    attachSessionError,
    renewSessionAttachment,
    renewSessionAttachmentError,
    releaseSessionAttachment,
    releaseSessionAttachmentError,
    saveDaemonApiKey,
    saveDaemonDefaults,
    sessions,
    sessionsRefetch,
    sessionsUnauthorized,
    sessionArtifacts,
    sessionScene,
    sessionList,
    clearDaemonApiKey,
    updateDaemonDefaults,
  };
}
