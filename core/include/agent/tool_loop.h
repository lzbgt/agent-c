#pragma once

#include "agent/agent.h"
#include "agent/tool_provider.h"
#include "agent/tools.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compaction summary marker name used for the synthetic system message inserted during tool-loop compaction.
// This message is intentionally not treated as "pinned" (so it can be replaced over time).
const char* agent_tool_loop_compaction_summary_name(void);

typedef void (*agent_tool_loop_event_fn)(void* ctx, const char* type, const char* data_json);
typedef uint8_t (*agent_tool_loop_cancel_fn)(void* ctx);

// Optional host hook to cap tool outputs before they enter the prompt (or events).
// - `max_chars` is a character budget (best-effort; host may treat as bytes for UTF-8).
// - `out_truncated` should be set to 1 when truncation occurred.
typedef agent_status_t (*agent_tool_loop_cap_output_fn)(
  void* ctx,
  const char* tool_out,
  size_t tool_out_len,
  size_t max_chars,
  agent_string_t* out_capped,
  uint8_t* out_truncated
);

// Optional host hook to summarize a tool output as a JSON object string (e.g. {"kind":"json_envelope",...}).
// Used when verbose event capture is disabled.
typedef agent_status_t (*agent_tool_loop_summarize_output_fn)(
  void* ctx,
  const char* tool_out,
  size_t tool_out_len,
  agent_string_t* out_summary_json
);

typedef struct agent_tool_loop_hooks {
  agent_tool_loop_event_fn on_event;
  void* on_event_ctx;

  agent_tool_loop_cancel_fn should_cancel;
  void* should_cancel_ctx;

  agent_tool_loop_cap_output_fn cap_tool_output_for_prompt;
  void* cap_tool_output_for_prompt_ctx;

  agent_tool_loop_cap_output_fn cap_tool_output_for_event;
  void* cap_tool_output_for_event_ctx;

  agent_tool_loop_summarize_output_fn summarize_tool_output;
  void* summarize_tool_output_ctx;
} agent_tool_loop_hooks_t;

typedef struct agent_tool_loop_options {
  const char* model; // required
  const char* force_tool_or_null; // optional hint for step 0
  uint8_t require_tool_call; // when true, error if no tool call occurs

  // 0 means unlimited.
  size_t max_steps;

  // Repetition guard: abort if the same tool call (same tool name + arguments) is repeated
  // more than this many times consecutively. This prevents runaway loops (e.g. repeated camera capture).
  // 0 disables the guard.
  size_t max_repeated_tool_calls;

  // Cap total tool calls across the entire run.
  // This complements max_steps because a single step can include multiple tool calls.
  // 0 disables the guard.
  size_t max_tool_calls_total;

  // Cap tool calls per tool name across the entire run.
  // 0 disables the guard.
  size_t max_tool_calls_per_tool;

  // Seamless compaction (char budget heuristic).
  // 0 means default (20000).
  size_t max_chars;
  // 0 means default (16).
  size_t keep_last_messages;

  // Deterministic compaction summary insertion (no extra LLM call).
  uint8_t insert_compaction_summary;
  size_t summary_preview_items;
  size_t summary_snippet_chars;
  size_t summary_max_chars;

  // Caps tool outputs before they enter the prompt context (best-effort).
  // 0 means "no extra capping".
  size_t max_tool_result_chars;

  // Provider context-too-long retries per request (default: 2).
  size_t max_context_too_long_retries;

  // When true, emit verbose tool_result events including capped content; otherwise emit summaries (when available).
  uint8_t verbose_events;
  size_t max_capture_chars; // caps event fields (best-effort). 0 means default (256k).
} agent_tool_loop_options_t;

typedef struct agent_tool_record {
  agent_string_t tool_name;
  agent_string_t tool_call_id;
  agent_string_t arguments_json;
  agent_string_t result_string;
  agent_string_t result_string_for_prompt;
  uint8_t result_truncated_for_prompt;
} agent_tool_record_t;

typedef struct agent_tool_loop_result {
  agent_string_t final_assistant_text;
  uint8_t saw_tool_call;
  size_t steps_executed;

  agent_tool_record_t* tool_records;
  size_t tool_record_count;

  // Best-effort error string for non-OK return values.
  agent_string_t error_message;
} agent_tool_loop_result_t;

void agent_tool_loop_result_free(agent_tool_loop_result_t* r);

// Runs a provider tool loop using a seed session's messages as the initial transcript.
// The seed session is not modified; host apps decide what to persist.
agent_status_t agent_tool_loop_run(
  const agent_tool_provider_t* provider,
  const agent_tool_registry_t* tools,
  const agent_tool_executor_t* executor,
  const agent_session_t* seed_session,
  const char* user_prompt,
  const agent_tool_loop_options_t* options,
  const agent_tool_loop_hooks_t* hooks,
  agent_tool_loop_result_t* out_result
);

#ifdef __cplusplus
}  // extern "C"
#endif
