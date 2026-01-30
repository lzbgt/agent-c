import React from "react";
import { apiPostSessionUiEvent } from "../api";

function safeString(v: any): string {
  return typeof v === "string" ? v : "";
}

function safeBool(v: any): boolean {
  return typeof v === "boolean" ? v : false;
}

function safeInt(v: any, def: number): number {
  if (typeof v !== "number" || !Number.isFinite(v)) return def;
  return Math.trunc(v);
}

function safeStringOrNull(v: any): string | null {
  const s = typeof v === "string" ? v.trim() : "";
  return s.length > 0 ? s : null;
}

function guessKind(path: string): "image" | "audio" | "video" | "text" | "file" {
  const lower = (path || "").toLowerCase();
  if (/\.(png|jpe?g|gif|webp|svg)$/.test(lower)) return "image";
  if (/\.(mp3|wav)$/.test(lower)) return "audio";
  if (/\.(mp4|webm|mov)$/.test(lower)) return "video";
  return "file";
}

export default function ArtifactView({
  baseUrl,
  yolo,
  artifact,
  allowAutoplay,
  sessionId,
  daemonAuthToken,
}: {
  baseUrl: string;
  yolo: boolean;
  artifact: any;
  allowAutoplay: boolean;
  sessionId?: string;
  daemonAuthToken?: string;
}) {
  const path = safeString(artifact?.path);
  const title = safeString(artifact?.title) || path || "artifact";
  const kind: string = safeString(artifact?.kind) || guessKind(path);
  const autoplay = safeBool(artifact?.autoplay);
  const repeat = Math.min(Math.max(safeInt(artifact?.repeat, 1), 1), 16);
  const toolCallId = safeStringOrNull(artifact?.tool_call_id) ?? safeStringOrNull(artifact?.source_tool_call_id);

  const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(path)}&yolo=${yolo ? "1" : "0"}`;

  const audioRef = React.useRef<HTMLAudioElement | null>(null);
  const [playError, setPlayError] = React.useState<string | null>(null);
  const playRemainingRef = React.useRef<number>(0);
  const lastPlayRequestRef = React.useRef<{ n: number; autoplay: boolean; started_ms: number } | null>(null);
  const renderedSentRef = React.useRef<boolean>(false);

  const postUiEvent = React.useCallback(
    async (etype: string, data?: any, appendToSession?: boolean) => {
      const sid = typeof sessionId === "string" ? sessionId.trim() : "";
      if (sid.length === 0) return;
      try {
        await apiPostSessionUiEvent(
          baseUrl,
          {
            session_id: sid,
            type: etype,
            data: data ?? {},
            append_to_session: typeof appendToSession === "boolean" ? appendToSession : false,
          },
          daemonAuthToken,
        );
      } catch {
        // Best-effort: never block media playback on event posting.
      }
    },
    [baseUrl, daemonAuthToken, sessionId],
  );

  const playTimes = React.useCallback(
    async (n: number) => {
      const el = audioRef.current;
      if (!el) return;
      setPlayError(null);
      playRemainingRef.current = Math.min(Math.max(n, 1), 16);
      lastPlayRequestRef.current = { n: playRemainingRef.current, autoplay, started_ms: Date.now() };
      try {
        el.currentTime = 0;
        await el.play();
        void postUiEvent(
          "audio_play_started",
          { path, title, repeat_requested: playRemainingRef.current, autoplay, tool_call_id: toolCallId },
          false,
        );
      } catch (e) {
        playRemainingRef.current = 0;
        setPlayError(String(e));
        void postUiEvent("audio_play_failed", { path, title, error: String(e), autoplay, tool_call_id: toolCallId }, false);
      }
    },
    [audioRef, autoplay, path, postUiEvent, title, toolCallId],
  );

  React.useEffect(() => {
    if (renderedSentRef.current) return;
    if (!path) return;
    if (!toolCallId) return; // avoid spamming acks for historic/browsed artifacts without a deterministic key
    renderedSentRef.current = true;
    void postUiEvent("artifact_rendered", { path, kind, title, tool_call_id: toolCallId }, false);
  }, [kind, path, postUiEvent, title, toolCallId]);

  React.useEffect(() => {
    const el = audioRef.current;
    if (!el) return;
    const onEnded = () => {
      if (playRemainingRef.current <= 1) {
        const last = lastPlayRequestRef.current;
        lastPlayRequestRef.current = null;
        playRemainingRef.current = 0;
        void postUiEvent(
          "audio_play_finished",
          {
          path,
          title,
          repeat_requested: last?.n ?? 1,
          autoplay: last?.autoplay ?? false,
          started_ms: last?.started_ms ?? 0,
          finished_ms: Date.now(),
          tool_call_id: toolCallId,
          },
          false,
        );
        return;
      }
      playRemainingRef.current -= 1;
      el.currentTime = 0;
      void el.play().catch((e) => {
        playRemainingRef.current = 0;
        setPlayError(String(e));
        void postUiEvent("audio_play_failed", { path, title, error: String(e), autoplay, tool_call_id: toolCallId }, false);
      });
    };
    el.addEventListener("ended", onEnded);
    return () => {
      el.removeEventListener("ended", onEnded);
    };
  }, [audioRef, autoplay, path, postUiEvent, title, toolCallId]);

  React.useEffect(() => {
    if (kind !== "audio") return;
    if (!autoplay) return;
    if (!allowAutoplay) return;
    // Best-effort: browsers may block autoplay without user gesture.
    void playTimes(repeat);
  }, [allowAutoplay, autoplay, kind, playTimes, repeat]);

  return (
    <div className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="text-xs text-white/60">{title}</div>
      {path ? <div className="mt-1 text-[11px] text-white/40">{path}</div> : null}

      {kind === "image" ? (
        <img src={src} className="mt-3 max-h-80 w-full rounded-md object-contain" />
      ) : kind === "video" ? (
        <video controls className="mt-3 w-full rounded-md">
          <source src={src} />
        </video>
      ) : kind === "audio" ? (
        <div className="mt-3">
          <audio ref={audioRef} controls className="w-full">
            <source src={src} />
          </audio>
          <div className="mt-2 flex flex-wrap gap-2">
            <button
              className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
              type="button"
              onClick={() => void playTimes(1)}
            >
              Play once
            </button>
            {repeat > 1 ? (
              <button
                className="rounded-md border border-white/10 bg-black/30 px-2 py-1 text-[11px] text-white/80 hover:bg-black/40"
                type="button"
                onClick={() => void playTimes(repeat)}
                title="Uses best-effort looping; browser autoplay policies may apply."
              >
                Play x{repeat}
              </button>
            ) : null}
            {autoplay ? (
              <div className="text-[11px] text-white/50">
                agent requested autoplay{repeat > 1 ? ` x${repeat}` : ""} {allowAutoplay ? "(enabled)" : "(disabled)"}
              </div>
            ) : null}
          </div>
          {playError ? (
            <div className="mt-2 text-[11px] text-amber-200/80">
              Audio playback failed (likely blocked by browser autoplay policy): {playError}
            </div>
          ) : null}
        </div>
      ) : (
        <div className="mt-3 text-xs text-white/70">
          <a
            className="text-sky-200 hover:underline"
            href={src}
            target="_blank"
            rel="noreferrer"
          >
            Download / open file
          </a>
        </div>
      )}
    </div>
  );
}
