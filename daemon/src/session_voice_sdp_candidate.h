#pragma once

#include <string>

namespace agentd {

bool sdp_candidate_is_end_marker(const std::string& value);
bool sdp_candidate_is_relay_candidate(const std::string& value);
std::string normalize_sdp_candidate_line(const std::string& value);
std::string strip_sdp_relay_candidate_lines(const std::string& sdp);

}  // namespace agentd
