import React from "react";
import { safeJsonParse, safeObject, safeTrunc } from "../jsonUtils";

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

export function parseAssistantMultimodal(data: unknown): AssistantMMParsed | null {
  const dataRecord = safeObject(data);
  const raw = typeof dataRecord.assistant_mm_json === "string" ? dataRecord.assistant_mm_json : "";
  if (!raw) return null;
  const bytes = typeof dataRecord.assistant_mm_bytes === "number" ? dataRecord.assistant_mm_bytes : raw.length;
  const truncated = dataRecord.assistant_mm_truncated === 1 || dataRecord.assistant_mm_truncated === true;
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
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
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
  const parsedRecord = safeObject(parsed);

  const images = Array.isArray(parsedRecord.images)
    ? parsedRecord.images
        .map((imageValue) => safeObject(imageValue))
        .filter((image) => typeof image.b64 === "string" && image.b64.length > 0)
        .map((image) => ({
          name: typeof image.name === "string" ? image.name : "",
          mime: typeof image.mime === "string" ? image.mime : "image/png",
          b64: image.b64 as string,
        }))
    : [];

  const files = Array.isArray(parsedRecord.files)
    ? parsedRecord.files
        .map((fileValue) => safeObject(fileValue))
        .filter((file) => typeof file.text === "string" && file.text.length > 0)
        .map((file) => ({
          name: typeof file.name === "string" ? file.name : "",
          mime: typeof file.mime === "string" ? file.mime : "",
          text: file.text as string,
          truncated: file.truncated === true,
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

  mm.files.forEach((file, index) => {
    const label = file.name || "file";
    const subtitle = file.mime ? ` (${file.mime})` : "";
    const text = file.text.length > ASSISTANT_MM_MAX_FILE_CHARS ? safeTrunc(file.text, ASSISTANT_MM_MAX_FILE_CHARS) : file.text;
    const wasTruncated = file.truncated || file.text.length > ASSISTANT_MM_MAX_FILE_CHARS;
    blocks.push(
      <div key={`mm-file-${index}`} className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
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

  mm.images.forEach((image, index) => {
    const label = image.name || "image";
    const subtitle = image.mime ? ` (${image.mime})` : "";
    if (image.b64.length > ASSISTANT_MM_MAX_IMAGE_B64) {
      blocks.push(
        <div key={`mm-img-${index}`} className="mt-2 rounded-md border border-white/10 bg-black/20 p-2 text-xs text-white/70">
          Image omitted (too large): {label}
          {subtitle} ({image.b64.length} bytes)
        </div>,
      );
      return;
    }
    const url = `data:${image.mime || "image/png"};base64,${image.b64}`;
    blocks.push(
      <div key={`mm-img-${index}`} className="mt-2 rounded-md border border-white/10 bg-black/20 p-2">
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
