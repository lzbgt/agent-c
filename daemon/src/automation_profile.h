#pragma once

#include "daemon_config.h"

#include <string>
#include <vector>

namespace agentd {

std::vector<std::string> automation_profile_list();
std::string automation_profile_from_config(const DaemonConfig& cfg);
bool automation_profile_apply(const std::string& profile, DaemonConfig* cfg, std::string* out_error);

}  // namespace agentd
