#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

struct RtpAudioPayloadSpec {
  int payload_type = -1;
  std::string codec_name;
  int sample_rate_hz = 0;
  int channels = 1;
};

struct DecodedAudioFrame {
  int payload_type = -1;
  std::string codec_name;
  int sample_rate_hz = 0;
  int channels = 1;
  int samples_per_channel = 0;
  std::vector<int16_t> pcm_samples;
};

class InboundRtpAudioDecoder {
 public:
  InboundRtpAudioDecoder();
  ~InboundRtpAudioDecoder();

  InboundRtpAudioDecoder(const InboundRtpAudioDecoder&) = delete;
  InboundRtpAudioDecoder& operator=(const InboundRtpAudioDecoder&) = delete;

  void reset();
  bool configure_from_remote_sdp(const std::string& remote_sdp, std::string* out_err);
  const RtpAudioPayloadSpec* resolve_payload_spec(uint8_t payload_type) const;
  bool decode_payload(
    uint8_t payload_type,
    const unsigned char* payload,
    size_t payload_size,
    DecodedAudioFrame* out_frame,
    std::string* out_err);

  const std::vector<RtpAudioPayloadSpec>& payload_specs() const {
    return payload_specs_;
  }

 private:
#if defined(AGENTD_HAVE_OPUS)
  struct OpusDecoderWrapper;
  OpusDecoderWrapper* opus_decoder_ = nullptr;
#endif
  std::vector<RtpAudioPayloadSpec> payload_specs_;
};

}  // namespace agentd
