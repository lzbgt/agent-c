#pragma once

#include "toolset_host.h"

#include <string>

namespace agentd {

const char* host_policy_to_string(HostToolsetPolicyMode p);
bool host_policy_from_string(const std::string& s, HostToolsetPolicyMode* out);

// Policy tightening: a request can only *reduce* capabilities compared to the daemon default.
HostToolsetPolicyMode tighten_host_policy(HostToolsetPolicyMode base, HostToolsetPolicyMode requested);

bool normalize_tools_mode(const std::string& s, std::string* out);
bool tools_mode_allows(const std::string& daemon_tools, const std::string& requested_tools);

// Computes an "effective" yolo value that can only *tighten* compared to the daemon default:
// - if daemon_yolo_default is false, effective_yolo is always false (requests cannot enable yolo)
// - if daemon_yolo_default is true, requests may disable yolo by setting requested_yolo=false
bool sandbox_tighten_yolo(bool daemon_yolo_default, bool requested_yolo, bool requested_yolo_set);

}  // namespace agentd
