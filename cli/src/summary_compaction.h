#pragma once

#include "agent/agent.h"

#include <cstddef>
#include <string>

struct SummaryCompactionInput {
  std::string excerpt;
  size_t pinned_system_messages = 0;
  size_t dropped_messages = 0;
  size_t kept_suffix_messages = 0;
  bool truncated = false;
};

// Builds a bounded excerpt of the "dropped region" that would be removed by the core compaction policy:
// - keep pinned system messages (leading system prefix)
// - keep last K messages (suffix)
// - drop the middle region
//
// This helper is host-only and is used to generate an LLM summary for dropped content before compaction runs.
SummaryCompactionInput build_summary_compaction_input(
  const agent_session_t* session,
  size_t keep_last_messages,
  size_t max_excerpt_chars = 8000,
  size_t max_messages = 64,
  size_t per_message_chars = 600
);

