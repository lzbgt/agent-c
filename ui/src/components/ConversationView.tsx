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

function Card({
  title,
  children,
}: {
  title: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <div className="rounded-lg border border-white/10 bg-white/5">
      <div className="flex items-center justify-between px-3 py-2">
        <div className="text-sm font-semibold">{title}</div>
      </div>
      <div className="px-3 pb-3">{children}</div>
    </div>
  );
}

function prettyJsonOrRaw(s: string) {
  const parsed = safeJsonParse(s);
  if (!parsed) return s;
  return JSON.stringify(parsed, null, 2);
}

export default function ConversationView({
  baseUrl,
  yolo,
  prompt,
  events,
  showDebugEvents,
}: {
  baseUrl: string;
  yolo: boolean;
  prompt: string;
  events: AgentEvent[];
  showDebugEvents?: boolean;
}) {
  const items: Array<React.ReactNode> = [];
  let streamedAssistant = "";
  let sawFinalAssistant = false;

  if (prompt.trim().length > 0) {
    items.push(
      <Card key="user" title="User">
        <Markdown text={prompt} />
      </Card>,
    );
  }

  events.forEach((ev, idx) => {
    const type = ev.type;
    const data: any = normalizeEventData(ev.data);

    if (type === "assistant_message") {
      sawFinalAssistant = true;
      items.push(
        <Card key={`a-${idx}`} title="Assistant">
          <Markdown text={String(data.assistant_content ?? "")} />
        </Card>,
      );
      return;
    }

    if (type === "assistant_delta") {
      const delta = typeof data.delta === "string" ? data.delta : "";
      if (delta) streamedAssistant += delta;
      return;
    }

    if (type === "tool_call") {
      const name = String(data.tool_name ?? "");
      const args = typeof data.arguments_json === "string" ? data.arguments_json : "";
      items.push(
        <Card key={`tc-${idx}`} title={`Tool call: ${name || "(unknown)"}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {args ? prettyJsonOrRaw(args) : "(enable verbose to capture arguments)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (type === "tool_result") {
      const name = String(data.tool_name ?? "");
      if (typeof data.content === "string") {
        items.push(
          <Card key={`tr-${idx}`} title={`Tool result: ${name || "(unknown)"}`}>
            <ToolResultView baseUrl={baseUrl} yolo={yolo} content={data.content} />
          </Card>,
        );
        return;
      }
      items.push(
        <Card key={`tr-${idx}`} title={`Tool result: ${name || "(unknown)"}`}>
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
            {data.summary ? JSON.stringify(data.summary, null, 2) : "(enable verbose to capture tool output)"}
          </pre>
        </Card>,
      );
      return;
    }

    if (showDebugEvents) {
      if (type === "retry") {
        items.push(
          <Card key={`rt-${idx}`} title="Retry">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs leading-relaxed text-amber-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "cancel_requested") {
        items.push(
          <Card key={`cr-${idx}`} title="Cancel requested">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-amber-500/30 bg-amber-500/10 p-3 text-xs leading-relaxed text-amber-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "cancelled") {
        items.push(
          <Card key={`cx-${idx}`} title="Cancelled">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "compaction") {
        items.push(
          <Card key={`cp-${idx}`} title="Compaction">
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
      if (type === "llm_request" || type === "llm_response" || type === "start" || type === "end" || type === "done") {
        items.push(
          <Card key={`dbg-${type}-${idx}`} title={`Debug: ${type}`}>
            <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-white/10 bg-black/30 p-3 text-xs leading-relaxed text-white/90">
              {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
            </pre>
          </Card>,
        );
        return;
      }
    }

    // Hide low-level transport/debug events by default; those remain visible in the “Events” timeline.
    if (type === "llm_request" || type === "llm_response") return;
    if (type === "start" || type === "end" || type === "done" || type === "retry" || type === "compaction") return;
    if (type === "error") {
      items.push(
        <Card key={`e-${idx}`} title="Error">
          <pre className="overflow-auto whitespace-pre-wrap rounded-md border border-rose-500/30 bg-rose-500/10 p-3 text-xs leading-relaxed text-rose-100">
            {typeof data === "string" ? data : JSON.stringify(data, null, 2)}
          </pre>
        </Card>,
      );
    }
  });

  if (!sawFinalAssistant && streamedAssistant.trim().length > 0) {
    items.push(
      <Card key="assistant-stream" title="Assistant (streaming)">
        <Markdown text={streamedAssistant} />
      </Card>,
    );
  }

  return <div className="grid gap-3">{items}</div>;
}
