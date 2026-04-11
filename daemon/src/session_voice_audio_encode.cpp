#include "session_voice_audio_encode.h"

#include <algorithm>
#include <array>
#include <cctype>

#if defined(AGENTD_HAVE_OPUS)
#include <opus.h>
#endif

namespace agentd {
namespace {

constexpr size_t kOutboundG711FrameSamples = 160;
constexpr size_t kOutboundOpusFrameSamplesPerChannel = 960;
constexpr int kOutboundOpusMaxPayloadBytes = 1275;

uint8_t encode_pcmu_sample(int16_t sample) {
  constexpr int kBias = 0x84;
  constexpr int kClip = 32635;
  static constexpr std::array<int, 8> kSegmentEnd = {{
    0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF,
  }};

  int value = static_cast<int>(sample);
  uint8_t mask = 0xFFu;
  if (value < 0) {
    value = -value;
    mask = 0x7Fu;
  }
  value = std::min(value, kClip) + kBias;

  int segment = 0;
  while (segment < static_cast<int>(kSegmentEnd.size()) && value > kSegmentEnd[segment]) {
    segment += 1;
  }
  if (segment >= static_cast<int>(kSegmentEnd.size())) {
    return static_cast<uint8_t>(0x7Fu ^ mask);
  }
  const uint8_t encoded = static_cast<uint8_t>(
    (segment << 4) | ((value >> (segment + 3)) & 0x0F));
  return static_cast<uint8_t>(encoded ^ mask);
}

uint8_t encode_pcma_sample(int16_t sample) {
  static constexpr std::array<int, 8> kSegmentEnd = {{
    0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF,
  }};

  int value = static_cast<int>(sample);
  uint8_t mask = 0xD5u;
  if (value < 0) {
    value = -value - 1;
    mask = 0x55u;
  }
  value = std::min(value, 32635);

  int segment = 0;
  while (segment < static_cast<int>(kSegmentEnd.size()) && value > kSegmentEnd[segment]) {
    segment += 1;
  }
  if (segment >= static_cast<int>(kSegmentEnd.size())) {
    return static_cast<uint8_t>(0x7Fu ^ mask);
  }

  uint8_t encoded = static_cast<uint8_t>(segment << 4);
  encoded |= static_cast<uint8_t>(
    segment < 2 ? ((value >> 4) & 0x0F) : ((value >> (segment + 3)) & 0x0F));
  return static_cast<uint8_t>(encoded ^ mask);
}

std::vector<unsigned char> encode_pcm16_to_g711_20ms(
  const int16_t* pcm,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  const std::string& codec_name
) {
  const bool use_pcma = codec_name == "PCMA";
  std::vector<unsigned char> payload(
    kOutboundG711FrameSamples,
    use_pcma ? encode_pcma_sample(0) : encode_pcmu_sample(0));
  if (!pcm || pcm_samples == 0 || sample_rate_hz <= 0 || channels <= 0) return payload;

  const size_t channels_sz = static_cast<size_t>(channels);
  const size_t frame_count = pcm_samples / channels_sz;
  if (frame_count == 0) return payload;
  for (size_t i = 0; i < payload.size(); ++i) {
    size_t source_frame =
      (static_cast<uint64_t>(i) * static_cast<uint64_t>(sample_rate_hz)) / 8000u;
    if (source_frame >= frame_count) source_frame = frame_count - 1;
    payload[i] = use_pcma
      ? encode_pcma_sample(pcm[source_frame * channels_sz])
      : encode_pcmu_sample(pcm[source_frame * channels_sz]);
  }
  return payload;
}

std::vector<int16_t> resample_interleaved_pcm16_20ms(
  const int16_t* pcm,
  size_t pcm_samples,
  int source_sample_rate_hz,
  int source_channels,
  int target_sample_rate_hz,
  int target_channels,
  size_t target_samples_per_channel
) {
  std::vector<int16_t> out(target_samples_per_channel * static_cast<size_t>(target_channels), 0);
  if (!pcm || pcm_samples == 0 ||
      source_sample_rate_hz <= 0 || source_channels <= 0 ||
      target_sample_rate_hz <= 0 || target_channels <= 0 ||
      target_samples_per_channel == 0) {
    return out;
  }

  const size_t source_channels_sz = static_cast<size_t>(source_channels);
  const size_t target_channels_sz = static_cast<size_t>(target_channels);
  const size_t source_frames = pcm_samples / source_channels_sz;
  if (source_frames == 0) return out;

  for (size_t frame = 0; frame < target_samples_per_channel; ++frame) {
    size_t source_frame =
      (static_cast<uint64_t>(frame) * static_cast<uint64_t>(source_sample_rate_hz)) /
      static_cast<uint64_t>(target_sample_rate_hz);
    if (source_frame >= source_frames) source_frame = source_frames - 1;
    for (size_t channel = 0; channel < target_channels_sz; ++channel) {
      const size_t source_channel =
        std::min(channel, source_channels_sz - static_cast<size_t>(1));
      out[frame * target_channels_sz + channel] =
        pcm[source_frame * source_channels_sz + source_channel];
    }
  }
  return out;
}

std::string upper_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

const RtpAudioPayloadSpec* find_payload_by_codec(
  const std::vector<RtpAudioPayloadSpec>& payload_specs,
  const std::string& codec_name,
  int sample_rate_hz,
  int min_channels,
  int max_channels
) {
  for (const auto& spec : payload_specs) {
    if (upper_copy(spec.codec_name) != codec_name) continue;
    if (spec.sample_rate_hz != sample_rate_hz) continue;
    if (spec.channels < min_channels || spec.channels > max_channels) continue;
    return &spec;
  }
  return nullptr;
}

}  // namespace

#if defined(AGENTD_HAVE_OPUS)
struct OutboundRtpAudioEncoder::OpusEncoderState {
  OpusEncoder* encoder = nullptr;
  int sample_rate_hz = 0;
  int channels = 0;

