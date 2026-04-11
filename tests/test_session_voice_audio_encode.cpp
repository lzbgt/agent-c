#include "session_voice_audio_encode.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<agentd::RtpAudioPayloadSpec> browser_payload_specs() {
  return {
    agentd::RtpAudioPayloadSpec{0, "PCMU", 8000, 1},
    agentd::RtpAudioPayloadSpec{8, "PCMA", 8000, 1},
    agentd::RtpAudioPayloadSpec{111, "OPUS", 48000, 2},
  };
}

std::vector<int16_t> make_pcm(int channels, int frames) {
  std::vector<int16_t> pcm(static_cast<size_t>(frames * channels), 0);
  for (size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<int16_t>((static_cast<int>(i % 64) - 32) * 256);
  }
  return pcm;
}

void test_encoder_prefers_opus_when_available_else_pcmu() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  assert(encoder.select_payload(browser_payload_specs(), &err));
  assert(err.empty());
  const agentd::RtpAudioPayloadSpec* selected = encoder.selected_payload();
  assert(selected);
#if defined(AGENTD_HAVE_OPUS)
  assert(selected->codec_name == "OPUS");
  assert(selected->payload_type == 111);
  assert(selected->sample_rate_hz == 48000);
  assert(selected->channels == 2);
#else
  assert(selected->codec_name == "PCMU");
  assert(selected->payload_type == 0);
  assert(selected->sample_rate_hz == 8000);
  assert(selected->channels == 1);
#endif
}

void test_encoder_falls_back_to_pcma() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{8, "PCMA", 8000, 1},
  };
  assert(encoder.select_payload(specs, &err));
  assert(err.empty());
  const agentd::RtpAudioPayloadSpec* selected = encoder.selected_payload();
  assert(selected);
  assert(selected->codec_name == "PCMA");
  assert(selected->payload_type == 8);
}

void test_encoder_normalizes_codec_name_case() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{0, "pcmu", 8000, 1},
    agentd::RtpAudioPayloadSpec{8, "pcma", 8000, 1},
    agentd::RtpAudioPayloadSpec{111, "opus", 48000, 2},
  };
  assert(encoder.select_payload(specs, &err));
  assert(err.empty());
  const agentd::RtpAudioPayloadSpec* selected = encoder.selected_payload();
  assert(selected);
#if defined(AGENTD_HAVE_OPUS)
  assert(selected->codec_name == "OPUS");
  assert(selected->payload_type == 111);
#else
  assert(selected->codec_name == "PCMU");
  assert(selected->payload_type == 0);
#endif
}

void test_encoder_skips_unsupported_opus_variants_before_g711() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{111, "OPUS", 48000, 6},
    agentd::RtpAudioPayloadSpec{112, "OPUS", 16000, 1},
    agentd::RtpAudioPayloadSpec{0, "PCMU", 8000, 1},
    agentd::RtpAudioPayloadSpec{8, "PCMA", 8000, 1},
  };
  assert(encoder.select_payload(specs, &err));
  assert(err.empty());
  const agentd::RtpAudioPayloadSpec* selected = encoder.selected_payload();
  assert(selected);
  assert(selected->codec_name == "PCMU");
  assert(selected->payload_type == 0);
}

void test_encoder_rejects_unsupported_payloads() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{9, "G722", 8000, 1},
  };
  assert(!encoder.select_payload(specs, &err));
  assert(err.find("supported outbound audio payload") != std::string::npos);
  assert(!encoder.selected_payload());
}

void test_encoder_encodes_pcmu_20ms() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{0, "PCMU", 8000, 1},
  };
  assert(encoder.select_payload(specs, &err));
  const std::vector<int16_t> pcm = make_pcm(2, 960);
  agentd::EncodedRtpAudioPayload20ms encoded;
  assert(encoder.encode_20ms(pcm.data(), pcm.size(), 48000, 2, &encoded, &err));
  assert(err.empty());
  assert(encoded.payload.size() == 160);
  assert(encoded.timestamp_increment == 160);
}

void test_encoder_encodes_pcma_20ms() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{8, "PCMA", 8000, 1},
  };
  assert(encoder.select_payload(specs, &err));
  const std::vector<int16_t> pcm = make_pcm(1, 160);
  agentd::EncodedRtpAudioPayload20ms encoded;
  assert(encoder.encode_20ms(pcm.data(), pcm.size(), 8000, 1, &encoded, &err));
  assert(err.empty());
  assert(encoded.payload.size() == 160);
  assert(encoded.timestamp_increment == 160);
}

#if defined(AGENTD_HAVE_OPUS)
void test_encoder_encodes_opus_20ms() {
  agentd::OutboundRtpAudioEncoder encoder;
  std::string err;
  const std::vector<agentd::RtpAudioPayloadSpec> specs = {
    agentd::RtpAudioPayloadSpec{111, "OPUS", 48000, 2},
  };
  assert(encoder.select_payload(specs, &err));
  const std::vector<int16_t> pcm = make_pcm(2, 960);
  agentd::EncodedRtpAudioPayload20ms encoded;
  assert(encoder.encode_20ms(pcm.data(), pcm.size(), 48000, 2, &encoded, &err));
  assert(err.empty());
  assert(!encoded.payload.empty());
  assert(encoded.payload.size() <= 1275);
  assert(encoded.timestamp_increment == 960);
}
#endif

}  // namespace

int main() {
  test_encoder_prefers_opus_when_available_else_pcmu();
  test_encoder_falls_back_to_pcma();
  test_encoder_normalizes_codec_name_case();
  test_encoder_skips_unsupported_opus_variants_before_g711();
  test_encoder_rejects_unsupported_payloads();
  test_encoder_encodes_pcmu_20ms();
  test_encoder_encodes_pcma_20ms();
#if defined(AGENTD_HAVE_OPUS)
  test_encoder_encodes_opus_20ms();
#endif
  return 0;
}
