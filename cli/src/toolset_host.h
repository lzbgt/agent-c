#pragma once

#include "agent/tools.h"

#include <string>

struct HostToolsetConfig {
  // Optional root directory for host-side file editing tools.
  //
  // Notes:
  // - The "host" toolset intentionally avoids bespoke filesystem APIs; the LLM can use OS-native
  //   commands (ls/cat/rm/git/etc) via `shell_exec` / `proc_exec`.
  // - When `root_dir` is set, patch application (`file_apply_patch`) runs under that directory
  //   (via `git -C <root_dir> apply ...`).
  // - When empty, `file_apply_patch` runs from the current working directory (YOLO mode).
  std::string root_dir;
};

// Creates a "host" tool registry + executor suitable for CLI/daemon usage:
// - shell_exec (system-installed binaries via /bin/sh -lc)
// - proc_exec (argv-based execution, no shell)
// - file_apply_patch (unified diff apply via `git apply`, returns the patch for audit)
//
// This is a host adapter: embedded targets should implement their own toolset (GPIO, sensors, etc.).
agent_status_t toolset_host_create(const HostToolsetConfig& cfg, agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor);

// Destroy resources owned by the executor (tool registry remains owned by caller).
void toolset_host_destroy(agent_tool_executor_t* executor);
