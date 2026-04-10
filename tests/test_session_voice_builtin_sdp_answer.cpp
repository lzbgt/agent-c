#include "session_voice_builtin_sdp_answer.h"

#include <cassert>
#include <string>

namespace {

std::string browser_offer() {
  return
    "v=0\r\n"
    "o=- 1 2 IN IP4 127.0.0.1\r\n"
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
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n";
}

std::string local_description() {
  return
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=ice-ufrag:localUfrag\r\n"
    "a=ice-pwd:localPassword\r\n"
    "a=ice-options:trickle\r\n"
    "a=candidate:1 1 UDP 2122260223 127.0.0.1 55555 typ host\r\n"
    "a=end-of-candidates\r\n";
}

std::string local_description_with_bare_candidate() {
  return
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=ice-ufrag:localUfrag\r\n"
    "a=ice-pwd:localPassword\r\n"
    "candidate:2 1 TCP 1518280447 127.0.0.1 55556 typ host tcptype active\r\n";
}

std::string offer_with_direction(const char* direction) {
  std::string offer = browser_offer();
  const std::string needle = "a=sendrecv\r\n";
  const size_t pos = offer.find(needle);
  assert(pos != std::string::npos);
  offer.replace(pos, needle.size(), std::string("a=") + direction + "\r\n");
  return offer;
}

std::string offer_with_audio_and_data_mline() {
  std::string offer = browser_offer();
  offer +=
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n";
  return offer;
}

std::string offer_with_telephone_event() {
  std::string offer = browser_offer();
  const std::string needle = "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n";
  const size_t pos = offer.find(needle);
  assert(pos != std::string::npos);
  offer.replace(pos, needle.size(), "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 101\r\n");
  offer += "a=rtpmap:101 telephone-event/8000\r\n";
  return offer;
}

std::string unsupported_audio_offer() {
  return
    "v=0\r\n"
    "o=- 1 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 13\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:13 CN/8000\r\n";
}

std::string expected_answer_audio_mline() {
#if defined(AGENTD_HAVE_OPUS)
  return "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n";
#else
  return "m=audio 9 UDP/TLS/RTP/SAVPF 0 8\r\n";
#endif
}

size_t count_occurrences(const std::string& value, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    count += 1;
    pos += needle.size();
  }
  return count;
}

void test_active_answer_mirrors_browser_offer_media_contract() {
  const std::string answer = agentd::build_builtin_active_answer_sdp({
    browser_offer(),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  });

  assert(answer.find("a=group:BUNDLE 0\r\n") != std::string::npos);
  assert(answer.find(expected_answer_audio_mline()) != std::string::npos);
  assert(answer.find("a=mid:0\r\n") != std::string::npos);
  assert(answer.find("a=rtcp-mux\r\n") != std::string::npos);
  assert(answer.find("a=rtcp-rsize\r\n") != std::string::npos);
#if defined(AGENTD_HAVE_OPUS)
  assert(answer.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos);
  assert(answer.find("a=fmtp:111 minptime=10;useinbandfec=1\r\n") != std::string::npos);
#else
  assert(answer.find("a=rtpmap:111 opus/48000/2\r\n") == std::string::npos);
  assert(answer.find("a=fmtp:111 minptime=10;useinbandfec=1\r\n") == std::string::npos);
#endif
  assert(answer.find("a=sendrecv\r\n") != std::string::npos);
  assert(answer.find("a=inactive\r\n") == std::string::npos);
  assert(answer.find("a=setup:passive\r\n") != std::string::npos);
  assert(answer.find("a=fingerprint:sha-256 AA:BB:CC\r\n") != std::string::npos);
  assert(answer.find("a=ice-ufrag:localUfrag\r\n") != std::string::npos);
  assert(answer.find("a=ice-options:trickle\r\n") != std::string::npos);
  assert(answer.find("a=candidate:1 1 udp") != std::string::npos);
  assert(answer.find("a=candidate:1 1 UDP") == std::string::npos);
  assert(answer.find("a=ice-options:ice2") == std::string::npos);
  assert(answer.find("a=end-of-candidates\r\n") == std::string::npos);
  assert(answer.find("a=msid:agentd_builtin_stream agentd_builtin_audio\r\n") != std::string::npos);
  assert(answer.find("a=ssrc:2799795457 cname:agentd-builtin-native\r\n") != std::string::npos);
  assert(answer.find("a=ssrc:2799795457 msid:agentd_builtin_stream agentd_builtin_audio\r\n") != std::string::npos);
}

