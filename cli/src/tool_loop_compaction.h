#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

// Helper utilities for tool-loop message compaction and "session rotation".
// This is host-only code: it operates on OpenAI-compatible JSON message arrays.
//
// Design goals:
// - Deterministic compaction (no extra model call)
// - Preserve pinned leading system messages
// - Keep last K messages
// - Insert a single summary system message (named) so future compactions can replace it

struct ToolLoopCompactionOptions {
  // 0 means "use default" (20000).
  size_t max_chars = 0;
  // 0 means "use default" (16).
  size_t keep_last_messages = 0;

  bool insert_summary = true;
  size_t summary_preview_items = 3;
  size_t summary_snippet_chars = 160;
  size_t summary_max_chars = 600;
};

struct ToolLoopCompactionReport {
  size_t before_chars = 0;
  size_t after_chars = 0;
  size_t pinned_system_messages = 0;
  size_t dropped_messages = 0;
  bool inserted_summary = false;
  std::string summary;
};

// Name used on the inserted summary message. The compaction logic treats this as non-pinned
// so future compactions can replace the summary rather than accumulating multiple summaries.
const char* tool_loop_compaction_summary_name();

#if defined(AGENT_HAVE_JSONCPP)
// Rough, portable character estimate for an OpenAI-compatible JSON message array.
size_t tool_loop_compaction_estimate_chars(const Json::Value& messages);

// Returns count of pinned leading system messages. Stops at the first non-system,
// or at the first compaction-summary message (by name).
size_t tool_loop_compaction_pinned_system_prefix_count(const Json::Value& messages);

// Compacts `messages` in-place if needed to satisfy `max_chars`.
// Returns true if it modified the array (dropped messages and/or inserted summary).
bool tool_loop_compaction_maybe_compact(
  Json::Value* messages,
  const ToolLoopCompactionOptions& opt,
  ToolLoopCompactionReport* out_report
);

// Same as maybe_compact, but uses an explicit `max_chars` budget (instead of opt.max_chars),
// useful when you need to reserve room for an upcoming user message.
bool tool_loop_compaction_maybe_compact_with_budget(
  Json::Value* messages,
  size_t max_chars_budget,
  const ToolLoopCompactionOptions& opt,
  ToolLoopCompactionReport* out_report
);
#endif

