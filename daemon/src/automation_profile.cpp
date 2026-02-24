#include "automation_profile.h"

#include "policy_hooks.h"
#include "sandbox_policy.h"
#include "string_util.h"

namespace agentd {
namespace {

std::string normalize_profile(const std::string& profile) {
  return lower_copy(trim_copy(profile));
}

}  // namespace

std::vector<std::string> automation_profile_list() {
  return {"full", "guided", "strict", "custom"};
}

std::string automation_profile_from_config(const DaemonConfig& cfg) {
  const std::string mode = lower_copy(trim_copy(cfg.policy_mode));
  if (mode == "off" && cfg.yolo_default && cfg.host_policy == HostToolsetPolicyMode::Full) {
    return "full";
  }
  if (mode == "audit" && !cfg.yolo_default && cfg.host_policy == HostToolsetPolicyMode::ReadOnly) {
    return "guided";
  }
  if (mode == "enforce" && !cfg.yolo_default && cfg.host_policy == HostToolsetPolicyMode::ReadOnly) {
    return "strict";
  }
  return "custom";
}

bool automation_profile_apply(const std::string& profile, DaemonConfig* cfg, std::string* out_error) {
  if (!cfg) {
    if (out_error) *out_error = "automation_profile_apply: missing config";
    return false;
  }
  const std::string p = normalize_profile(profile);
  if (p.empty() || p == "custom") return true;
  if (p == "full") {
    cfg->yolo_default = true;
    cfg->host_policy = HostToolsetPolicyMode::Full;
    cfg->policy_mode = "off";
    return true;
  }
  if (p == "guided") {
    cfg->yolo_default = false;
    cfg->host_policy = HostToolsetPolicyMode::ReadOnly;
    cfg->policy_mode = "audit";
    return true;
  }
  if (p == "strict") {
    cfg->yolo_default = false;
    cfg->host_policy = HostToolsetPolicyMode::ReadOnly;
    cfg->policy_mode = "enforce";
    return true;
  }
  if (out_error) {
    *out_error = "unknown automation_profile (expected: full|guided|strict|custom)";
  }
  return false;
}

}  // namespace agentd
