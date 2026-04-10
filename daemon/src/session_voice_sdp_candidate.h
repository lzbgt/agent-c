#pragma once

#include <string>

namespace agentd {

bool sdp_candidate_is_end_marker(const std::string& value);
std::string normalize_sdp_candidate_line(const std::string& value);

}  // namespace agentd