void test_active_answer_prunes_unsupported_audio_payloads() {
  const std::string answer = agentd::build_builtin_active_answer_sdp({
    offer_with_telephone_event(),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  });

  assert(answer.find(expected_answer_audio_mline()) != std::string::npos);
  assert(answer.find(" 101\r\n") == std::string::npos);
  assert(answer.find("a=rtpmap:101 telephone-event/8000\r\n") == std::string::npos);
}

void test_active_answer_rejects_audio_mline_without_supported_payloads() {
  const std::string answer = agentd::build_builtin_active_answer_sdp({
    unsupported_audio_offer(),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  });

  assert(answer.find("m=audio 0 UDP/TLS/RTP/SAVPF 13\r\n") != std::string::npos);
  assert(answer.find("a=inactive\r\n") != std::string::npos);
  assert(answer.find("a=rtpmap:13 CN/8000\r\n") == std::string::npos);
  assert(answer.find("a=ssrc:2799795457 ") == std::string::npos);
}

void test_active_answer_normalizes_bare_local_candidate_lines() {
  const std::string answer = agentd::build_builtin_active_answer_sdp({
    browser_offer(),
    local_description_with_bare_candidate(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  });

  assert(answer.find("a=candidate:2 1 tcp") != std::string::npos);
  assert(answer.find("\r\ncandidate:2 ") == std::string::npos);
  assert(answer.find("a=candidate:2 1 TCP") == std::string::npos);
}

void test_active_answer_keeps_audio_ssrc_on_audio_mline_only() {
  const std::string answer = agentd::build_builtin_active_answer_sdp({
    offer_with_audio_and_data_mline(),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  });

  assert(answer.find(expected_answer_audio_mline()) != std::string::npos);
  assert(answer.find("m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n") != std::string::npos);
  assert(count_occurrences(answer, "a=ssrc:2799795457 ") == 2);
  const size_t app_pos = answer.find("m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n");
  assert(app_pos != std::string::npos);
  assert(answer.find("a=ssrc:2799795457 ", app_pos) == std::string::npos);
}

void test_active_answer_respects_offer_direction() {
  assert(agentd::build_builtin_active_answer_sdp({
    offer_with_direction("sendonly"),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  }).find("a=recvonly\r\n") != std::string::npos);
  assert(agentd::build_builtin_active_answer_sdp({
    offer_with_direction("recvonly"),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  }).find("a=sendonly\r\n") != std::string::npos);
  assert(agentd::build_builtin_active_answer_sdp({
    offer_with_direction("inactive"),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
    2799795457u,
    "agentd-builtin-native",
    "agentd_builtin_stream",
    "agentd_builtin_audio",
  }).find("a=inactive\r\n") != std::string::npos);
}

void test_falls_back_to_local_description_without_media_or_dtls_identity() {
  const std::string fallback = local_description();
  assert(agentd::build_builtin_active_answer_sdp({
    "v=0\r\ns=-\r\n",
    fallback,
    true,
    "passive",
    "AA:BB:CC",
    0u,
    "",
    "",
    "",
  }) == fallback);
  assert(agentd::build_builtin_active_answer_sdp({
    browser_offer(),
    fallback,
    false,
    "passive",
    "AA:BB:CC",
    0u,
    "",
    "",
    "",
  }) == fallback);
}

void test_sdp_marker_helpers_trim_inputs() {
  assert(agentd::sdp_contains_media_section(browser_offer()));
  assert(!agentd::sdp_contains_media_section("v=0\r\ns=-\r\n"));
  assert(agentd::sdp_is_end_of_candidates_marker("a=end-of-candidates\r\n"));
  assert(agentd::sdp_is_end_of_candidates_marker(" end-of-candidates "));
  assert(!agentd::sdp_is_end_of_candidates_marker("a=candidate:1 1 UDP 1 127.0.0.1 9 typ host"));
}

}  // namespace

int main() {
  test_active_answer_mirrors_browser_offer_media_contract();
  test_active_answer_prunes_unsupported_audio_payloads();
  test_active_answer_rejects_audio_mline_without_supported_payloads();
  test_active_answer_normalizes_bare_local_candidate_lines();
  test_active_answer_keeps_audio_ssrc_on_audio_mline_only();
  test_active_answer_respects_offer_direction();
  test_falls_back_to_local_description_without_media_or_dtls_identity();
  test_sdp_marker_helpers_trim_inputs();
  return 0;
}
