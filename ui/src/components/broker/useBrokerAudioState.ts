import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiBrokerCreateAudioSession,
  apiBrokerDeleteAudioSession,
  apiBrokerGetAudioSession,
  apiBrokerListAudioSessions,
  apiBrokerSendAudioSignal,
  BrokerAudioSignalEventSchema,
  daemonFetchInit,
  type ApiAuth,
} from "../../api";
import { readSseStream } from "../../sse";

const MAX_SIGNAL_EVENTS = 50;

type UseBrokerAudioStateArgs = {
  base: string;
  auth: ApiAuth;
  canQuery: boolean;
  agentId: string;
  defaultDeploymentId?: string;
};

export default function useBrokerAudioState(args: UseBrokerAudioStateArgs) {
  const [deploymentId, setDeploymentId] = React.useState<string>(String(args.defaultDeploymentId || "").trim());
  const [metadataText, setMetadataText] = React.useState<string>('{"client":"webui","surface":"broker-panel"}');
  const [selectedSessionId, setSelectedSessionId] = React.useState<string>("");
  const [signalType, setSignalType] = React.useState<string>("control");
  const [signalPayloadText, setSignalPayloadText] = React.useState<string>('{"state":"ready"}');
  const [signalEvents, setSignalEvents] = React.useState<Array<{ type: string; from?: string; ts_unix_ms: number; payload?: unknown }>>(
    [],
  );
  const [streamConnected, setStreamConnected] = React.useState<boolean>(false);
  const [streamError, setStreamError] = React.useState<string | null>(null);
  const [createError, setCreateError] = React.useState<string | null>(null);
  const [sendError, setSendError] = React.useState<string | null>(null);
  const [deleteError, setDeleteError] = React.useState<string | null>(null);

  React.useEffect(() => {
    if (deploymentId) return;
    const next = String(args.defaultDeploymentId || "").trim();
    if (next) setDeploymentId(next);
  }, [args.defaultDeploymentId, deploymentId]);

  const sessionsQuery = useQuery({
    queryKey: ["brokerAudioSessions", args.base, args.agentId, deploymentId, args.canQuery],
    enabled: false,
    queryFn: () => apiBrokerListAudioSessions(args.base, args.auth, { agentId: args.agentId, deploymentId }),
  });

  const sessionQuery = useQuery({
    queryKey: ["brokerAudioSession", args.base, selectedSessionId, args.canQuery],
    enabled: false,
    queryFn: () => apiBrokerGetAudioSession(args.base, selectedSessionId, args.auth),
  });

  React.useEffect(() => {
    if (!args.canQuery || !args.agentId) return;
    void sessionsQuery.refetch();
  }, [args.agentId, args.canQuery, deploymentId]); // eslint-disable-line react-hooks/exhaustive-deps

  React.useEffect(() => {
    const sessions = sessionsQuery.data?.sessions || [];
    if (!selectedSessionId && sessions.length > 0) {
      setSelectedSessionId(String(sessions[0]?.session_id || ""));
      return;
    }
    if (selectedSessionId && sessions.length > 0 && !sessions.some((row) => row?.session_id === selectedSessionId)) {
      setSelectedSessionId(String(sessions[0]?.session_id || ""));
    }
    if (selectedSessionId && sessions.length === 0) {
      setSelectedSessionId("");
    }
  }, [selectedSessionId, sessionsQuery.data?.sessions]);

  React.useEffect(() => {
    if (!args.canQuery || !selectedSessionId) return;
    void sessionQuery.refetch();
  }, [args.canQuery, selectedSessionId]); // eslint-disable-line react-hooks/exhaustive-deps

  const createMutation = useMutation({
    mutationFn: async () => {
      const agentId = String(args.agentId || "").trim();
      if (!agentId) throw new Error("select an agent first");
      let metadata: Record<string, unknown> | undefined;
      const trimmed = String(metadataText || "").trim();
      if (trimmed) {
        const parsed = JSON.parse(trimmed);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          metadata = parsed as Record<string, unknown>;
        } else {
          throw new Error("metadata must be a JSON object");
        }
      }
      const resp = await apiBrokerCreateAudioSession(
        args.base,
        {
          agent_id: agentId,
          deployment_id: String(deploymentId || "").trim() || undefined,
          mode: "webrtc",
          metadata,
        },
        args.auth,
      );
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "audio session create failed");
      return resp;
    },
    onMutate: () => setCreateError(null),
    onSuccess: async (resp) => {
      setSelectedSessionId(String(resp.session_id || ""));
      await sessionsQuery.refetch();
    },
    onError: (err) => setCreateError(String(err)),
  });

  const sendMutation = useMutation({
    mutationFn: async () => {
      const sid = String(selectedSessionId || "").trim();
      if (!sid) throw new Error("select a session first");
      const type = String(signalType || "").trim();
      if (!type) throw new Error("select a signal type");
      let payload: Record<string, unknown> | undefined;
      const trimmed = String(signalPayloadText || "").trim();
      if (trimmed) {
        const parsed = JSON.parse(trimmed);
        if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
          payload = parsed as Record<string, unknown>;
        } else {
          throw new Error("signal payload must be a JSON object");
        }
      }
      const resp = await apiBrokerSendAudioSignal(args.base, sid, { type, payload }, args.auth);
      if (!resp.ok) throw new Error(resp.error || resp.err || resp.code || "audio signal send failed");
      return { type };
    },
    onMutate: () => setSendError(null),
    onSuccess: async ({ type }) => {
      await sessionQuery.refetch();
      await sessionsQuery.refetch();
      if (type === "bye") {
        setSelectedSessionId("");
      }
    },
    onError: (err) => setSendError(String(err)),
  });

  const deleteMutation = useMutation({
    mutationFn: async () => {
      const sid = String(selectedSessionId || "").trim();
      if (!sid) throw new Error("select a session first");
      const resp = await apiBrokerDeleteAudioSession(args.base, sid, args.auth);
      if (!resp.ok || !resp.deleted) throw new Error(resp.error || resp.err || resp.code || "audio session delete failed");
      return resp;
    },
    onMutate: () => setDeleteError(null),
    onSuccess: async () => {
      setSelectedSessionId("");
      await sessionsQuery.refetch();
    },
    onError: (err) => setDeleteError(String(err)),
  });

  React.useEffect(() => {
    const sid = String(selectedSessionId || "").trim();
    if (!args.canQuery || !args.base || !sid) {
      setStreamConnected(false);
      return;
    }
    const ac = new AbortController();
    setStreamError(null);
    setStreamConnected(false);
    setSignalEvents([]);

    void (async () => {
      try {
        const resp = await fetch(
          `${args.base.replace(/\/+$/, "")}/v1/audio/sessions/${encodeURIComponent(sid)}/signal/stream`,
          daemonFetchInit(args.auth, { signal: ac.signal }),
        );
        if (!resp.ok) {
          throw new Error(`signal stream failed (${resp.status})`);
        }
        setStreamConnected(true);
        await readSseStream(resp, (ev) => {
          if (ev.event !== "signal") return;
          try {
            const parsed = BrokerAudioSignalEventSchema.parse(JSON.parse(ev.data));
            setSignalEvents((prev) => {
              const next = prev.concat(parsed);
              return next.length <= MAX_SIGNAL_EVENTS ? next : next.slice(next.length - MAX_SIGNAL_EVENTS);
            });
          } catch {
            // ignore malformed event payloads
          }
        });
      } catch (err) {
        if (!ac.signal.aborted) {
          setStreamError(String(err));
        }
      } finally {
        if (!ac.signal.aborted) {
          setStreamConnected(false);
        }
      }
    })();

    return () => ac.abort();
  }, [args.auth, args.base, args.canQuery, selectedSessionId]);

  const sessions = sessionsQuery.data?.sessions || [];
  const selectedSession = sessions.find((row) => row?.session_id === selectedSessionId) || sessionQuery.data?.session || null;

  return {
    deploymentId,
    setDeploymentId,
    metadataText,
    setMetadataText,
    selectedSessionId,
    setSelectedSessionId,
    signalType,
    setSignalType,
    signalPayloadText,
    setSignalPayloadText,
    signalEvents,
    streamConnected,
    streamError,
    createError,
    sendError,
    deleteError,
    createMutation,
    sendMutation,
    deleteMutation,
    sessionsQuery,
    sessionQuery,
    sessions,
    selectedSession,
  };
}
