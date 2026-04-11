#pragma once

#include "session_voice_audio_decode.h"
#include "session_voice_audio_encode.h"

#include <string>
#include <vector>

namespace agentd {

struct BuiltinOutboundAudioPayloadSelection {
  int payload_type = -1;
  int sample_rate_hz = 0;
  int channels = 0;
  std::string codec_name;
  std::string error;
};

void clear_builtin_outbound_audio_payload_selection(
  OutboundRtpAudioEncoder* encoder,
  BuiltinOutboundAudioPayloadSelection* out_selection);

bool select_builtin_outbound_audio_payload(
  OutboundRtpAudioEncoder* encoder,
  const std::vector<RtpAudioPayloadSpec>& payload_specs,
  BuiltinOutboundAudioPayloadSelection* out_selection);

bool select_builtin_outbound_audio_payload_from_answer_sdp(
  OutboundRtpAudioEncoder* encoder,
  const std::string& answer_sdp,
  const std::vector<RtpAudioPayloadSpec>& fallback_payload_specs,
  bool allow_ice_only_fallback,
  BuiltinOutboundAudioPayloadSelection* out_selection);

}  // namespace agentd
