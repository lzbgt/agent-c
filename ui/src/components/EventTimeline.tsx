import React from "react";
import type { AgentEvent } from "../api";
import { normalizeEventData, prettyJsonOrRaw, safeObject } from "../jsonUtils";
import Markdown from "./Markdown";
import LlmDebugView from "./LlmDebugView";
import ToolResultView from "./ToolResultView";

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
  const data = normalizeEventData(ev.data);
  const dataRecord = safeObject(data);

  const title = (() => {
    if (type === "user_message") return "User";
    if (type === "assistant_message") return "Assistant";
    if (type === "assistant_delta") return "Assistant delta";
    if (type === "tool_call") return `Tool call: ${typeof dataRecord.tool_name === "string" ? dataRecord.tool_name : ""}`;
    if (type === "tool_result") return `Tool result: ${typeof dataRecord.tool_name === "string" ? dataRecord.tool_name : ""}`;
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
          {type === "user_message" ? (
            <>
              <Markdown text={typeof dataRecord.user_content === "string" ? dataRecord.user_content : ""} />
              {typeof dataRecord.user_mm_json === "string" && dataRecord.user_mm_json.length > 0 ? (
                <pre className="mt-3 overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
                  {prettyJsonOrRaw(dataRecord.user_mm_json)}
                </pre>
              ) : null}
            </>
          ) : type === "assistant_message" ? (
            <Markdown text={typeof dataRecord.assistant_content === "string" ? dataRecord.assistant_content : ""} />
          ) : type === "assistant_delta" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof dataRecord.delta === "string" ? dataRecord.delta : JSON.stringify(data, null, 2)}
            </pre>
          ) : type === "tool_result" ? (
            typeof dataRecord.content === "string" ? (
              <ToolResultView baseUrl={baseUrl} yolo={yolo} content={dataRecord.content} />
            ) : (
              <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
                {dataRecord.summary !== undefined ? JSON.stringify(dataRecord.summary, null, 2) : "(enable verbose to capture tool output)"}
              </pre>
            )
          ) : type === "tool_call" ? (
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof dataRecord.arguments_json === "string"
                ? prettyJsonOrRaw(dataRecord.arguments_json)
                : "(enable verbose to capture arguments)"}
            </pre>
          ) : type === "llm_request" ? (
            <LlmDebugView kind="request" data={data} />
          ) : type === "llm_response" ? (
            <LlmDebugView kind="response" data={data} />
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
