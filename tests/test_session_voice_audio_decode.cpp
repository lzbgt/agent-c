#include "session_voice_audio_decode.h"

#if defined(AGENTD_HAVE_OPUS)
#include <opus.h>
#endif

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string make_browser_style_offer_sdp() {
  return
    "v=0\r\n"
    "o=- 4962323985234234 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n";
}

std::string make_answer_with_rejected_extra_audio_sdp() {
  return
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=sendrecv\r\n"
    "m=audio 0 UDP/TLS/RTP/SAVPF 111 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=inactive\r\n";
}

void test_decoder_parses_audio_payload_specs_from_sdp() {
  agentd::InboundRtpAudioDecoder decoder;
  std::string err;
  assert(decoder.configure_from_remote_sdp(make_browser_style_offer_sdp(), &err));
  assert(err.empty());

  const agentd::RtpAudioPayloadSpec* opus = decoder.resolve_payload_spec(111);
  assert(opus);
  assert(opus->codec_name == "OPUS");
  assert(opus->sample_rate_hz == 48000);
  assert(opus->channels == 2);

  const agentd::RtpAudioPayloadSpec* pcmu = decoder.resolve_payload_spec(0);
  assert(pcmu);
  assert(pcmu->codec_name == "PCMU");
  assert(pcmu->sample_rate_hz == 8000);
  assert(pcmu->channels == 1);

  const agentd::RtpAudioPayloadSpec* pcma = decoder.resolve_payload_spec(8);
  assert(pcma);
  assert(pcma->codec_name == "PCMA");
  assert(pcma->sample_rate_hz == 8000);
  assert(pcma->channels == 1);
}

void test_first_active_audio_payload_parser_ignores_rejected_sections() {
  const std::vector<agentd::RtpAudioPayloadSpec> specs =
    agentd::parse_first_active_audio_payload_specs_from_sdp(
      make_answer_with_rejected_extra_audio_sdp());
  assert(specs.size() == 1);
  assert(specs[0].payload_type == 0);
  assert(specs[0].codec_name == "PCMU");
  assert(specs[0].sample_rate_hz == 8000);
  assert(specs[0].channels == 1);
}

void test_first_active_audio_payload_parser_returns_empty_when_all_audio_is_rejected() {
  const std::vector<agentd::RtpAudioPayloadSpec> specs =
    agentd::parse_first_active_audio_payload_specs_from_sdp(
      "v=0\r\n"
      "o=- 0 0 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=audio 0 UDP/TLS/RTP/SAVPF 111\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=inactive\r\n");
  assert(specs.empty());
}

void test_decoder_decodes_pcmu_payload_to_pcm() {
  agentd::InboundRtpAudioDecoder decoder;
  std::string err;
  const unsigned char payload[] = {0xFFu, 0xFEu, 0x7Eu, 0x00u};
  agentd::DecodedAudioFrame frame;
  assert(decoder.decode_payload(0, payload, sizeof(payload), &frame, &err));
  assert(err.empty());
  assert(frame.payload_type == 0);
  assert(frame.codec_name == "PCMU");
  assert(frame.sample_rate_hz == 8000);
  assert(frame.channels == 1);
  assert(frame.samples_per_channel == 4);
  assert(frame.pcm_samples.size() == 4);
  assert(frame.pcm_samples[0] == 0);
  assert(frame.pcm_samples[1] < 0);
  assert(frame.pcm_samples[2] > 0);
  assert(frame.pcm_samples[3] > frame.pcm_samples[2]);
}

void test_decoder_decodes_pcma_payload_to_pcm() {
  agentd::InboundRtpAudioDecoder decoder;
  std::string err;
  const unsigned char payload[] = {0xD5u, 0x55u, 0x00u, 0x80u};
  agentd::DecodedAudioFrame frame;
  assert(decoder.decode_payload(8, payload, sizeof(payload), &frame, &err));
  assert(err.empty());
  assert(frame.payload_type == 8);
  assert(frame.codec_name == "PCMA");
  assert(frame.sample_rate_hz == 8000);
  assert(frame.channels == 1);
  assert(frame.samples_per_channel == 4);
  assert(frame.pcm_samples.size() == 4);
  assert(frame.pcm_samples[0] > 0);
  assert(frame.pcm_samples[1] < 0);
}

#if defined(AGENTD_HAVE_OPUS)
void test_decoder_decodes_opus_payload_to_pcm() {
  agentd::InboundRtpAudioDecoder decoder;
  std::string err;
  assert(decoder.configure_from_remote_sdp(
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "a=rtpmap:111 opus/48000/1\r\n",
    &err));
  assert(err.empty());

  int opus_err = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &opus_err);
  assert(encoder);
  assert(opus_err == OPUS_OK);

  std::vector<opus_int16> pcm(960, 0);
  for (size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<opus_int16>((static_cast<int>(i % 32) - 16) * 256);
  }
  unsigned char encoded[4000];
  const int encoded_bytes = opus_encode(encoder, pcm.data(), 960, encoded, sizeof(encoded));
  assert(encoded_bytes > 0);
  opus_encoder_destroy(encoder);

  agentd::DecodedAudioFrame frame;
  assert(decoder.decode_payload(111, encoded, static_cast<size_t>(encoded_bytes), &frame, &err));
  assert(err.empty());
  assert(frame.payload_type == 111);
  assert(frame.codec_name == "OPUS");
  assert(frame.sample_rate_hz == 48000);
  assert(frame.channels == 1);
  assert(frame.samples_per_channel == 960);
  assert(frame.pcm_samples.size() == 960);
}
#endif

}  // namespace

int main() {
  test_decoder_parses_audio_payload_specs_from_sdp();
  test_first_active_audio_payload_parser_ignores_rejected_sections();
  test_first_active_audio_payload_parser_returns_empty_when_all_audio_is_rejected();
  test_decoder_decodes_pcmu_payload_to_pcm();
  test_decoder_decodes_pcma_payload_to_pcm();
#if defined(AGENTD_HAVE_OPUS)
  test_decoder_decodes_opus_payload_to_pcm();
#endif
  return 0;
}
