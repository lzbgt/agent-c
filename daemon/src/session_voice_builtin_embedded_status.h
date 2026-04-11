#pragma once

#include "session_voice_builtin_progress_key.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace agentd {

struct BuiltinEmbeddedMediaStatusSnapshot {
  BuiltinVoiceAsyncProgressKey progress;
  std::string event_name;
  std::string media_engine_state;
  std::string provider_name = "agentd_builtin_embedded_transport_provider";
  std::string transport_family = "embedded_transport_primitives";
  std::string srtp_version = "unknown";
  std::string dtls_setup_role = "passive";
  std::string dtls_fingerprint_sha256;
  std::string dtls_certificate_subject;
  std::string dtls_last_error;
  std::string sdp_answer_shape = "ice_only";
  size_t local_description_bytes = 0;
  uint64_t dtls_packets_sent = 0;
  uint64_t dtls_packets_received = 0;
  uint64_t initial_remote_candidate_count = 0;
  bool usrsctp_initialized = true;
};

std::string escape_builtin_embedded_media_json_string(const std::string& value);

std::string derive_builtin_embedded_media_engine_state(
  const BuiltinVoiceAsyncProgressKey& progress);

std::string build_builtin_embedded_media_event_json(
  const BuiltinEmbeddedMediaStatusSnapshot& snapshot);

}  // namespace agentd
