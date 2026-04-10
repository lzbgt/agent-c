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

void test_inactive_answer_mirrors_browser_offer_media_contract() {
  const std::string answer = agentd::build_builtin_inactive_answer_sdp({
    browser_offer(),
    local_description(),
    true,
    "passive",
    "AA:BB:CC",
  });

  assert(answer.find("a=group:BUNDLE 0\r\n") != std::string::npos);
  assert(answer.find("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n") != std::string::npos);
  assert(answer.find("a=mid:0\r\n") != std::string::npos);
  assert(answer.find("a=rtcp-mux\r\n") != std::string::npos);
  assert(answer.find("a=rtcp-rsize\r\n") != std::string::npos);
  assert(answer.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos);
  assert(answer.find("a=fmtp:111 minptime=10;useinbandfec=1\r\n") != std::string::npos);
  assert(answer.find("a=inactive\r\n") != std::string::npos);
  assert(answer.find("a=sendrecv\r\n") == std::string::npos);
  assert(answer.find("a=setup:passive\r\n") != std::string::npos);
  assert(answer.find("a=fingerprint:sha-256 AA:BB:CC\r\n") != std::string::npos);
  assert(answer.find("a=ice-ufrag:localUfrag\r\n") != std::string::npos);
  assert(answer.find("a=candidate:1 1 UDP") != std::string::npos);
  assert(answer.find("a=end-of-candidates\r\n") != std::string::npos);
}

void test_falls_back_to_local_description_without_media_or_dtls_identity() {
  const std::string fallback = local_description();
  assert(agentd::build_builtin_inactive_answer_sdp({
    "v=0\r\ns=-\r\n",
    fallback,
    true,
    "passive",
    "AA:BB:CC",
  }) == fallback);
  assert(agentd::build_builtin_inactive_answer_sdp({
    browser_offer(),
    fallback,
    false,
    "passive",
    "AA:BB:CC",
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
  test_inactive_answer_mirrors_browser_offer_media_contract();
  test_falls_back_to_local_description_without_media_or_dtls_identity();
  test_sdp_marker_helpers_trim_inputs();
  return 0;
}
