#include "sandbox_policy.h"

#include "string_util.h"

namespace agentd {

const char* host_policy_to_string(HostToolsetPolicyMode p) {
  return (p == HostToolsetPolicyMode::ReadOnly) ? "readonly" : "full";
}

bool host_policy_from_string(const std::string& s, HostToolsetPolicyMode* out) {
  if (!out) return false;
  if (s == "full") {
    *out = HostToolsetPolicyMode::Full;
    return true;
  }
  if (s == "readonly") {
    *out = HostToolsetPolicyMode::ReadOnly;
    return true;
  }
  return false;
}

HostToolsetPolicyMode tighten_host_policy(HostToolsetPolicyMode base, HostToolsetPolicyMode requested) {
  if (base == HostToolsetPolicyMode::ReadOnly) {
    return HostToolsetPolicyMode::ReadOnly;
  }
  if (requested == HostToolsetPolicyMode::ReadOnly) {
    return HostToolsetPolicyMode::ReadOnly;
  }
  return HostToolsetPolicyMode::Full;
}

namespace {

int tools_mode_rank(const std::string& s) {
  if (s == "none") return 0;
  if (s == "basic") return 1;
  if (s == "host") return 2;
  return -1;
}

}  // namespace

bool normalize_tools_mode(const std::string& s, std::string* out) {
  if (!out) return false;
  std::string v = trim_copy(lower_copy(s));
  if (tools_mode_rank(v) < 0) return false;
  *out = std::move(v);
  return true;
}

bool tools_mode_allows(const std::string& daemon_tools, const std::string& requested_tools) {
  const int daemon_rank = tools_mode_rank(daemon_tools);
  const int requested_rank = tools_mode_rank(requested_tools);
  if (daemon_rank < 0 || requested_rank < 0) return false;
  return requested_rank <= daemon_rank;
}

bool sandbox_tighten_yolo(bool daemon_yolo_default, bool requested_yolo, bool requested_yolo_set) {
  const bool req = requested_yolo_set ? requested_yolo : daemon_yolo_default;
  return daemon_yolo_default && req;
}

}  // namespace agentd
