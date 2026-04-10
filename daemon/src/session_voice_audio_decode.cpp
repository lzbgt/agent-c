#include "session_voice_audio_decode.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(AGENTD_HAVE_OPUS)
#include <opus.h>
#endif

namespace agentd {
namespace {

std::string trim_copy(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    begin += 1;
  }
  size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end -= 1;
  }
  return value.substr(begin, end - begin);
}

std::string upper_copy(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return out;
}

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> split_sdp_lines(const std::string& sdp) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < sdp.size()) {
    const size_t end = sdp.find('\n', start);
    std::string line = end == std::string::npos ? sdp.substr(start) : sdp.substr(start, end - start);
    line = trim_copy(line);
    if (!line.empty()) out.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

std::vector<std::string> split_space_tokens(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) tokens.push_back(token);
  return tokens;
}

bool parse_int(const std::string& value, int* out) {
  if (!out) return false;
  try {
    const long long parsed = std::stoll(value);
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
      return false;
    }
    *out = static_cast<int>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool static_audio_payload_spec(uint8_t payload_type, RtpAudioPayloadSpec* out_spec) {
  if (!out_spec) return false;
  if (payload_type == 0) {
    out_spec->payload_type = 0;
    out_spec->codec_name = "PCMU";
    out_spec->sample_rate_hz = 8000;
    out_spec->channels = 1;
    return true;
  }
  if (payload_type == 8) {
    out_spec->payload_type = 8;
    out_spec->codec_name = "PCMA";
    out_spec->sample_rate_hz = 8000;
    out_spec->channels = 1;
    return true;
  }
  return false;
}

void upsert_payload_spec(
  std::vector<RtpAudioPayloadSpec>* specs,
  const RtpAudioPayloadSpec& candidate
) {
  if (!specs || candidate.payload_type < 0) return;
  for (auto& spec : *specs) {
    if (spec.payload_type != candidate.payload_type) continue;
    if (!candidate.codec_name.empty()) spec.codec_name = candidate.codec_name;
    if (candidate.sample_rate_hz > 0) spec.sample_rate_hz = candidate.sample_rate_hz;
    if (candidate.channels > 0) spec.channels = candidate.channels;
    return;
  }
  specs->push_back(candidate);
}

std::vector<RtpAudioPayloadSpec> parse_audio_payload_specs_from_sdp(const std::string& remote_sdp) {
  std::vector<RtpAudioPayloadSpec> specs;
  bool in_audio_section = false;
  for (const auto& raw_line : split_sdp_lines(remote_sdp)) {
    const std::string line = trim_copy(raw_line);
    if (starts_with(line, "m=")) {
      const auto tokens = split_space_tokens(line.substr(2));
      const bool is_audio = !tokens.empty() && upper_copy(tokens.front()) == "AUDIO";
      if (in_audio_section && !is_audio) break;
      in_audio_section = is_audio;
      if (!in_audio_section) continue;
      for (size_t i = 3; i < tokens.size(); ++i) {
        int payload_type = -1;
        if (!parse_int(tokens[i], &payload_type) || payload_type < 0 || payload_type > 127) {
          continue;
        }
        RtpAudioPayloadSpec spec;
        spec.payload_type = payload_type;
        static_audio_payload_spec(static_cast<uint8_t>(payload_type), &spec);
        upsert_payload_spec(&specs, spec);
      }
      continue;
    }
    if (!in_audio_section) continue;
    if (!starts_with(line, "a=rtpmap:")) continue;
    const std::string rest = line.substr(std::strlen("a=rtpmap:"));
    const size_t space = rest.find(' ');
    if (space == std::string::npos) continue;
    int payload_type = -1;
    if (!parse_int(trim_copy(rest.substr(0, space)), &payload_type) ||
        payload_type < 0 || payload_type > 127) {
      continue;
    }
    const std::string encoding = trim_copy(rest.substr(space + 1));
    const size_t slash_1 = encoding.find('/');
    if (slash_1 == std::string::npos) continue;
    const std::string codec_name = upper_copy(trim_copy(encoding.substr(0, slash_1)));
    const size_t slash_2 = encoding.find('/', slash_1 + 1);
    const std::string rate_token =
      slash_2 == std::string::npos
        ? encoding.substr(slash_1 + 1)
        : encoding.substr(slash_1 + 1, slash_2 - slash_1 - 1);
    int sample_rate_hz = 0;
    if (!parse_int(trim_copy(rate_token), &sample_rate_hz) || sample_rate_hz <= 0) continue;
    int channels = 1;
    if (slash_2 != std::string::npos) {
      int parsed_channels = 0;
      if (parse_int(trim_copy(encoding.substr(slash_2 + 1)), &parsed_channels) &&
          parsed_channels > 0) {
        channels = parsed_channels;
      }
    }

    RtpAudioPayloadSpec spec;
    spec.payload_type = payload_type;
    spec.codec_name = codec_name;
    spec.sample_rate_hz = sample_rate_hz;
    spec.channels = channels;
    upsert_payload_spec(&specs, spec);
  }

  if (specs.empty()) {
    RtpAudioPayloadSpec spec;
    (void)static_audio_payload_spec(0, &spec);
    upsert_payload_spec(&specs, spec);
    (void)static_audio_payload_spec(8, &spec);
    upsert_payload_spec(&specs, spec);
  }
  return specs;
}

int16_t decode_pcmu_sample(uint8_t value) {
  value = static_cast<uint8_t>(~value);
  int sample = ((value & 0x0F) << 3) + 0x84;
  sample <<= ((value & 0x70) >> 4);
  sample -= 0x84;
  return (value & 0x80) != 0 ? static_cast<int16_t>(sample) : static_cast<int16_t>(-sample);
}

int16_t decode_pcma_sample(uint8_t value) {
  value ^= 0x55;
  int sample = (value & 0x0F) << 4;
  const int segment = (value & 0x70) >> 4;
  switch (segment) {
    case 0:
      sample += 8;
      break;
    case 1:
      sample += 0x108;
      break;
    default:
      sample += 0x108;
      sample <<= (segment - 1);
      break;
  }
  return (value & 0x80) != 0 ? static_cast<int16_t>(sample) : static_cast<int16_t>(-sample);
}

bool decode_g711_frame(
  const RtpAudioPayloadSpec& spec,
  const unsigned char* payload,
  size_t payload_size,
  DecodedAudioFrame* out_frame,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!payload || payload_size == 0) {
    if (out_err) *out_err = "empty G.711 payload";
    return false;
  }
  if (!out_frame) {
    if (out_err) *out_err = "missing decoded frame output";
    return false;
  }
  out_frame->payload_type = spec.payload_type;
  out_frame->codec_name = spec.codec_name;
  out_frame->sample_rate_hz = spec.sample_rate_hz;
  out_frame->channels = spec.channels <= 0 ? 1 : spec.channels;
  out_frame->samples_per_channel = static_cast<int>(payload_size);
  out_frame->pcm_samples.resize(payload_size);
  for (size_t i = 0; i < payload_size; ++i) {
    out_frame->pcm_samples[i] =
      spec.codec_name == "PCMA"
        ? decode_pcma_sample(payload[i])
        : decode_pcmu_sample(payload[i]);
  }
  return true;
}

}  // namespace

