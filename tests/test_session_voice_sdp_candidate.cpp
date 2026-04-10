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

}  // namespace

int main() {
  test_candidate_normalization_preserves_sdp_attribute_shape();
  test_candidate_normalization_keeps_already_attributed_candidate();
  test_end_of_candidates_markers_are_canonicalized();
  return 0;
}
