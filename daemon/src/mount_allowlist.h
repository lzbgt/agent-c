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

std::string mount_allowlist_path();
const MountAllowlist* mount_allowlist_or_null();
MountAllowlistStatus mount_allowlist_status();

}  // namespace agentd
