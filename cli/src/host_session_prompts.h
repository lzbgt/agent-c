#pragma once

#include "agent/agent.h"

#include <filesystem>
#include <string>

// Marker prefix for auto-injected project instructions derived from AGENTS.md files.
// Kept stable so long-lived sessions can refresh/deduplicate the prompt cleanly.
constexpr const char* kProjectInstructionsPrefix = "PROJECT_INSTRUCTIONS=agmd-v1";

// Build a pinned system prompt from AGENTS.md files discovered from start_dir upward.
// Returns "" when no AGENTS.md files are found or the start directory cannot be resolved.
std::string build_project_instructions_system_prompt(const std::filesystem::path& start_dir);

// Ensure the leading pinned system messages for a host session are present and ordered.
// Rebuilds the session only when the managed prefix changes or is missing.
bool ensure_pinned_host_session_prompts(
  agent_session_t** session_io,
  const std::string& host_system_profile,
  const std::string& client_profile_prompt,
  const std::string& client_profile_marker,
  const std::string& project_instructions_prompt
);
