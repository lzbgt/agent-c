import React from "react";
import { apiPostSessionUiEvent, daemonFetchInit, type ApiAuth } from "../api";

function safeString(v: any): string {
  return typeof v === "string" ? v : "";
}

function safeStringOrNull(v: any): string | null {
  const s = typeof v === "string" ? v.trim() : "";
  return s.length > 0 ? s : null;
}

function guessKind(path: string): "image" | "audio" | "video" | "text" | "file" {
  const lower = (path || "").toLowerCase();
  if (/\.(png|jpe?g|gif|webp|svg)$/.test(lower)) return "image";
  if (/\.(mp3|wav|aiff|aif|aifc|m4a|ogg)$/.test(lower)) return "audio";
  if (/\.(mp4|webm|mov)$/.test(lower)) return "video";
  if (/\.(txt|md|json|log|csv)$/.test(lower)) return "text";
  return "file";
}

function isAbsoluteLikePath(p: string): boolean {
  const s = (p || "").trim();
  if (!s) return false;
  // Unix absolute
  if (s.startsWith("/")) return true;
  // Windows drive absolute: C:\...
  if (/^[a-zA-Z]:[\\/]/.test(s)) return true;
  // Windows UNC: \\server\share
  if (s.startsWith("\\\\")) return true;
  return false;
}

