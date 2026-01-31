#pragma once

#include "agent/tools.h"

#include <cstddef>
#include <string>

using HostCancelCallback = bool (*)(void* ctx);
using HostReadClientEventsTailCallback =
  agent_status_t (*)(void* ctx, const std::string& session_id, size_t max_bytes, size_t max_files, std::string* out_tail_jsonl);

enum class HostToolsetPolicyMode {
  Full = 0,     // all host tools enabled (exec + patch + fs)
  ReadOnly = 1, // no process execution or patch application
};

struct HostToolsetConfig {
  // Optional root directory for host-side tools.
  //
  // Notes:
  // - When `root_dir` is set, host tools enforce containment for file APIs (fs_* and file_apply_patch)
  //   and run patch application under that directory (via `git -C <root_dir> apply ...`).
  // - When empty, host tools run from the current working directory in unrestricted "YOLO" mode.
  std::string root_dir;

  // Safety policy for which host tools are exposed via the tool registry/executor.
  HostToolsetPolicyMode policy = HostToolsetPolicyMode::Full;

  // When false, omit process execution tools from the registry and reject them in the executor
  // (defense in depth). This does not affect bounded filesystem inspection tools.
  //
  // Daemon note:
  // - When the daemon runs with `yolo=false` (scoped tools_root), it may disable exec tools even if policy=full.
  // CLI note:
  // - The CLI may keep exec tools enabled even when root_dir is set (local, interactive usage).
  bool enable_process_exec = true;

  // When false and `root_dir` is set (scoped mode), reject paths that traverse through symlinks
  // under the root. This prevents "symlink escapes" where a directory entry inside the root
  // points outside of it (e.g. root/out -> /).
  //
  // Default: true for backwards compatibility and local CLI ergonomics.
  // Daemon note: recommended false when yolo is disabled.
  bool allow_symlinks = true;

  // Optional cooperative cancellation hook.
  // If set and it returns true, long-running tools (shell/proc exec) will terminate their subprocess and return early.
  HostCancelCallback should_cancel = nullptr;
  void* should_cancel_ctx = nullptr;

  // Optional session context (daemon / UI integration).
  //
  // When set (non-empty), some host tools can coordinate with session-scoped UI events.
  // - sessions_root_dir: directory containing <session_id>.client_events.jsonl (written by agentd).
  // - session_id: current run's session id.
  //
  // This is intentionally optional so CLI runs can ignore it.
  std::string sessions_root_dir;
  std::string session_id;

  // Optional: DB-backed session client event log reader (preferred).
  //
  // This enables tools like `client_wait_event` and `client_peek` to read session client events even when
  // agentd stores them in SQLite instead of session-scoped JSONL files.
  //
  // When set, host tools prefer this callback over reading files from sessions_root_dir.
  HostReadClientEventsTailCallback read_client_events_tail = nullptr;
  void* read_client_events_tail_ctx = nullptr;
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