#if defined(AGENTD_HAVE_OPUS)
struct InboundRtpAudioDecoder::OpusDecoderWrapper {
  OpusDecoder* decoder = nullptr;
  int sample_rate_hz = 0;
  int channels = 0;
};
#endif

InboundRtpAudioDecoder::InboundRtpAudioDecoder() = default;

InboundRtpAudioDecoder::~InboundRtpAudioDecoder() {
  reset();
}

void InboundRtpAudioDecoder::reset() {
  payload_specs_.clear();
#if defined(AGENTD_HAVE_OPUS)
  if (opus_decoder_) {
    if (opus_decoder_->decoder) {
      opus_decoder_destroy(opus_decoder_->decoder);
      opus_decoder_->decoder = nullptr;
    }
    delete opus_decoder_;
    opus_decoder_ = nullptr;
  }
#endif
}

bool InboundRtpAudioDecoder::configure_from_remote_sdp(
  const std::string& remote_sdp,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  payload_specs_ = parse_audio_payload_specs_from_sdp(remote_sdp);
#if defined(AGENTD_HAVE_OPUS)
  if (opus_decoder_) {
    if (opus_decoder_->decoder) {
      opus_decoder_destroy(opus_decoder_->decoder);
      opus_decoder_->decoder = nullptr;
    }
    delete opus_decoder_;
    opus_decoder_ = nullptr;
  }
#endif
  if (payload_specs_.empty()) {
    if (out_err) *out_err = "no audio payload specs found";
    return false;
  }
  return true;
}

