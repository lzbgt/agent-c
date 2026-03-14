import React from "react";
import { useMutation } from "@tanstack/react-query";
import {
  apiAttachSession,
  apiDeleteSession,
  apiListSessions,
  apiNewSession,
  apiReleaseSessionAttachment,
  apiRenewSessionAttachment,
  apiUpdateDaemonConfig,
  extractSessionAttachment,
  extractSessionErrorEnvelope,
  extractSessionErrorMessage,
  extractSessionIds,
  type ApiAuth,
} from "../api";
import type { AppDataPlaneArgs, SessionLeaseConflict } from "./appDataPlaneTypes";

type MutationQueryHandles = {
  audit: { refetch: () => Promise<unknown> | unknown };
  daemonConfig: { refetch: () => Promise<unknown> | unknown };
  dbClientEvents: { refetch: () => Promise<unknown> | unknown };
  dbUiActions: { refetch: () => Promise<unknown> | unknown };
  sessionArtifacts: { refetch: () => Promise<unknown> | unknown };
  sessionClientEvents: { refetch: () => Promise<unknown> | unknown };
  sessionInfo: { refetch: () => Promise<unknown> | unknown };
  sessionScene: { refetch: () => Promise<unknown> | unknown };
  sessions: { isSuccess: boolean; data: Parameters<typeof extractSessionIds>[0]; refetch: () => Promise<unknown> | unknown };
};

type UseAppSessionMutationsArgs = Pick<
  AppDataPlaneArgs,
  | "activeJobId"
  | "apiKey"
  | "baseUrl"
  | "clientId"
  | "cursorRef"
  | "daemonAuth"
  | "effectiveBase"
  | "lastRunPromptRef"
  | "model"
  | "proxyUrl"
  | "selectedSessionId"
  | "sessionLeaseSeconds"
  | "summaryMaxChars"
  | "summaryModel"
  | "timeoutMs"
  | "setActiveJobId"
  | "setJobError"
  | "setJobStatus"
  | "setJobUpdatedMs"
  | "setLastCompletedPrompt"
  | "setLastRunPrompt"
  | "setLiveEvents"
  | "setPrompt"
  | "setResult"
  | "setSessionId"
> &
  MutationQueryHandles;

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

export function useAppSessionMutations(args: UseAppSessionMutationsArgs) {
  const {
    activeJobId,
    apiKey,
    audit,
    baseUrl,
    clientId,
    cursorRef,
    daemonAuth,
    daemonConfig,
    dbClientEvents,
    dbUiActions,
    effectiveBase,
    lastRunPromptRef,
    model,
    proxyUrl,
    selectedSessionId,
    sessionArtifacts,
    sessionClientEvents,
    sessionInfo,
    sessionLeaseSeconds,
    sessions,
    sessionScene,
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
  } = args;

  const [sessionLeaseConflict, setSessionLeaseConflict] = React.useState<SessionLeaseConflict | null>(null);
  React.useEffect(() => {
    setSessionLeaseConflict(null);
  }, [clientId, selectedSessionId]);

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
  }, [newSession, selectedSessionId, sessions.data, sessions.isSuccess]);

  const deleteSessionError = deleteSession.isError ? String(deleteSession.error) : null;
  const clearAllSessionsError = clearAllSessions.isError ? String(clearAllSessions.error) : null;
  const attachSessionError = attachSession.isError ? String(attachSession.error) : null;
  const renewSessionAttachmentError = renewSessionAttachment.isError ? String(renewSessionAttachment.error) : null;
  const releaseSessionAttachmentError = releaseSessionAttachment.isError ? String(releaseSessionAttachment.error) : null;

  return {
    attachSession,
    attachSessionError,
    clearAllSessions,
    clearAllSessionsError,
    clearDaemonApiKey,
    deleteSession,
    deleteSessionError,
    newSession,
    releaseSessionAttachment,
    releaseSessionAttachmentError,
    renewSessionAttachment,
    renewSessionAttachmentError,
    saveDaemonApiKey,
    saveDaemonDefaults,
    sessionLeaseConflict,
    setSessionLeaseConflict,
    updateDaemonDefaults,
  };
}
