#include "session_voice_builtin_audio_payload.h"

namespace agentd {
namespace {

constexpr const char* kNoSupportedOutboundPayload =
  "remote SDP did not negotiate a supported outbound audio payload";

void reset_selection(BuiltinOutboundAudioPayloadSelection* out_selection) {
  if (out_selection) *out_selection = BuiltinOutboundAudioPayloadSelection{};
}

void set_selection_error(
  BuiltinOutboundAudioPayloadSelection* out_selection,
  const std::string& error
) {
  if (out_selection) {
    out_selection->error = error.empty() ? std::string(kNoSupportedOutboundPayload) : error;
  }
}

}  // namespace

void clear_builtin_outbound_audio_payload_selection(
  OutboundRtpAudioEncoder* encoder,
  BuiltinOutboundAudioPayloadSelection* out_selection
) {
  if (encoder) encoder->reset();
  reset_selection(out_selection);
}

bool select_builtin_outbound_audio_payload(
  OutboundRtpAudioEncoder* encoder,
  const std::vector<RtpAudioPayloadSpec>& payload_specs,
  BuiltinOutboundAudioPayloadSelection* out_selection
) {
  clear_builtin_outbound_audio_payload_selection(encoder, out_selection);
  if (!encoder) {
    set_selection_error(out_selection, "missing outbound audio encoder");
    return false;
  }

  std::string select_err;
  if (!encoder->select_payload(payload_specs, &select_err)) {
    set_selection_error(out_selection, select_err);
    return false;
  }

  const RtpAudioPayloadSpec* selected = encoder->selected_payload();
  if (!selected) {
    encoder->reset();
    set_selection_error(out_selection, kNoSupportedOutboundPayload);
    return false;
  }

  if (out_selection) {
    out_selection->payload_type = selected->payload_type;
    out_selection->sample_rate_hz = selected->sample_rate_hz;
    out_selection->channels = selected->channels;
    out_selection->codec_name = selected->codec_name;
    out_selection->error.clear();
  }
  return true;
}

bool select_builtin_outbound_audio_payload_from_answer_sdp(
  OutboundRtpAudioEncoder* encoder,
  const std::string& answer_sdp,
  const std::vector<RtpAudioPayloadSpec>& fallback_payload_specs,
  bool allow_ice_only_fallback,
  BuiltinOutboundAudioPayloadSelection* out_selection
) {
  std::vector<RtpAudioPayloadSpec> payload_specs =
    parse_first_active_audio_payload_specs_from_sdp(answer_sdp);
  if (payload_specs.empty() && allow_ice_only_fallback) {
    payload_specs = fallback_payload_specs;
  }
  return select_builtin_outbound_audio_payload(encoder, payload_specs, out_selection);
}

}  // namespace agentd
