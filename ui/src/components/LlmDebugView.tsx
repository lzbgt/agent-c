import React from "react";
import { isUnknownRecord, safeJsonParse, safeObject, safeTrunc, type UnknownRecord } from "../jsonUtils";

function renderMaybePrettyJson(raw: string): { ok: true; pretty: string; parsed: unknown } | { ok: false; raw: string } {
  const parsed = safeJsonParse(raw);
  if (parsed === null) return { ok: false, raw };
  return { ok: true, pretty: JSON.stringify(parsed, null, 2), parsed };
}

function renderMessagePreview(content: unknown): string {
  if (typeof content === "string") return safeTrunc(content, 300);
  if (Array.isArray(content)) {
    const parts = content.slice(0, 10).map((partValue) => {
      const part = safeObject(partValue);
      if (Object.keys(part).length === 0) return safeTrunc(String(partValue ?? ""), 80);
      const kind = typeof part.type === "string" ? part.type : "part";
      if (kind === "text" && typeof part.text === "string") return safeTrunc(part.text, 120);
      if (kind === "image_url") {
        const image = safeObject(part.image_url);
        return safeTrunc(typeof image.url === "string" ? image.url : "image_url", 120);
      }
      return safeTrunc(JSON.stringify(part), 160);
    });
    return safeTrunc(parts.join(" | "), 300);
  }
  if (isUnknownRecord(content)) return safeTrunc(JSON.stringify(content), 300);
  return safeTrunc(String(content ?? ""), 300);
}

function chip(text: string, tone: "neutral" | "good" | "warn" | "bad" = "neutral") {
  const cls =
    tone === "good"
      ? "bg-emerald-500/15 text-emerald-200"
      : tone === "warn"
        ? "bg-amber-500/15 text-amber-200"
        : tone === "bad"
          ? "bg-rose-500/15 text-rose-200"
          : "bg-white/10 text-white/80";
  return <span className={`rounded-md px-2 py-1 text-[11px] ${cls}`}>{text}</span>;
}

function getMessageRows(parsed: unknown): UnknownRecord[] | null {
  if (!isUnknownRecord(parsed) || !Array.isArray(parsed.messages)) return null;
  return parsed.messages.map((message) => safeObject(message));
}

export default function LlmDebugView({
  kind,
  data,
}: {
  kind: "request" | "response";
  data: unknown;
}) {
  const dataRecord = safeObject(data);
  const attempt = typeof dataRecord.attempt === "number" ? dataRecord.attempt : null;
  const stream = typeof dataRecord.stream === "boolean" ? dataRecord.stream : null;
  const httpStatus = typeof dataRecord.http_status === "number" ? dataRecord.http_status : null;
  const truncated =
    kind === "request"
      ? typeof dataRecord.request_truncated === "boolean"
        ? dataRecord.request_truncated
        : null
      : typeof dataRecord.response_truncated === "boolean"
        ? dataRecord.response_truncated
        : null;

  const raw =
    kind === "request"
      ? typeof dataRecord.request_json === "string"
        ? dataRecord.request_json
        : null
      : typeof dataRecord.response_body === "string"
        ? dataRecord.response_body
        : null;

  const pretty = raw ? renderMaybePrettyJson(raw) : null;
  const parsed = pretty && pretty.ok ? pretty.parsed : null;
  const parsedRecord = safeObject(parsed);

  const model = kind === "request" && typeof parsedRecord.model === "string" ? parsedRecord.model : null;
  const toolsCount =
    kind === "request" && Array.isArray(parsedRecord.tools)
      ? parsedRecord.tools.length
      : kind === "request" && Array.isArray(parsedRecord.functions)
        ? parsedRecord.functions.length
        : null;
  const messages = kind === "request" ? getMessageRows(parsed) : null;

  return (
    <div>
      <div className="mb-2 flex flex-wrap items-center gap-2">
        {attempt !== null ? chip(`attempt=${attempt}`) : null}
        {stream !== null ? chip(`stream=${String(stream)}`) : null}
        {httpStatus !== null ? chip(`http_status=${httpStatus}`, httpStatus >= 200 && httpStatus < 300 ? "good" : "bad") : null}
        {model ? chip(`model=${model}`) : null}
        {toolsCount !== null ? chip(`tools=${toolsCount}`) : null}
        {messages ? chip(`messages=${messages.length}`) : null}
        {truncated === true ? chip("truncated=true", "warn") : truncated === false ? chip("truncated=false") : null}
      </div>

      {!raw ? (
        <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
          (enable verbose to capture {kind === "request" ? "request body" : "response body"})
        </pre>
      ) : null}

      {raw && messages ? (
        <div className="rounded-md border border-white/10 bg-black/20">
          <div className="border-b border-white/10 px-3 py-2 text-xs font-semibold text-white/70">Messages</div>
          <div className="max-h-[360px] overflow-auto">
            <table className="w-full text-left text-[11px] text-white/80">
              <thead className="sticky top-0 bg-black/40 text-white/60">
                <tr>
                  <th className="px-3 py-2">Role</th>
                  <th className="px-3 py-2">Len</th>
                  <th className="px-3 py-2">Preview</th>
                </tr>
              </thead>
              <tbody>
                {messages.slice(0, 200).map((message, index) => {
                  const role = typeof message.role === "string" ? message.role : "";
                  const content = message.content;
                  const preview = renderMessagePreview(content);
                  const len = typeof content === "string" ? content.length : JSON.stringify(content ?? "").length;
                  return (
                    <tr key={index} className="border-t border-white/5">
                      <td className="px-3 py-2 font-mono text-white/70">{role}</td>
                      <td className="px-3 py-2 font-mono text-white/60">{len}</td>
                      <td className="px-3 py-2 whitespace-pre-wrap">{preview}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        </div>
      ) : null}

      {raw ? (
        <details className="mt-3 rounded-md border border-white/10 bg-black/20 p-3">
          <summary className="cursor-pointer text-xs font-semibold text-white/70">Full {kind === "request" ? "request" : "response"} JSON</summary>
          <pre className="mt-2 max-h-[520px] overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {pretty
              ? pretty.ok
                ? pretty.pretty
                : pretty.raw
              : raw}
          </pre>
        </details>
      ) : null}
    </div>
  );
}
