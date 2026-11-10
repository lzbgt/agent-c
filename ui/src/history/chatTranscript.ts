import type { AgentEvent } from "../api";
import { getNormalizedEventRecord } from "../components/conversation/conversationData";
import type { DbMessageRow, HistoryEntry } from "./historyPanelData";

export type ChatMessage = {
  key: string;
  role: "user" | "assistant";
  content: string;
  multimodal?: Record<string, unknown>;
  truncated?: boolean;
};

export function latestChatFailure(entries: HistoryEntry[]): string | undefined {
  const latest = entries.filter((entry) => !entry.live).slice().sort((a, b) => b.ts_unix_ms - a.ts_unix_ms)[0];
  if (latest?.ok !== false) return undefined;
  const error = latest.events.slice().reverse().find((event) => event.type === "error");
  const data = error ? getNormalizedEventRecord(error.data) : {};
  const message = [data.error, data.message, data.reason].find((value) => typeof value === "string" && value.trim());
  return typeof message === "string" ? message.slice(0, 180) : "The agent could not complete this request. See Activity & details.";
}

function assistantMessages(events: AgentEvent[], key: string): ChatMessage[] {
  const messages: ChatMessage[] = [];
  let delta = "";
  for (const [index, event] of events.entries()) {
    const data = getNormalizedEventRecord(event.data);
    if (event.type === "assistant_delta" && typeof data.delta === "string") delta += data.delta;
    if (event.type === "assistant_message") {
      const content = typeof data.assistant_content === "string" ? data.assistant_content : "";
      if (content || data.assistant_mm_json) messages.push({ key: `${key}:${index}`, role: "assistant", content, multimodal: data });
      delta = "";
    }
  }
  if (delta) messages.push({ key: `${key}:stream`, role: "assistant", content: delta });
  return messages;
}

function entryMessages(entry: HistoryEntry, key: string): ChatMessage[] {
  const messages: ChatMessage[] = [];
  const prompt = entry.prompt || "";
  if (prompt && prompt !== "Live session event stream") messages.push({ key: `${key}:user`, role: "user", content: prompt });
  messages.push(...assistantMessages(entry.events || [], key));
  if (entry.assistant_text && (messages[messages.length - 1]?.role !== "assistant" || messages[messages.length - 1]?.content !== entry.assistant_text)) {
    messages.push({ key: `${key}:final`, role: "assistant", content: entry.assistant_text });
  }
  return messages;
}

export function buildChatTranscript(rows: DbMessageRow[], entries: HistoryEntry[]): ChatMessage[] {
  const settled = entries.filter((entry) => !entry.live).slice().sort((a, b) => a.ts_unix_ms - b.ts_unix_ms);
  const snapshotTime = Math.max(0, ...rows.map((row) => row.created_unix_ms || 0));
  const auditTime = settled[settled.length - 1]?.ts_unix_ms || 0;
  // The native DB mirrors model context with the run timestamp; audit entries
  // preserve the user's turns even when that context has been compacted. A
  // newer DB snapshot takes precedence while the audit query catches up.
  const messages = settled.length && auditTime >= snapshotTime
    ? settled.flatMap((entry, index) => entryMessages(entry, entry.trace_id || `turn:${entry.ts_unix_ms}:${index}`))
    : rows.flatMap((row, index): ChatMessage[] => {
        if (row.role !== "user" && row.role !== "assistant") return [];
        if (!row.content && !row.mm_json) return [];
        return [{ key: `message:${index}`, role: row.role, content: row.content || "", truncated: row.content_truncated,
          multimodal: row.mm_json ? { assistant_mm_json: row.mm_json, assistant_mm_bytes: row.mm_bytes } : undefined }];
      });
  const traceOf = (entry: HistoryEntry) => entry.trace_id || entry.events.find((event) => event.trace_id)?.trace_id || entry.events.map((event) => getNormalizedEventRecord(event.data).trace_id).find((value) => typeof value === "string");
  const settledTraces = new Set(settled.map(traceOf).filter(Boolean));
  for (const [index, entry] of entries.entries()) {
    if (!entry.live || (traceOf(entry) && settledTraces.has(traceOf(entry)))) continue;
    messages.push(...entryMessages(entry, entry.job_id || `live:${index}`));
  }
  return messages;
}