export default function ArtifactView({
  baseUrl,
  yolo,
  artifact,
  // Kept for backwards compatibility with existing call sites.
  // The Web UI should not hardcode audio playback; the agent can drive client-side presentation via RPCs.
  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  allowAutoplay,
  sessionId,
  client,
  daemonAuth,
}: {
  baseUrl: string;
  yolo: boolean;
  artifact: any;
  allowAutoplay: boolean;
  sessionId?: string;
  client?: { id?: string; kind?: string; instance_id?: string };
  daemonAuth?: ApiAuth;
}) {
  const path = safeString(artifact?.path);
  const resolvedPath = safeString(artifact?.resolved_path);
  const kind: string = safeString(artifact?.kind) || guessKind(path);
  const title = safeString(artifact?.title) || path || "artifact";
  const toolCallId = safeStringOrNull(artifact?.tool_call_id) ?? safeStringOrNull(artifact?.source_tool_call_id);

  // Artifact path agreement (WebUI client profile):
  // - Prefer a *relative* `artifact.path` so agentd can serve it from the session folder.
  // - Fall back to `resolved_path` only when the tool/agent produced an absolute path (or omitted `path`).
  const preferredFetchPath = path && !isAbsoluteLikePath(path) ? path : yolo && resolvedPath ? resolvedPath : path;
  const fallbackFetchPath =
    yolo && preferredFetchPath === path && path && !isAbsoluteLikePath(path) && resolvedPath && isAbsoluteLikePath(resolvedPath)
      ? resolvedPath
      : "";

  const [activeFetchPath, setActiveFetchPath] = React.useState<string>(preferredFetchPath);
  React.useEffect(() => {
    setActiveFetchPath(preferredFetchPath);
  }, [preferredFetchPath]);

  const sid = safeStringOrNull(sessionId) ?? "";
  const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
  const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(activeFetchPath)}&yolo=${yolo ? "1" : "0"}${sidQ}`;

  const [blobUrl, setBlobUrl] = React.useState<string | null>(null);
  const [fetchError, setFetchError] = React.useState<string | null>(null);
  const [contentType, setContentType] = React.useState<string>("");
  const [retryNonce, setRetryNonce] = React.useState<number>(0);
  const [loading, setLoading] = React.useState<boolean>(false);
  const failedSrcRef = React.useRef<string>("");
  const renderedSentRef = React.useRef<boolean>(false);
  const renderFailedSentRef = React.useRef<boolean>(false);
  const globalAckMap = React.useMemo(() => {
    const g: any = typeof globalThis !== "undefined" ? (globalThis as any) : {};
    if (!g.__agentui_artifact_acked || typeof g.__agentui_artifact_acked !== "object") {
      g.__agentui_artifact_acked = {};
    }
    return g.__agentui_artifact_acked as Record<string, boolean>;
  }, []);

  // Reset per-artifact ack state when switching to a different artifact (or different fetch path).
  React.useEffect(() => {
    renderedSentRef.current = false;
    renderFailedSentRef.current = false;
  }, [activeFetchPath, toolCallId]);

  const postUiEvent = React.useCallback(
    async (etype: string, data?: any) => {
      const sid = typeof sessionId === "string" ? sessionId.trim() : "";
      if (sid.length === 0) return;
      try {
        await apiPostSessionUiEvent(
          baseUrl,
          {
            session_id: sid,
            type: etype,
            client: client ?? { id: "webui", kind: "webui" },
            data: data ?? {},
            append_to_session: false,
          },
          daemonAuth,
        );
      } catch {
        // Best-effort: never block UI rendering on event posting.
      }
    },
    [baseUrl, client, daemonAuth, sessionId],
  );

  const postArtifactRendered = React.useCallback(async () => {
    if (renderedSentRef.current) return;
    if (renderFailedSentRef.current) return;
    if (!activeFetchPath) return;
    if (!toolCallId) return; // avoid spamming acks for historic/browsed artifacts without a deterministic key
    const sid = typeof sessionId === "string" ? sessionId.trim() : "";
    const gk = `${baseUrl}::${sid}::${toolCallId}`;
    if (globalAckMap[gk]) return;
    renderedSentRef.current = true;
    globalAckMap[gk] = true;
    await postUiEvent("artifact_rendered", {
      path,
      resolved_path: resolvedPath || undefined,
      fetch_path: activeFetchPath,
      kind,
      title,
      tool_call_id: toolCallId,
      content_type: contentType || undefined,
    });
  }, [activeFetchPath, contentType, kind, path, postUiEvent, resolvedPath, title, toolCallId]);

  const postArtifactRenderFailed = React.useCallback(
    async (reason: string) => {
      if (renderFailedSentRef.current) return;
      if (renderedSentRef.current) return;
      if (!activeFetchPath) return;
      if (!toolCallId) return;
      const sid = typeof sessionId === "string" ? sessionId.trim() : "";
      const gk = `${baseUrl}::${sid}::${toolCallId}`;
      if (globalAckMap[gk]) return;
      renderFailedSentRef.current = true;
      globalAckMap[gk] = true;
      await postUiEvent("artifact_render_failed", {
        path,
        resolved_path: resolvedPath || undefined,
        fetch_path: activeFetchPath,
        kind,
        title,
        tool_call_id: toolCallId,
        content_type: contentType || undefined,
        error: String(reason || "failed"),
      });
    },
    [activeFetchPath, contentType, kind, path, postUiEvent, resolvedPath, title, toolCallId],
  );

  // Fetch bytes (with Authorization when configured) so:
  // - authenticated daemons still work (media tags cannot set headers)
  // - we can deterministically mark artifact_rendered vs artifact_render_failed
  //
  // NOTE: the UI intentionally does not hardcode media presentation (no <audio>/<video>).
  // The agent can present/play/visualize via client RPCs (dom_apply/page_eval/script_eval/entity_apply).
  React.useEffect(() => {
    if (!activeFetchPath) {
      setBlobUrl(null);
      setFetchError(null);
      setContentType("");
      setLoading(false);
      return;
    }
    if (failedSrcRef.current === src && retryNonce === 0) {
      return;
    }

    const ac = new AbortController();
    let revoked: string | null = null;
    setLoading(true);

    void (async () => {
      try {
        setFetchError(null);
        setContentType("");

        const r = await fetch(src, daemonFetchInit(daemonAuth, { method: "GET", signal: ac.signal }));
        if (!r.ok) {
          // In YOLO mode, try an absolute resolved_path fallback.
          if (fallbackFetchPath && activeFetchPath !== fallbackFetchPath) {
            setActiveFetchPath(fallbackFetchPath);
            return;
          }
          failedSrcRef.current = src;
          throw new Error(`file fetch failed: ${r.status}`);
        }
        const ct = String(r.headers.get("content-type") || "").trim();
        if (ct) setContentType(ct);

        const b = await r.blob();
        const u = URL.createObjectURL(b);
        revoked = u;
        setBlobUrl(u);
      } catch (e) {
        const name = typeof (e as any)?.name === "string" ? String((e as any).name) : "";
        if (ac.signal.aborted || name === "AbortError") {
          return;
        }
        setFetchError(String(e));
        setBlobUrl(null);
      } finally {
        setLoading(false);
      }
    })();

    return () => {
      ac.abort();
      if (revoked) {
        try {
          URL.revokeObjectURL(revoked);
        } catch {
          // ignore
        }
      }
    };
  }, [activeFetchPath, daemonAuth, fallbackFetchPath, retryNonce, src]);

  // Deterministic acknowledgements.
  React.useEffect(() => {
    if (!activeFetchPath) return;
    if (!toolCallId) return;
    if (fetchError) return;
    if (blobUrl) void postArtifactRendered().catch(() => {});
  }, [activeFetchPath, blobUrl, fetchError, postArtifactRendered, toolCallId]);

  React.useEffect(() => {
    if (!activeFetchPath) return;
    if (!toolCallId) return;
    if (fetchError) void postArtifactRenderFailed(fetchError).catch(() => {});
  }, [activeFetchPath, fetchError, postArtifactRenderFailed, toolCallId]);

  const openHref = blobUrl || src;

  return (
    <div className="rounded-md border border-white/10 bg-black/20 p-3">
      <div className="text-xs text-white/60">{title}</div>
      {path ? <div className="mt-1 text-[11px] text-white/40">{path}</div> : null}
      {resolvedPath && resolvedPath !== path ? (
        <div className="mt-1 text-[11px] text-white/30">
          resolved: <span className="font-mono">{resolvedPath}</span>
        </div>
      ) : null}

      <div className="mt-2 flex flex-wrap items-center gap-2 text-[11px] text-white/50">
        {kind ? (
          <span>
            kind=<code className="text-white/70">{kind}</code>
          </span>
        ) : null}
        {contentType ? (
          <span>
            content-type=<code className="text-white/70">{contentType}</code>
          </span>
        ) : null}
        {loading ? <span className="text-white/60">loading…</span> : null}
      </div>

      {fetchError ? (
        <div className="mt-3 rounded-md border border-rose-500/30 bg-rose-500/10 px-3 py-2 text-xs text-rose-200">
          <div className="font-semibold">Load failed</div>
          <div className="mt-1 text-rose-200/90">
            {fetchError}{" "}
            {activeFetchPath ? (
              <>
                (<code className="text-rose-100/90">{activeFetchPath}</code>)
              </>
            ) : null}
          </div>
          <div className="mt-2">
            <button
              className="rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200 hover:bg-rose-500/15"
              type="button"
              onClick={() => {
                failedSrcRef.current = "";
                setRetryNonce((n) => n + 1);
              }}
            >
              Retry
            </button>
          </div>
        </div>
      ) : null}

      <div className="mt-3 text-xs text-white/70">
        <a
          className="text-sky-200 hover:underline"
          href={openHref}
          target="_blank"
          rel="noreferrer"
          download={activeFetchPath ? activeFetchPath.split("/").pop() || "artifact" : undefined}
        >
          Download / open file
        </a>
      </div>
    </div>
  );
}
