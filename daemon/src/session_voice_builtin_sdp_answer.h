#pragma once

#include <string>

namespace agentd {

struct BuiltinSdpAnswerInput {
  std::string remote_sdp;
  std::string fallback_local_description;
  bool dtls_identity_ready = false;
  std::string dtls_setup_role;
  std::string dtls_fingerprint_sha256;
};

bool sdp_contains_media_section(const std::string& sdp);
bool sdp_is_end_of_candidates_marker(const std::string& value);
std::string build_builtin_inactive_answer_sdp(const BuiltinSdpAnswerInput& input);

}  // namespace agentd