const RtpAudioPayloadSpec* InboundRtpAudioDecoder::resolve_payload_spec(uint8_t payload_type) const {
  for (const auto& spec : payload_specs_) {
    if (spec.payload_type == static_cast<int>(payload_type)) return &spec;
  }
  static RtpAudioPayloadSpec static_spec;
  if (static_audio_payload_spec(payload_type, &static_spec)) return &static_spec;
  return nullptr;
}

bool InboundRtpAudioDecoder::decode_payload(
  uint8_t payload_type,
  const unsigned char* payload,
  size_t payload_size,
  DecodedAudioFrame* out_frame,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_frame) {
    if (out_err) *out_err = "missing decoded frame output";
    return false;
  }
  const RtpAudioPayloadSpec* spec = resolve_payload_spec(payload_type);
  if (!spec) {
    if (out_err) *out_err = "unsupported RTP audio payload type " + std::to_string(payload_type);
    return false;
  }

  if (spec->codec_name == "PCMU" || spec->codec_name == "PCMA") {
    return decode_g711_frame(*spec, payload, payload_size, out_frame, out_err);
  }

#if defined(AGENTD_HAVE_OPUS)
  if (spec->codec_name == "OPUS") {
    if (!payload || payload_size == 0) {
      if (out_err) *out_err = "empty Opus payload";
      return false;
    }
    const int sample_rate_hz = spec->sample_rate_hz > 0 ? spec->sample_rate_hz : 48000;
    const int channels = spec->channels > 0 ? spec->channels : 2;
    if (!opus_decoder_ ||
        opus_decoder_->sample_rate_hz != sample_rate_hz ||
        opus_decoder_->channels != channels) {
      if (opus_decoder_) {
        if (opus_decoder_->decoder) {
          opus_decoder_destroy(opus_decoder_->decoder);
          opus_decoder_->decoder = nullptr;
        }
        delete opus_decoder_;
      }
      opus_decoder_ = new OpusDecoderWrapper();
      int opus_err = OPUS_OK;
      opus_decoder_->decoder = opus_decoder_create(sample_rate_hz, channels, &opus_err);
      if (!opus_decoder_->decoder) {
        delete opus_decoder_;
        opus_decoder_ = nullptr;
        if (out_err) *out_err = std::string("opus decoder creation failed: ") + opus_strerror(opus_err);
        return false;
      }
      opus_decoder_->sample_rate_hz = sample_rate_hz;
      opus_decoder_->channels = channels;
    }

    const int max_samples_per_channel = sample_rate_hz * 120 / 1000;
    out_frame->pcm_samples.assign(static_cast<size_t>(max_samples_per_channel * channels), 0);
    const int decoded =
      opus_decode(
        opus_decoder_->decoder,
        payload,
        static_cast<opus_int32>(payload_size),
        out_frame->pcm_samples.data(),
        max_samples_per_channel,
        0);
    if (decoded < 0) {
      out_frame->pcm_samples.clear();
      if (out_err) *out_err = std::string("opus decode failed: ") + opus_strerror(decoded);
      return false;
    }
    out_frame->payload_type = spec->payload_type;
    out_frame->codec_name = spec->codec_name;
    out_frame->sample_rate_hz = sample_rate_hz;
    out_frame->channels = channels;
    out_frame->samples_per_channel = decoded;
    out_frame->pcm_samples.resize(static_cast<size_t>(decoded * channels));
    return true;
  }
#endif

  if (out_err) {
    *out_err = "unsupported RTP audio codec " + spec->codec_name +
               " for payload type " + std::to_string(spec->payload_type);
  }
  return false;
}

}  // namespace agentd
