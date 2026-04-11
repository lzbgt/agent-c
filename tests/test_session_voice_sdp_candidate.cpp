#include "session_voice_sdp_candidate.h"

#include <cassert>

namespace {

void test_candidate_normalization_preserves_sdp_attribute_shape() {
  const std::string normalized = agentd::normalize_sdp_candidate_line(
    " candidate:1 1 UDP 2122260223 127.0.0.1 55555 typ host ");
  assert(normalized == "a=candidate:1 1 udp 2122260223 127.0.0.1 55555 typ host");
}

void test_candidate_normalization_keeps_already_attributed_candidate() {
  const std::string normalized = agentd::normalize_sdp_candidate_line(
    "a=candidate:2 1 TCP 1518280447 127.0.0.1 55556 typ host tcptype active");
  assert(normalized == "a=candidate:2 1 tcp 1518280447 127.0.0.1 55556 typ host tcptype active");
}

void test_end_of_candidates_markers_are_canonicalized() {
  assert(agentd::sdp_candidate_is_end_marker(""));
  assert(agentd::sdp_candidate_is_end_marker("   "));
  assert(agentd::sdp_candidate_is_end_marker("end-of-candidates"));
  assert(agentd::sdp_candidate_is_end_marker("a=end-of-candidates\r\n"));
  assert(agentd::normalize_sdp_candidate_line("end-of-candidates") == "a=end-of-candidates");
  assert(agentd::normalize_sdp_candidate_line("a=end-of-candidates\r\n") == "a=end-of-candidates");
  assert(!agentd::sdp_candidate_is_end_marker("candidate:1 1 UDP 1 127.0.0.1 9 typ host"));
}

void test_malformed_end_marker_prefix_is_not_canonicalized() {
  assert(!agentd::sdp_candidate_is_end_marker("a=end-of-candidates:malformed"));
  assert(agentd::normalize_sdp_candidate_line("a=end-of-candidates:malformed") ==
         "a=end-of-candidates:malformed");
}

void test_relay_candidate_detection_requires_candidate_typ_relay() {
  assert(agentd::sdp_candidate_is_relay_candidate(
    "candidate:3 1 UDP 1677729535 203.0.113.2 60000 typ relay raddr 10.0.0.2 rport 50000"));
  assert(agentd::sdp_candidate_is_relay_candidate(
    "a=candidate:4 1 tcp 1518280447 203.0.113.3 9 TYP RELAY tcptype active"));
  assert(!agentd::sdp_candidate_is_relay_candidate(
    "candidate:relay 1 UDP 1677729535 203.0.113.2 60000 typ srflx raddr 10.0.0.2 rport 50000"));
  assert(!agentd::sdp_candidate_is_relay_candidate(
    "a=candidate:6 1 udp 1677729535 203.0.113.4 60001 typ prflx raddr 10.0.0.3 rport 50001"));
  assert(!agentd::sdp_candidate_is_relay_candidate(
    "a=candidate:5 1 udp 2113937151 127.0.0.1 49999 typ host"));
  assert(!agentd::sdp_candidate_is_relay_candidate("a=end-of-candidates"));
  assert(!agentd::sdp_candidate_is_relay_candidate("a=end-of-candidates:relay"));
}

void test_relay_candidate_lines_are_stripped_from_sdp() {
  const std::string sdp =
    "v=0\r\n"
    "a=candidate:1 1 UDP 2113937151 127.0.0.1 49999 typ host\r\n"
    "a=candidate:2 1 UDP 1677729535 203.0.113.2 60000 typ relay raddr 10.0.0.2 rport 50000\r\n"
    "a=candidate:relay 1 UDP 1677729535 203.0.113.3 60001 typ srflx raddr 10.0.0.3 rport 50001\r\n"
    "a=candidate:6 1 UDP 1677729535 203.0.113.4 60002 typ prflx raddr 10.0.0.4 rport 50002\r\n"
    "a=end-of-candidates\r\n";
  const std::string stripped = agentd::strip_sdp_relay_candidate_lines(sdp);
  assert(stripped.find("typ relay") == std::string::npos);
  assert(stripped.find("typ host") != std::string::npos);
  assert(stripped.find("typ srflx") != std::string::npos);
  assert(stripped.find("typ prflx") != std::string::npos);
  assert(stripped.find("a=end-of-candidates") != std::string::npos);
}

}  // namespace

int main() {
  test_candidate_normalization_preserves_sdp_attribute_shape();
  test_candidate_normalization_keeps_already_attributed_candidate();
  test_end_of_candidates_markers_are_canonicalized();
  test_malformed_end_marker_prefix_is_not_canonicalized();
  test_relay_candidate_detection_requires_candidate_typ_relay();
  test_relay_candidate_lines_are_stripped_from_sdp();
  return 0;
}
