import React from "react";
import { daemonHeaders, type ApiAuth } from "../api";

function uniq<T>(arr: T[]): T[] {
  return Array.from(new Set(arr));
}

function guessKind(path: string): "image" | "audio" | "video" | null {
  const lower = path.toLowerCase();
  if (/\.(png|jpe?g|gif|webp|svg)$/.test(lower)) return "image";
  if (/\.(mp3|wav|ogg|m4a|aac|flac|aiff|aif)$/.test(lower)) return "audio";
  if (/\.(mp4|webm|mov|mkv)$/.test(lower)) return "video";
  return null;
}

export default function MediaPreviews({
  baseUrl,
  yolo,
  daemonAuth,
  sessionId,
  text,
}: {
  baseUrl: string;
  yolo: boolean;
  daemonAuth?: ApiAuth;
  sessionId?: string;
  text: string;
}) {
  // Best-effort: extract file-looking tokens from text output.
  const matches = Array.from(
    text.matchAll(
      /(^|[\s'"])([^\s'"]+\.(?:png|jpe?g|gif|webp|svg|mp3|wav|ogg|m4a|aac|flac|aiff|aif|mp4|webm|mov|mkv))([\s'"]|$)/gim,
    ),
  ).map((m) => m[2]);

  const files = uniq(matches)
    .map((p) => p.trim())
    .filter((p) => p.length > 0)
    .slice(0, 8);

  if (files.length === 0) return null;

  const hdr = React.useMemo(() => daemonHeaders(daemonAuth), [daemonAuth]);
  const hasAuthHeaders = React.useMemo(() => {
    return typeof hdr.Authorization === "string" || typeof (hdr as any)["X-Agentd-Authorization"] === "string";
  }, [hdr]);
  const sid = typeof sessionId === "string" ? sessionId.trim() : "";
  const sidQ = sid ? `&session_id=${encodeURIComponent(sid)}` : "";
  const [blobByPath, setBlobByPath] = React.useState<Record<string, { url: string; contentType?: string; error?: string }>>({});

  React.useEffect(() => {
    // When auth is enabled, media elements cannot attach Authorization headers.
    // Workaround: fetch with headers here and use blob: URLs.
    if (!hasAuthHeaders) {
      // Cleanup any prior blob URLs.
      const prev = blobByPath;
      Object.values(prev).forEach((v) => {
        if (v && v.url && v.url.startsWith("blob:")) {
          try {
            URL.revokeObjectURL(v.url);
          } catch {
            // ignore
          }
        }
      });
      setBlobByPath({});
      return;
    }

    let cancelled = false;
    const created: string[] = [];
    (async () => {
      for (const path of files) {
        if (cancelled) return;
        const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(path)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
        try {
          const r = await fetch(src, { headers: hdr });
          if (!r.ok) throw new Error(`HTTP ${r.status}`);
          const ct = String(r.headers.get("content-type") || "").trim();
          const b = await r.blob();
          const u = URL.createObjectURL(b);
          created.push(u);
          if (cancelled) return;
          setBlobByPath((prev) => ({ ...prev, [path]: { url: u, contentType: ct || undefined } }));
        } catch (e) {
          if (cancelled) return;
          setBlobByPath((prev) => ({ ...prev, [path]: { url: "", error: String(e) } }));
        }
      }
    })();

    return () => {
      cancelled = true;
      created.forEach((u) => {
        try {
          URL.revokeObjectURL(u);
        } catch {
          // ignore
        }
      });
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, hdr, hasAuthHeaders, yolo, files.join("\n")]);

  return (
    <div className="mt-3 grid gap-3 md:grid-cols-2">
      {files.map((path) => {
        const kind = guessKind(path);
        if (!kind) return null;
        const directSrc = `${baseUrl}/api/v1/file?path=${encodeURIComponent(path)}&yolo=${yolo ? "1" : "0"}${sidQ}`;
        const blob = blobByPath[path];
        const src = hasAuthHeaders ? (blob?.url || "") : directSrc;
        return (
          <div key={path} className="rounded-lg border border-white/10 bg-black/20 p-3">
            <div className="mb-2 text-xs text-white/60">Preview: {path}</div>
            {hasAuthHeaders && blob?.error ? (
              <div className="mb-2 rounded-md border border-rose-500/30 bg-rose-500/10 px-2 py-1 text-[11px] text-rose-200">
                Load failed: {blob.error}
              </div>
            ) : null}
            {kind === "image" ? (
              src ? <img src={src} className="max-h-72 w-full rounded-md object-contain" /> : null
            ) : kind === "audio" ? (
              src ? (
                <audio controls className="w-full">
                  <source src={src} />
                </audio>
              ) : null
            ) : kind === "video" ? (
              src ? (
                <video controls className="w-full rounded-md">
                  <source src={src} />
                </video>
              ) : null
            ) : null}
            <div className="mt-2 text-[11px] text-white/60">
              {!hasAuthHeaders ? (
                <a className="underline hover:text-white" href={directSrc} target="_blank" rel="noreferrer">
                  Open file
                </a>
              ) : (
                <span title="Direct open cannot include Authorization headers (use blob preview).">Open file (auth required)</span>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
}
