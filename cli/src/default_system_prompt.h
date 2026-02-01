#pragma once

// Host-only policy prompt (CLI/daemon), not core behavior.
// Returns a process-lifetime stable pointer.
const char* default_host_system_prompt();

// Selects a built-in host system prompt profile by name.
// Unknown/empty profiles fall back to the default prompt.
//
// Known profiles (case-sensitive):
// - "default"
// - "jules_codex"
const char* host_system_prompt_for_profile(const char* profile);
