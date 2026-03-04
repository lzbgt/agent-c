#pragma once

#include <string>
#include <vector>

namespace agentd {

struct AllowedRoot {
  std::string path;
  bool readonly = true;
};

struct MountAllowlist {
  std::vector<AllowedRoot> allowed_roots;
  std::vector<std::string> blocked_patterns;
  bool non_main_readonly = true;
};

struct MountAllowlistStatus {
  bool present = false;
  bool loaded = false;
  std::string path;
  std::string error;
  size_t allowed_roots = 0;
  size_t blocked_patterns = 0;
};

struct MountAllowlistInput {
  std::string host_path;
  std::string container_path;
  std::string container_prefix;
  bool is_main = true;
};

struct MountAllowlistDecision {
  bool allowed = false;
  bool readonly = true;
  std::string reason;
  std::string resolved_host_path;
  std::string resolved_container_path;
  std::string matched_root;
  std::string blocked_pattern;
};

std::string mount_allowlist_path();
const MountAllowlist* mount_allowlist_or_null();
MountAllowlistStatus mount_allowlist_status();
MountAllowlistDecision mount_allowlist_validate(const MountAllowlist* allowlist, const MountAllowlistInput& input);

}  // namespace agentd
