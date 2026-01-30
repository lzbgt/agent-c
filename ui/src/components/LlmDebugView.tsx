import React from "react";

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function safeTrunc(s: string, max: number): string {
  const v = String(s ?? "");
  if (v.length <= max) return v;
  return v.slice(0, Math.max(0, max - 1)) + "…";
}

function renderMaybePrettyJson(s: string): { ok: true; pretty: string; parsed: any } | { ok: false; raw: string } {
  const parsed = safeJsonParse(s);
  if (!parsed) return { ok: false, raw: s };
  return { ok: true, pretty: JSON.stringify(parsed, null, 2), parsed };
}

function renderMessagePreview(content: any): string {
  if (typeof content === "string") return safeTrunc(content, 300);
  if (Array.isArray(content)) {
    // Multimodal content blocks; keep it readable.
    const parts = content.slice(0, 10).map((p) => {
      if (!p || typeof p !== "object") return safeTrunc(String(p ?? ""), 80);
      const t = typeof p.type === "string" ? p.type : "part";
      if (t === "text" && typeof p.text === "string") return safeTrunc(p.text, 120);
      if (t === "image_url") return safeTrunc(String(p.image_url?.url ?? "image_url"), 120);
      return safeTrunc(JSON.stringify(p), 160);
    });
    return safeTrunc(parts.join(" | "), 300);
  }
  if (content && typeof content === "object") return safeTrunc(JSON.stringify(content), 300);
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

export default function LlmDebugView({
  kind,
  data,
}: {
  kind: "request" | "response";
  data: any;
}) {
  const attempt = typeof data?.attempt === "number" ? data.attempt : null;
  const stream = typeof data?.stream === "boolean" ? data.stream : null;
  const httpStatus = typeof data?.http_status === "number" ? data.http_status : null;
  const truncated =
    kind === "request"
      ? typeof data?.request_truncated === "boolean"
        ? data.request_truncated
        : null
      : typeof data?.response_truncated === "boolean"
        ? data.response_truncated
        : null;

  const raw =
    kind === "request"
      ? typeof data?.request_json === "string"
        ? data.request_json
        : null
      : typeof data?.response_body === "string"
        ? data.response_body
        : null;

  const pretty = raw ? renderMaybePrettyJson(raw) : null;
  const parsed = pretty && pretty.ok ? pretty.parsed : null;

  const model = kind === "request" && parsed && typeof parsed.model === "string" ? parsed.model : null;
  const toolsCount =
    kind === "request" && parsed && Array.isArray(parsed.tools) ? parsed.tools.length : kind === "request" && parsed && Array.isArray(parsed.functions) ? parsed.functions.length : null;
  const messages = kind === "request" && parsed && Array.isArray(parsed.messages) ? parsed.messages : null;

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
                {messages.slice(0, 200).map((m: any, idx: number) => {
                  const role = typeof m?.role === "string" ? m.role : "";
                  const content = m?.content;
                  const preview = renderMessagePreview(content);
                  const len = typeof content === "string" ? content.length : JSON.stringify(content ?? "").length;
                  return (
                    <tr key={idx} className="border-t border-white/5">
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

