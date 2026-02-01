#include "sandbox_policy.h"

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

bool sandbox_tighten_yolo(bool daemon_yolo_default, bool requested_yolo, bool requested_yolo_set) {
  const bool req = requested_yolo_set ? requested_yolo : daemon_yolo_default;
  return daemon_yolo_default && req;
}

}  // namespace agentd
