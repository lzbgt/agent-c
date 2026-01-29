#pragma once

#include "agent/tools.h"

#include <string>

using HostCancelCallback = bool (*)(void* ctx);

struct HostToolsetConfig {
  // Optional root directory for host-side tools.
  //
  // Notes:
  // - When `root_dir` is set, host tools enforce containment for file APIs (fs_* and file_apply_patch)
  //   and run patch application under that directory (via `git -C <root_dir> apply ...`).
  // - When empty, host tools run from the current working directory in unrestricted "YOLO" mode.
  std::string root_dir;

  // Optional cooperative cancellation hook.
  // If set and it returns true, long-running tools (shell/proc exec) will terminate their subprocess and return early.
  HostCancelCallback should_cancel = nullptr;
  void* should_cancel_ctx = nullptr;
};

// Creates a "host" tool registry + executor suitable for CLI/daemon usage:
// - shell_exec (system-installed binaries via /bin/sh -lc)
// - proc_exec (argv-based execution, no shell)
// - file_apply_patch (unified diff apply via `git apply`, returns the patch for audit)
// - fs_stat/fs_list/fs_read (bounded filesystem inspection for token efficiency)
//
// This is a host adapter: embedded targets should implement their own toolset (GPIO, sensors, etc.).
agent_status_t toolset_host_create(const HostToolsetConfig& cfg, agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor);

// Destroy resources owned by the executor (tool registry remains owned by caller).
void toolset_host_destroy(agent_tool_executor_t* executor);
