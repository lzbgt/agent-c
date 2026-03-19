#include "session_voice_signal_client.h"
#include "session_voice_signal_protocol.h"
#include "string_util.h"

#include <iostream>
#include <string>

using namespace agentd;

struct Options {
  std::string broker_url;
  std::string token;
  std::string session_id;
  int64_t stream_timeout_ms = 15000;
  int64_t post_answer_candidate_timeout_ms = 0;
  std::string send_bye_reason;
};

static void print_usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_audio_signal_loopback")
    << " --broker-url <url> --token <token> --session-id <id>"
    << " [--stream-timeout-ms <ms>] [--post-answer-candidate-timeout-ms <ms>]"
    << " [--send-bye-reason <reason>]\n";
}

static bool parse_args(int argc, char** argv, Options* out) {
  if (!out) return false;
  Options opt;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--broker-url" && i + 1 < argc) {
      opt.broker_url = argv[++i];
    } else if (a == "--token" && i + 1 < argc) {
      opt.token = argv[++i];
    } else if (a == "--session-id" && i + 1 < argc) {
      opt.session_id = argv[++i];
    } else if (a == "--stream-timeout-ms" && i + 1 < argc) {
      opt.stream_timeout_ms = std::stoll(argv[++i]);
    } else if (a == "--post-answer-candidate-timeout-ms" && i + 1 < argc) {
      opt.post_answer_candidate_timeout_ms = std::stoll(argv[++i]);
    } else if (a == "--send-bye-reason" && i + 1 < argc) {
      opt.send_bye_reason = argv[++i];
    } else if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      return false;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      print_usage(argv[0]);
      return false;
    }
  }
  if (opt.broker_url.empty() || opt.token.empty() || opt.session_id.empty()) {
    print_usage(argv[0]);
    return false;
  }
  *out = opt;
  return true;
}

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, &opt)) return 2;

  long http_status = 0;
  std::string err;
  VoiceBrokerSignalSessionState signal_state("");
  VoiceBrokerSignalRemoteDescriptionReady remote_ready;
  if (!wait_for_voice_broker_signal_remote_description_ready_with_state(
        opt.broker_url,
        opt.token,
        opt.session_id,
        opt.stream_timeout_ms,
        &signal_state,
        &remote_ready,
        &http_status,
        &err)) {
    std::cerr << "Failed to read offer";
    if (http_status > 0) std::cerr << " (http status " << http_status << ")";
    if (!err.empty()) std::cerr << ": " << err;
    std::cerr << "\n";
    return 1;
  }
  const std::string offer_type = lower_copy(trim_copy(remote_ready.description.type));
  if (!offer_type.empty() && offer_type != "offer") {
    std::cerr << "Failed to read offer: expected remote offer, got " << offer_type << "\n";
    return 1;
  }

  VoiceBrokerSignalDescription answer;
  answer.type = "answer";
  answer.sdp = "stub-answer";
  if (!send_voice_broker_answer(opt.broker_url, opt.token, opt.session_id, answer, &err)) {
    std::cerr << "Failed to send answer: " << err << "\n";
    return 1;
  }

  size_t post_answer_remote_candidate_count = 0;
  if (opt.post_answer_candidate_timeout_ms > 0) {
    VoiceBrokerSignalCandidate candidate;
    if (!wait_for_voice_broker_signal_remote_candidate_ready(
          opt.broker_url,
          opt.token,
          opt.session_id,
          opt.post_answer_candidate_timeout_ms,
          &signal_state,
          &candidate,
          &http_status,
          &err)) {
      std::cerr << "Failed to read post-answer candidate";
      if (http_status > 0) std::cerr << " (http status " << http_status << ")";
      if (!err.empty()) std::cerr << ": " << err;
      std::cerr << "\n";
      return 1;
    }
    if (!trim_copy(candidate.candidate).empty()) post_answer_remote_candidate_count = 1;
  }

  VoiceBrokerSignalBye bye;
  bye.reason = opt.send_bye_reason;
  if (!bye.reason.empty() && !send_voice_broker_bye(opt.broker_url, opt.token, opt.session_id, bye, &err)) {
    std::cerr << "Failed to send bye: " << err << "\n";
    return 1;
  }

  std::cout
    << "agentd_audio_signal_loopback OK initial_remote_candidate_count="
    << remote_ready.initial_remote_candidates.size()
    << " post_answer_remote_candidate_count="
    << post_answer_remote_candidate_count << "\n";
  return 0;
}
