import React from "react";
import type { AgentEvent } from "../api";
import Markdown from "./Markdown";
import ToolResultView from "./ToolResultView";

function safeJsonParse(s: string): any | null {
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

function normalizeEventData(data: unknown): any {
  if (typeof data === "string") {
    return safeJsonParse(data) ?? data;
  }
  return data ?? {};
}

function prettyJsonOrRaw(s: string) {
  const parsed = safeJsonParse(s);
  if (!parsed) return s;
  return JSON.stringify(parsed, null, 2);
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
  const data: any = normalizeEventData(ev.data);

  const title = (() => {
    if (type === "assistant_message") return "Assistant";
    if (type === "assistant_delta") return "Assistant delta";
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
          ) : type === "assistant_delta" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof data.delta === "string" ? data.delta : JSON.stringify(data, null, 2)}
            </pre>
          ) : type === "tool_result" ? (
            typeof data.content === "string" ? (
              <ToolResultView baseUrl={baseUrl} yolo={yolo} content={data.content} />
            ) : (
              <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
                {data.summary ? JSON.stringify(data.summary, null, 2) : "(enable verbose to capture tool output)"}
              </pre>
            )
          ) : type === "tool_call" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof data.arguments_json === "string"
                ? prettyJsonOrRaw(data.arguments_json)
                : "(enable verbose to capture arguments)"}
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
              {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
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
