#pragma once

#include <cstdint>
#include <string>

namespace agentd {

struct BuiltinSdpAnswerInput {
  std::string remote_sdp;
  std::string fallback_local_description;
  bool dtls_identity_ready = false;
  std::string dtls_setup_role;
  std::string dtls_fingerprint_sha256;
  uint32_t outbound_audio_ssrc = 0;
  std::string outbound_audio_cname;
  std::string outbound_audio_stream_id;
  std::string outbound_audio_track_id;
};

bool sdp_contains_media_section(const std::string& sdp);
bool sdp_is_end_of_candidates_marker(const std::string& value);
std::string build_builtin_active_answer_sdp(const BuiltinSdpAnswerInput& input);

}  // namespace agentd
