#pragma once

#include "toolset_host.h"

#include <string>

namespace agentd {

const char* host_policy_to_string(HostToolsetPolicyMode p);
bool host_policy_from_string(const std::string& s, HostToolsetPolicyMode* out);

// Policy tightening: a request can only *reduce* capabilities compared to the daemon default.
HostToolsetPolicyMode tighten_host_policy(HostToolsetPolicyMode base, HostToolsetPolicyMode requested);

// Computes an "effective" yolo value that can only *tighten* compared to the daemon default:
// - if daemon_yolo_default is false, effective_yolo is always false (requests cannot enable yolo)
// - if daemon_yolo_default is true, requests may disable yolo by setting requested_yolo=false
bool sandbox_tighten_yolo(bool daemon_yolo_default, bool requested_yolo, bool requested_yolo_set);

// Resolves a requested tools root into an effective absolute root (string path) under a given host scope root.
// Semantics:
// - When effective_yolo is true: out_effective_root is set to "" (unrestricted).
// - When effective_yolo is false: out_effective_root is a non-empty absolute path within host_scope_root.
// - Requests cannot loosen to "@cwd"/"" when effective_yolo is false; those will be clamped to daemon_tools_root (or host_scope_root).
// - Special values:
//   - "@host" => host_scope_root
//   - "@cwd"  => "" (unrestricted; only allowed when effective_yolo==true)
//
// Returns true on success; false on error (e.g. requested root escapes host_scope_root).
bool sandbox_resolve_tools_root(
  const std::string& host_scope_root,
  bool effective_yolo,
  const std::string& daemon_tools_root,
  const std::string& requested_tools_root,
  bool requested_tools_root_set,
  std::string* out_effective_root,
  std::string* out_error
);

}  // namespace agentd
