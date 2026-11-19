import type { AgentEvent } from "../api";
import { getNormalizedEventRecord } from "../components/conversation/conversationData";

const chatFields = new Set(["prompt", "messages", "assistant_text", "assistant_content", "assistant_mm_json"]);

// Project display metadata without producing another conversation from a raw
// request/response envelope. Execution still receives the original payloads.
export function executionMetadata(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(executionMetadata);
  if (!value || typeof value !== "object") return value;
  const record = value as Record<string, unknown>;
  const isChatMessage = record.role === "user" || record.role === "assistant";
  return Object.fromEntries(Object.entries(record)
    .filter(([key]) => !chatFields.has(key) && !(isChatMessage && (key === "content" || key === "mm_json")))
    .map(([key, item]) => [key, executionMetadata(item)]));
}

export function executionEvent(event: AgentEvent): AgentEvent | null {
  if (["assistant_message", "assistant_delta", "user_message", "llm_request", "llm_response"].includes(event.type)) return null;
  return event.type === "start" || event.type === "end" || event.type === "done"
    ? { ...event, data: executionMetadata(getNormalizedEventRecord(event.data)) }
    : event;
}

export function executionEvents(events: AgentEvent[]): AgentEvent[] {
  return events.map(executionEvent).filter((event): event is AgentEvent => event !== null);
}

// Transport status or a summary is not evidence that the original tool
// response is present. Honor the native 0/1 flag and boolean transport forms.
export function toolEvidenceNote(data: Record<string, unknown>): string | null {
  if (typeof data.content !== "string") {
    return data.summary === undefined || data.summary === null
      ? "Unverified: this event does not include the original tool output."
      : "Unverified: this event contains a summary, not the original tool output.";
  }
  const flag = data.content_truncated;
  if (flag === 0 || flag === false) return null;
  return flag === 1 || flag === true
    ? "Unverified: this event contains only a truncated copy of the tool output."
    : "Unverified: this event does not record whether the tool output is complete.";
}

// Make retained text copyable, without inventing an outcome or converting
// metadata alone into a synthetic tool result.
export function toolSummaryOutput(summary: Record<string, unknown>, toolName: string): string | null {
  if (typeof summary.output !== "string") return null;
  return JSON.stringify({
    ...(typeof summary.ok === "boolean" ? { ok: summary.ok } : {}),
    data: { ...summary, tool: toolName },
  });
}