  ~OpusEncoderState() {
    if (encoder) opus_encoder_destroy(encoder);
  }
};
#endif

OutboundRtpAudioEncoder::OutboundRtpAudioEncoder() {
#if defined(AGENTD_HAVE_OPUS)
  opus_state_ = new OpusEncoderState();
#endif
}

OutboundRtpAudioEncoder::~OutboundRtpAudioEncoder() {
#if defined(AGENTD_HAVE_OPUS)
  delete opus_state_;
  opus_state_ = nullptr;
#endif
}

void OutboundRtpAudioEncoder::reset() {
  selected_payload_ = RtpAudioPayloadSpec();
#if defined(AGENTD_HAVE_OPUS)
  reset_opus_encoder();
#endif
}

#if defined(AGENTD_HAVE_OPUS)
void OutboundRtpAudioEncoder::reset_opus_encoder() {
  if (!opus_state_) return;
  if (opus_state_->encoder) {
    opus_encoder_destroy(opus_state_->encoder);
    opus_state_->encoder = nullptr;
  }
  opus_state_->sample_rate_hz = 0;
  opus_state_->channels = 0;
}
#endif

bool OutboundRtpAudioEncoder::select_payload(
  const std::vector<RtpAudioPayloadSpec>& payload_specs,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  const RtpAudioPayloadSpec* selected = nullptr;
#if defined(AGENTD_HAVE_OPUS)
  selected = find_payload_by_codec(payload_specs, "OPUS", 48000, 1, 2);
#endif
  if (!selected) selected = find_payload_by_codec(payload_specs, "PCMU", 8000, 1, 1);
  if (!selected) selected = find_payload_by_codec(payload_specs, "PCMA", 8000, 1, 1);
  if (!selected) {
    reset();
    if (out_err) *out_err = "remote SDP did not negotiate a supported outbound audio payload";
    return false;
  }

  RtpAudioPayloadSpec normalized_selected = *selected;
  normalized_selected.codec_name = upper_copy(normalized_selected.codec_name);
#if defined(AGENTD_HAVE_OPUS)
  if (selected_payload_.codec_name != normalized_selected.codec_name ||
      selected_payload_.sample_rate_hz != normalized_selected.sample_rate_hz ||
      selected_payload_.channels != normalized_selected.channels) {
    reset_opus_encoder();
  }
#endif
  selected_payload_ = normalized_selected;
  return true;
}

#if defined(AGENTD_HAVE_OPUS)
bool OutboundRtpAudioEncoder::ensure_opus_encoder(
  int sample_rate_hz,
  int channels,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!opus_state_) {
    if (out_err) *out_err = "missing outbound Opus encoder state";
    return false;
  }
  if (sample_rate_hz != 48000 || (channels != 1 && channels != 2)) {
    if (out_err) *out_err = "unsupported outbound Opus RTP format";
    return false;
  }
  if (opus_state_->encoder &&
      opus_state_->sample_rate_hz == sample_rate_hz &&
      opus_state_->channels == channels) {
    return true;
  }
  reset_opus_encoder();

