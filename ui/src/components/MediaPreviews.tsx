import React from "react";

function uniq<T>(arr: T[]): T[] {
  return Array.from(new Set(arr));
}

function guessKind(path: string): "image" | "audio" | "video" | null {
  const lower = path.toLowerCase();
  if (/\.(png|jpe?g|gif|webp|svg)$/.test(lower)) return "image";
  if (/\.(mp3|wav)$/.test(lower)) return "audio";
  if (/\.(mp4|webm|mov)$/.test(lower)) return "video";
  return null;
}

export default function MediaPreviews({
  baseUrl,
  yolo,
  text,
}: {
  baseUrl: string;
  yolo: boolean;
  text: string;
}) {
  // Best-effort: extract file-looking tokens from text output.
  const matches = Array.from(
    text.matchAll(/(^|[\s'"])([^\s'"]+\.(?:png|jpe?g|gif|webp|svg|mp3|wav|mp4|webm|mov))([\s'"]|$)/gim),
  ).map((m) => m[2]);

  const files = uniq(matches)
    .map((p) => p.trim())
    .filter((p) => p.length > 0)
    .slice(0, 8);

  if (files.length === 0) return null;

  return (
    <div className="mt-3 grid gap-3 md:grid-cols-2">
      {files.map((path) => {
        const kind = guessKind(path);
        if (!kind) return null;
        const src = `${baseUrl}/api/v1/file?path=${encodeURIComponent(path)}&yolo=${yolo ? "1" : "0"}`;
        return (
          <div key={path} className="rounded-lg border border-white/10 bg-black/20 p-3">
            <div className="mb-2 text-xs text-white/60">Preview: {path}</div>
            {kind === "image" ? (
              <img src={src} className="max-h-72 w-full rounded-md object-contain" />
            ) : kind === "audio" ? (
              <audio controls className="w-full">
                <source src={src} />
              </audio>
            ) : kind === "video" ? (
              <video controls className="w-full rounded-md">
                <source src={src} />
              </video>
            ) : null}
          </div>
        );
      })}
    </div>
  );
}

