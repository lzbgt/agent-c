#include "session_voice_builtin_audio_payload.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

std::string answer_with_opus_and_g711() {
  return
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=sendrecv\r\n";
}

std::string answer_with_only_unsupported_payload() {
  return
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 9\r\n"
    "a=rtpmap:9 G722/8000\r\n"
    "a=sendrecv\r\n";
}

std::vector<agentd::RtpAudioPayloadSpec> fallback_pcma_specs() {
  return {
    agentd::RtpAudioPayloadSpec{8, "PCMA", 8000, 1},
  };
}

void test_selects_payload_from_active_answer_audio_mline() {
  agentd::OutboundRtpAudioEncoder encoder;
  agentd::BuiltinOutboundAudioPayloadSelection selection;
  assert(agentd::select_builtin_outbound_audio_payload_from_answer_sdp(
    &encoder,
    answer_with_opus_and_g711(),
    fallback_pcma_specs(),
    true,
    &selection));
  assert(selection.error.empty());
#if defined(AGENTD_HAVE_OPUS)
  assert(selection.payload_type == 111);
  assert(selection.codec_name == "OPUS");
  assert(selection.sample_rate_hz == 48000);
  assert(selection.channels == 2);
#else
  assert(selection.payload_type == 0);
  assert(selection.codec_name == "PCMU");
  assert(selection.sample_rate_hz == 8000);
  assert(selection.channels == 1);
#endif
  assert(encoder.selected_payload());
  assert(encoder.selected_payload()->payload_type == selection.payload_type);
}

void test_ice_only_answer_uses_fallback_when_allowed() {
  agentd::OutboundRtpAudioEncoder encoder;
  agentd::BuiltinOutboundAudioPayloadSelection selection;
  assert(agentd::select_builtin_outbound_audio_payload_from_answer_sdp(
    &encoder,
    "v=0\r\ns=-\r\n",
    fallback_pcma_specs(),
    true,
    &selection));
  assert(selection.payload_type == 8);
  assert(selection.codec_name == "PCMA");
  assert(selection.error.empty());
}

void test_ice_only_answer_rejects_without_fallback_permission() {
  agentd::OutboundRtpAudioEncoder encoder;
  agentd::BuiltinOutboundAudioPayloadSelection selection;
  assert(!agentd::select_builtin_outbound_audio_payload_from_answer_sdp(
    &encoder,
    "v=0\r\ns=-\r\n",
    fallback_pcma_specs(),
    false,
    &selection));
  assert(selection.payload_type == -1);
  assert(selection.codec_name.empty());
  assert(selection.error.find("supported outbound audio payload") != std::string::npos);
  assert(!encoder.selected_payload());
}

void test_active_unsupported_answer_does_not_use_fallback() {
  agentd::OutboundRtpAudioEncoder encoder;
  agentd::BuiltinOutboundAudioPayloadSelection selection;
  assert(!agentd::select_builtin_outbound_audio_payload_from_answer_sdp(
    &encoder,
    answer_with_only_unsupported_payload(),
    fallback_pcma_specs(),
    true,
    &selection));
  assert(selection.payload_type == -1);
  assert(selection.codec_name.empty());
  assert(selection.error.find("supported outbound audio payload") != std::string::npos);
  assert(!encoder.selected_payload());
}

void test_failure_clears_stale_encoder_selection() {
  agentd::OutboundRtpAudioEncoder encoder;
  agentd::BuiltinOutboundAudioPayloadSelection selection;
  assert(agentd::select_builtin_outbound_audio_payload(
    &encoder,
    fallback_pcma_specs(),
    &selection));
  assert(encoder.selected_payload());
  assert(selection.payload_type == 8);

  const std::vector<agentd::RtpAudioPayloadSpec> unsupported_specs = {
    agentd::RtpAudioPayloadSpec{9, "G722", 8000, 1},
  };
  assert(!agentd::select_builtin_outbound_audio_payload(
    &encoder,
    unsupported_specs,
    &selection));
  assert(!encoder.selected_payload());
  assert(selection.payload_type == -1);
  assert(selection.codec_name.empty());
  assert(selection.error.find("supported outbound audio payload") != std::string::npos);
}

}  // namespace

int main() {
  test_selects_payload_from_active_answer_audio_mline();
  test_ice_only_answer_uses_fallback_when_allowed();
  test_ice_only_answer_rejects_without_fallback_permission();
  test_active_unsupported_answer_does_not_use_fallback();
  test_failure_clears_stale_encoder_selection();
  return 0;
}
