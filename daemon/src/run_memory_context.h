#pragma once

#include "agent/agent.h"

#include <string>

namespace agentd {

enum class MemoryContextMode {
  Files,
  Search,
  Index,
};

struct MemoryContextPolicy {
  MemoryContextMode mode = MemoryContextMode::Files;
  bool include_structured = true; // memory/STRUCTURED.md
  bool include_core = true;       // memory/MEMORY.md
  bool include_daily = true;      // memory/YYYY-MM-DD.md
  bool include_session = true;    // memory/sessions/<session_id>.md
  int daily_days = 2;             // includes today + (daily_days-1) previous days
  size_t total_cap = 12000;
  bool search_use_index = true;
  bool search_case_sensitive = false;
  bool search_fallback_to_files = true;
  int search_max_results = 12;
  int search_max_snippet_chars = 800;
  int search_context_lines = 2;
};

bool build_memory_context_text(
  const std::string& state_dir,
  const std::string& session_id,
  const MemoryContextPolicy& pol,
  const std::string& query,
  std::string* out_text
);

agent_session_t* clone_session_with_memory_context(const agent_session_t* src, const std::string& memory_context);

}  // namespace agentd
