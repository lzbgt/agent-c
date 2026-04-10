#include "session_voice_builtin_progress_key.h"

#include <cassert>

namespace {

void test_progress_key_equality_includes_media_counters() {
  agentd::BuiltinVoiceAsyncProgressKey lhs;
  agentd::BuiltinVoiceAsyncProgressKey rhs;
  assert(lhs == rhs);
  lhs.rtp_packets_received = 1;
  assert(lhs != rhs);
  rhs.rtp_packets_received = 1;
  assert(lhs == rhs);
  lhs.audio_outbound_codec_name = "OPUS";
  assert(lhs != rhs);
  rhs.audio_outbound_codec_name = "OPUS";
  assert(lhs == rhs);
  lhs.rtcp_last_error = "srtcp";
  assert(lhs != rhs);
}

void test_progress_key_equality_includes_transport_state() {
  agentd::BuiltinVoiceAsyncProgressKey lhs;
  agentd::BuiltinVoiceAsyncProgressKey rhs;
  lhs.libjuice_state = "connected";
  assert(lhs != rhs);
  rhs.libjuice_state = "connected";
  assert(lhs == rhs);
  lhs.transport_connectivity_ready = true;
  assert(lhs != rhs);
  rhs.transport_connectivity_ready = true;
  assert(lhs == rhs);
  lhs.selected_remote_candidate = "candidate:1";
  assert(lhs != rhs);
}

}  // namespace

int main() {
  test_progress_key_equality_includes_media_counters();
  test_progress_key_equality_includes_transport_state();
  return 0;
}
