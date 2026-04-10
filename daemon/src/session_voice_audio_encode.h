#pragma once

#include "session_voice_audio_decode.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

struct EncodedRtpAudioPayload20ms {
  std::vector<unsigned char> payload;
  uint32_t timestamp_increment = 0;
};

class OutboundRtpAudioEncoder {
 public:
  OutboundRtpAudioEncoder();
  ~OutboundRtpAudioEncoder();

  OutboundRtpAudioEncoder(const OutboundRtpAudioEncoder&) = delete;
  OutboundRtpAudioEncoder& operator=(const OutboundRtpAudioEncoder&) = delete;

  void reset();
  bool select_payload(
    const std::vector<RtpAudioPayloadSpec>& payload_specs,
    std::string* out_err);
  bool encode_20ms(
    const int16_t* pcm,
    size_t pcm_samples,
    int sample_rate_hz,
    int channels,
    EncodedRtpAudioPayload20ms* out_payload,
    std::string* out_err);

  bool has_payload() const {
    return selected_payload_.payload_type >= 0 && !selected_payload_.codec_name.empty();
  }

  const RtpAudioPayloadSpec* selected_payload() const {
    return has_payload() ? &selected_payload_ : nullptr;
  }

 private:
#if defined(AGENTD_HAVE_OPUS)
  struct OpusEncoderState;

  bool ensure_opus_encoder(int sample_rate_hz, int channels, std::string* out_err);
  void reset_opus_encoder();

  OpusEncoderState* opus_state_ = nullptr;
#endif

  RtpAudioPayloadSpec selected_payload_;
};

}  // namespace agentd
