#pragma once

#include "openai_client.h"
#include "agent/tools.h"
#include "agent/agent.h"

#include <cstddef>
#include <string>
#include <vector>
#include <iosfwd>

// Optional callback for streaming tool-loop events as they occur.
// `data_json` is a JSON object string for the event's `data` field.
using ToolLoopEventCallback = void (*)(void* ctx, const char* type, const char* data_json);
using ToolLoopCancelCallback = bool (*)(void* ctx);

struct ToolLoopOptions {
  std::string force_tool;    // optional tool name to force on first request
  bool require_tool_call = false;
  // When true, request OpenAI-compatible SSE streaming (`stream: true`) and emit `assistant_delta` events.
  // Applies to `tools=basic|host` tool loops (provider-dependent).
  bool stream_assistant = false;
  // Max number of tool-loop steps.
  // 0 means "unlimited" (run until the model stops producing tool calls).
  size_t max_steps = 0;
  // When true, captures full request/response bodies and tool I/O into `events_json`.
  // When false, captures a lightweight event log without large blobs.
  bool verbose = false;
  // Max bytes captured per field in events (best-effort).
  size_t max_capture_bytes = 256 * 1024;

  // Caps tool outputs before they are appended into the LLM request context.
  // 0 means "no extra capping" (not recommended for host tools).
  size_t max_tool_result_chars = 12000;

  // Seamless compaction for tool loops (portable char-budget heuristic).
  // 0 means "use default" (20000).
  size_t max_chars = 0;
  // 0 means "use default" (16).
  size_t keep_last_messages = 0;

  // When true, inserts a short system summary describing what was dropped.
  // This is a lightweight, deterministic summary (no extra LLM call).
  bool insert_compaction_summary = true;
  size_t summary_preview_items = 3;
  size_t summary_snippet_chars = 160;
  size_t summary_max_chars = 600;

  // Optional live event hook. When set, run_tool_loop will invoke this callback for every event.
  // This is used by the daemon/UI to show progress while long requests are running.
  ToolLoopEventCallback on_event = nullptr;
  void* on_event_ctx = nullptr;

  // Optional cooperative cancellation hook.
  // When set and it returns true, the tool loop aborts at the next safe boundary (between requests/tool calls).
  ToolLoopCancelCallback should_cancel = nullptr;
  void* should_cancel_ctx = nullptr;
};

// Tool call record (captured during the tool loop) that can be persisted into host sessions
// in a portable plain-text form.
struct ToolLoopToolRecord {
  std::string tool_name;
  std::string tool_call_id;     // OpenAI-style call id when present (may be empty)
  std::string arguments_json;   // OpenAI tool arguments string (JSON)
  std::string result_string;            // executor output (often JSON envelope string)
  std::string result_string_for_prompt; // capped version suitable for putting into LLM context
  bool result_truncated_for_prompt = false;
};

struct ToolLoopResult {
  std::string final_assistant_text;
  // Structured event log as a JSON array string (best-effort).
  // This is intended for UIs; do not assume a fixed schema beyond `type`.
  std::string events_json;
  bool saw_tool_call = false;
  // Tool transcript captured during execution (best-effort).
  std::vector<ToolLoopToolRecord> tool_records;
};

// Runs an OpenAI-compatible tool-call loop using the host's HTTP client (libcurl).
// On success, returns true and fills out_result.
// On failure, returns false and fills out_error (best effort).
bool run_tool_loop(
  const OpenAIClientConfig& cfg,
  const struct agent_session* seed_session,
  const std::string& user_prompt,
  const struct agent_tool_registry* tools,
  const struct agent_tool_executor* executor,
  const ToolLoopOptions& options,
  std::ostream* trace_stream,
  ToolLoopResult* out_result,
  std::string* out_error,
  long* out_http_status,
  std::string* out_http_body
);
