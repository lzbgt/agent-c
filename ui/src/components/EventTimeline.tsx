import React from "react";
import type { AgentEvent } from "../api";
import Markdown from "./Markdown";
import MediaPreviews from "./MediaPreviews";

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function RenderToolContent({
  baseUrl,
  yolo,
  content,
}: {
  baseUrl: string;
  yolo: boolean;
  content: string;
}) {
  const [showRaw, setShowRaw] = React.useState(false);
  const parsed = safeJsonParse(content);
  if (parsed && typeof parsed === "object") {
    const toolName = typeof parsed?.data?.tool === "string" ? parsed.data.tool : "";
    const patch = typeof parsed?.data?.patch === "string" ? parsed.data.patch : null;
    const output = typeof parsed?.data?.output === "string" ? parsed.data.output : null;
    const exitCode =
      typeof parsed?.data?.exit_code === "number"
        ? parsed.data.exit_code
        : typeof parsed?.data?.apply?.exit_code === "number"
          ? parsed.data.apply.exit_code
          : null;
    const ok = typeof parsed?.ok === "boolean" ? parsed.ok : null;
    const error = typeof parsed?.error === "string" ? parsed.error : null;

    return (
      <div>
        <div className="mb-2 flex flex-wrap items-center gap-2 text-xs text-white/70">
          {ok !== null ? (
            <span className={`rounded-md px-2 py-1 ${ok ? "bg-emerald-500/15 text-emerald-200" : "bg-rose-500/15 text-rose-200"}`}>
              ok={String(ok)}
            </span>
          ) : null}
          {exitCode !== null ? <span className="rounded-md bg-white/10 px-2 py-1">exit_code={exitCode}</span> : null}
          {toolName ? <span className="rounded-md bg-white/10 px-2 py-1">{toolName}</span> : null}
          {error ? <span className="rounded-md bg-rose-500/10 px-2 py-1 text-rose-200">{error}</span> : null}
          <button
            className="ml-auto rounded-md border border-white/10 bg-black/30 px-2 py-1 text-xs text-white/70 hover:bg-black/40"
            onClick={() => setShowRaw((v) => !v)}
            type="button"
          >
            {showRaw ? "Hide raw" : "Show raw"}
          </button>
        </div>

        {output ? (
          <div>
            <div className="mb-1 text-xs font-semibold text-white/70">Output</div>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {output}
            </pre>
            <MediaPreviews baseUrl={baseUrl} yolo={yolo} text={output} />
          </div>
        ) : null}

        {typeof patch === "string" ? (
          <div className="mt-3">
            <div className="mb-1 text-xs font-semibold text-white/70">Diff</div>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-indigo-400/20 bg-indigo-500/10 p-3 text-xs leading-relaxed text-indigo-50">
              {patch}
            </pre>
          </div>
        ) : null}

        {showRaw ? (
          <div className="mt-3">
            <div className="mb-1 text-xs font-semibold text-white/70">Raw JSON</div>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(parsed, null, 2)}
            </pre>
          </div>
        ) : null}
      </div>
    );
  }

  return (
    <div>
      <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
        {content}
      </pre>
      <MediaPreviews baseUrl={baseUrl} yolo={yolo} text={content} />
    </div>
  );
}

function EventCard({
  baseUrl,
  yolo,
  ev,
}: {
  baseUrl: string;
  yolo: boolean;
  ev: AgentEvent;
}) {
  const [open, setOpen] = React.useState(true);
  const type = ev.type;
  const data: any = ev.data ?? {};

  const title = (() => {
    if (type === "assistant_message") return "Assistant";
    if (type === "tool_call") return `Tool call: ${data.tool_name ?? ""}`;
    if (type === "tool_result") return `Tool result: ${data.tool_name ?? ""}`;
    if (type === "llm_request") return "LLM request";
    if (type === "llm_response") return "LLM response";
    if (type === "error") return "Error";
    return type;
  })();

  return (
    <div className="rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold">{title}</div>
        <button
          className="text-xs text-white/70 hover:text-white"
          onClick={() => setOpen((v) => !v)}
          type="button"
        >
          {open ? "Collapse" : "Expand"}
        </button>
      </div>
      {open ? (
        <div className="px-3 pb-3">
          {type === "assistant_message" ? (
            <Markdown text={String(data.assistant_content ?? "")} />
          ) : type === "tool_result" ? (
            typeof data.content === "string" ? (
              <RenderToolContent baseUrl={baseUrl} yolo={yolo} content={data.content} />
            ) : (
              <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
                {data.summary ? JSON.stringify(data.summary, null, 2) : "(enable verbose to capture tool output)"}
              </pre>
            )
          ) : type === "tool_call" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {data.arguments_json ? String(data.arguments_json) : "(enable verbose to capture arguments)"}
            </pre>
          ) : type === "llm_request" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {data.request_json ? String(data.request_json) : "(enable verbose to capture request body)"}
            </pre>
          ) : type === "llm_response" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {data.response_body ? String(data.response_body) : "(enable verbose to capture response body)"}
            </pre>
          ) : (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(data, null, 2)}
            </pre>
          )}
        </div>
      ) : null}
    </div>
  );
}

export default function EventTimeline({
  baseUrl,
  yolo,
  events,
}: {
  baseUrl: string;
  yolo: boolean;
  events: AgentEvent[];
}) {
  return (
    <div className="grid gap-3">
      {events.map((ev, idx) => (
        <EventCard key={`${ev.type}-${idx}`} baseUrl={baseUrl} yolo={yolo} ev={ev} />
      ))}
    </div>
  );
}
