import React from "react";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  apiGetSessionVoiceStats,
  apiPostSessionVoiceControl,
  type ApiAuth,
  type SessionVoiceControlReq,
  type SessionVoiceControlResp,
  type SessionVoiceStatsResp,
} from "../../api";

type UseVoicePanelStateArgs = {
  baseUrl: string;
  auth: ApiAuth;
  sessionId: string;
};

function parsePositiveInt(raw: string, fallback: number, max: number): number {
  const value = Number.parseInt(String(raw || "").trim(), 10);
  if (!Number.isFinite(value) || value <= 0) return fallback;
  return Math.min(value, max);
}

function parseVolume(raw: string): number | undefined {
  const trimmed = String(raw || "").trim();
  if (!trimmed) return undefined;
  const value = Number.parseFloat(trimmed);
  if (!Number.isFinite(value)) return undefined;
  return Math.max(0, Math.min(1, value));
}

export default function useVoicePanelState(args: UseVoicePanelStateArgs) {
  const baseUrl = String(args.baseUrl || "").trim().replace(/\/+$/, "");
  const sessionId = String(args.sessionId || "").trim();
  const canQuery = baseUrl.length > 0 && sessionId.length > 0;

  const [selector, setSelector] = React.useState<string>("#voice-audio");
  const [sourceUrl, setSourceUrl] = React.useState<string>("");
  const [title, setTitle] = React.useState<string>("Voice output");
  const [message, setMessage] = React.useState<string>("");
  const [muted, setMuted] = React.useState<boolean>(true);
  const [controls, setControls] = React.useState<boolean>(true);
  const [autoplay, setAutoplay] = React.useState<boolean>(true);
  const [loop, setLoop] = React.useState<boolean>(false);
  const [volumeText, setVolumeText] = React.useState<string>("1");
  const [maxBytesText, setMaxBytesText] = React.useState<string>(String(1024 * 1024));
  const [autoRefresh, setAutoRefresh] = React.useState<boolean>(true);
  const [actionError, setActionError] = React.useState<string | null>(null);
  const [lastActionResponse, setLastActionResponse] = React.useState<SessionVoiceControlResp | null>(null);

  const maxBytes = React.useMemo(() => parsePositiveInt(maxBytesText, 1024 * 1024, 16 * 1024 * 1024), [maxBytesText]);

  const statsQuery = useQuery({
    queryKey: ["sessionVoiceStats", baseUrl, sessionId, maxBytes],
    enabled: canQuery,
    queryFn: () => apiGetSessionVoiceStats(baseUrl, sessionId, args.auth, { maxBytes }),
    refetchInterval: canQuery && autoRefresh ? 5_000 : false,
  });

  const actionMutation = useMutation({
    mutationFn: async (action: "play" | "pause" | "snapshot") => {
      if (!canQuery) throw new Error("missing base URL or session_id");
      const payload: SessionVoiceControlReq = {
        session_id: sessionId,
        action,
      };
      const selectorTrimmed = String(selector || "").trim();
      if (selectorTrimmed) payload.selector = selectorTrimmed;
      if (action === "play") {
        const urlTrimmed = String(sourceUrl || "").trim();
        if (urlTrimmed) payload.url = urlTrimmed;
        const titleTrimmed = String(title || "").trim();
        if (titleTrimmed) payload.title = titleTrimmed;
        const messageTrimmed = String(message || "").trim();
        if (messageTrimmed) payload.message = messageTrimmed;
        payload.muted = muted;
        payload.controls = controls;
        payload.autoplay = autoplay;
        payload.loop = loop;
        const volume = parseVolume(volumeText);
        if (typeof volume === "number") payload.volume = volume;
      }
      return apiPostSessionVoiceControl(baseUrl, payload, args.auth);
    },
    onMutate: () => setActionError(null),
    onSuccess: async (response) => {
      setLastActionResponse(response);
      await statsQuery.refetch();
    },
    onError: (error) => {
      setActionError(String(error));
    },
  });

  const counts: NonNullable<SessionVoiceStatsResp["counts"]> = statsQuery.data?.counts ?? {};
  const clients: NonNullable<SessionVoiceStatsResp["clients"]> = statsQuery.data?.clients ?? [];
  const recentResults: NonNullable<SessionVoiceStatsResp["recent_results"]> = statsQuery.data?.recent_results ?? [];

  return {
    actionError,
    actionMutation,
    autoRefresh,
    canQuery,
    clients,
    controls,
    counts,
    lastActionResponse,
    loop,
    maxBytes,
    maxBytesText,
    message,
    muted,
    recentResults,
    selector,
    sessionId,
    setAutoRefresh,
    setControls,
    setLoop,
    setMaxBytesText,
    setMessage,
    setMuted,
    setSelector,
    setSourceUrl,
    setTitle,
    setVolumeText,
    sourceUrl,
    statsQuery,
    title,
    volumeText,
    autoplay,
    setAutoplay,
  };
}
