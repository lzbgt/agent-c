import React from "react";

const ASSISTANT_MM_MAX_JSON_CHARS = 512 * 1024;
const ASSISTANT_MM_MAX_FILE_CHARS = 8000;
const ASSISTANT_MM_MAX_IMAGE_B64 = 512 * 1024;

type AssistantMMImage = {
  name: string;
  mime: string;
  b64: string;
};

type AssistantMMFile = {
  name: string;
  mime: string;
  text: string;
  truncated: boolean;
};

export type AssistantMMParsed = {
  raw: string;
  bytes: number;
  truncated: boolean;
  tooLarge: boolean;
  parseError: boolean;
  images: AssistantMMImage[];
  files: AssistantMMFile[];
};

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function safeTrunc(s: string, max: number): string {
  if (s.length <= max) return s;
  return s.slice(0, Math.max(0, max - 1)) + "…";
}

export function parseAssistantMultimodal(data: any): AssistantMMParsed | null {
  const raw = typeof data?.assistant_mm_json === "string" ? data.assistant_mm_json : "";
  if (!raw) return null;
  const bytes = typeof data?.assistant_mm_bytes === "number" ? data.assistant_mm_bytes : raw.length;
  const truncated = data?.assistant_mm_truncated === 1 || data?.assistant_mm_truncated === true;
  if (raw.length > ASSISTANT_MM_MAX_JSON_CHARS) {
    return {
      raw,
      bytes,
      truncated,
      tooLarge: true,
      parseError: false,
      images: [],
      files: [],
    };
  }
  const parsed = safeJsonParse(raw);
  if (!parsed || typeof parsed !== "object") {
    return {
      raw,
      bytes,
      truncated,
      tooLarge: false,
      parseError: true,
      images: [],
      files: [],
    };
  }
  const images: AssistantMMImage[] = Array.isArray((parsed as any).images)
    ? (parsed as any).images
        .filter((im: any) => im && typeof im === "object" && typeof im.b64 === "string" && im.b64.length > 0)
        .map((im: any) => ({
          name: typeof im.name === "string" ? im.name : "",
          mime: typeof im.mime === "string" ? im.mime : "image/png",
          b64: String(im.b64 ?? ""),
        }))
    : [];
  const files: AssistantMMFile[] = Array.isArray((parsed as any).files)
    ? (parsed as any).files
        .filter((f: any) => f && typeof f === "object" && typeof f.text === "string" && f.text.length > 0)
        .map((f: any) => ({
          name: typeof f.name === "string" ? f.name : "",
          mime: typeof f.mime === "string" ? f.mime : "",
          text: String(f.text ?? ""),
          truncated: f.truncated === true,
        }))
    : [];
  return {
    raw,
    bytes,
    truncated,
    tooLarge: false,
    parseError: false,
    images,
    files,
  };
}

export function renderAssistantMultimodal(mm: AssistantMMParsed | null) {
  if (!mm) return null;
  if (mm.tooLarge) {
    return (
      <div className="mt-2 rounded-md border border-yellow-400/30 bg-yellow-500/10 px-2 py-1 text-xs text-yellow-100">
        Multimodal payload too large to render ({mm.bytes} bytes).
      </div>
    );
  }
  if (mm.parseError) {
    return (
      <div className="mt-2 rounded-md border border-red-400/30 bg-red-500/10 px-2 py-1 text-xs text-red-100">
        Failed to parse multimodal payload.
      </div>
    );
  }

  const blocks: React.ReactNode[] = [];

  mm.files.forEach((f, idx) => {
    const label = f.name || "file";
    const subtitle = f.mime ? ` (${f.mime})` : "";
    const text = f.text.length > ASSISTANT_MM_MAX_FILE_CHARS ? safeTrunc(f.text, ASSISTANT_MM_MAX_FILE_CHARS) : f.text;
    const wasTruncated = f.truncated || f.text.length > ASSISTANT_MM_MAX_FILE_CHARS;
    blocks.push(
      <div key={`mm-file-${idx}`} className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
        <div className="text-xs font-semibold text-white/80">
          Attachment: {label}
          {subtitle}
        </div>
        <pre className="mt-1 overflow-auto whitespace-pre-wrap break-words text-[11px] leading-relaxed text-white/80">
          {text}
        </pre>
        {wasTruncated ? <div className="mt-1 text-[10px] text-white/50">Attachment text truncated.</div> : null}
      </div>,
    );
  });

  mm.images.forEach((im, idx) => {
    const label = im.name || "image";
    const subtitle = im.mime ? ` (${im.mime})` : "";
    if (im.b64.length > ASSISTANT_MM_MAX_IMAGE_B64) {
      blocks.push(
        <div key={`mm-img-${idx}`} className="mt-2 rounded-md border border-white/10 bg-black/20 p-2 text-xs text-white/70">
          Image omitted (too large): {label}
          {subtitle} ({im.b64.length} bytes)
        </div>,
      );
      return;
    }
    const url = `data:${im.mime || "image/png"};base64,${im.b64}`;
    blocks.push(
      <div key={`mm-img-${idx}`} className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
        <div className="text-xs font-semibold text-white/80">
          Image: {label}
          {subtitle}
        </div>
        <img className="mt-2 max-h-80 w-auto rounded border border-white/10" src={url} alt={label} />
      </div>,
    );
  });

  if (blocks.length === 0) {
    blocks.push(
      <div key="mm-empty" className="mt-2 rounded-md border border-white/10 bg-black/20 px-2 py-1 text-xs text-white/70">
        Multimodal payload present but empty.
      </div>,
    );
  }

  if (mm.truncated) {
    blocks.push(
      <div key="mm-trunc" className="mt-2 text-[10px] text-white/50">
        Multimodal payload truncated; some parts may be omitted.
      </div>,
    );
  }

  return <div className="mt-2">{blocks}</div>;
}