  int opus_err = OPUS_OK;
  opus_state_->encoder =
    opus_encoder_create(sample_rate_hz, channels, OPUS_APPLICATION_AUDIO, &opus_err);
  if (!opus_state_->encoder) {
    if (out_err) *out_err = std::string("opus encoder creation failed: ") + opus_strerror(opus_err);
    return false;
  }
  opus_state_->sample_rate_hz = sample_rate_hz;
  opus_state_->channels = channels;
  return true;
}
#endif

bool OutboundRtpAudioEncoder::encode_20ms(
  const int16_t* pcm,
  size_t pcm_samples,
  int sample_rate_hz,
  int channels,
  EncodedRtpAudioPayload20ms* out_payload,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_payload) *out_payload = EncodedRtpAudioPayload20ms();
  if (!out_payload) {
    if (out_err) *out_err = "missing outbound audio payload output";
    return false;
  }
  if (!has_payload()) {
    if (out_err) *out_err = "outbound audio payload was not negotiated";
    return false;
  }

  if (selected_payload_.codec_name == "PCMU" || selected_payload_.codec_name == "PCMA") {
    out_payload->payload = encode_pcm16_to_g711_20ms(
      pcm, pcm_samples, sample_rate_hz, channels, selected_payload_.codec_name);
    out_payload->timestamp_increment = static_cast<uint32_t>(out_payload->payload.size());
    return true;
  }

#if defined(AGENTD_HAVE_OPUS)
  if (selected_payload_.codec_name == "OPUS") {
    if (!ensure_opus_encoder(
          selected_payload_.sample_rate_hz,
          selected_payload_.channels,
          out_err)) {
      return false;
    }
    const std::vector<int16_t> opus_pcm = resample_interleaved_pcm16_20ms(
      pcm,
      pcm_samples,
      sample_rate_hz,
      channels,
      selected_payload_.sample_rate_hz,
      selected_payload_.channels,
      kOutboundOpusFrameSamplesPerChannel);

    std::array<unsigned char, kOutboundOpusMaxPayloadBytes> encoded{};
    const int encoded_bytes = opus_encode(
      opus_state_->encoder,
      reinterpret_cast<const opus_int16*>(opus_pcm.data()),
      static_cast<int>(kOutboundOpusFrameSamplesPerChannel),
      encoded.data(),
      static_cast<opus_int32>(encoded.size()));
    if (encoded_bytes < 0) {
      if (out_err) *out_err = std::string("opus encode failed: ") + opus_strerror(encoded_bytes);
      return false;
    }
    if (encoded_bytes == 0) {
      if (out_err) *out_err = "opus encode returned an empty payload";
      return false;
    }
    out_payload->payload.assign(encoded.data(), encoded.data() + encoded_bytes);
    out_payload->timestamp_increment =
      static_cast<uint32_t>(kOutboundOpusFrameSamplesPerChannel);
    return true;
  }
#endif

  if (out_err) *out_err = "outbound audio payload codec was not negotiated";
  return false;
}

}  // namespace agentd
