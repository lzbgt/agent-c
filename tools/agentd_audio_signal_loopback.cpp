#include "session_voice_signal_negotiation.h"

#include <iostream>
#include <string>

using namespace agentd;

struct Options {
  std::string broker_url;
  std::string token;
  std::string session_id;
  int64_t stream_timeout_ms = 15000;
  int64_t post_answer_candidate_timeout_ms = 0;
  int64_t remote_bye_timeout_ms = 0;
  std::string send_bye_reason;
};

static void print_usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_audio_signal_loopback")
    << " --broker-url <url> --token <token> --session-id <id>"
    << " [--stream-timeout-ms <ms>] [--post-answer-candidate-timeout-ms <ms>]"
    << " [--remote-bye-timeout-ms <ms>]"
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
    } else if (a == "--remote-bye-timeout-ms" && i + 1 < argc) {
      opt.remote_bye_timeout_ms = std::stoll(argv[++i]);
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

  VoiceBrokerSignalAnswerExchangeOptions exchange_options;
  exchange_options.remote_description_timeout_ms = opt.stream_timeout_ms;
  exchange_options.post_answer_candidate_timeout_ms = opt.post_answer_candidate_timeout_ms;
  exchange_options.remote_bye_timeout_ms = opt.remote_bye_timeout_ms;
  exchange_options.local_answer.type = "answer";
  exchange_options.local_answer.sdp = "stub-answer";
  exchange_options.local_bye.reason = opt.send_bye_reason;

  long http_status = 0;
  std::string err;
  VoiceBrokerSignalAnswerExchangeResult exchange_result;
  if (!run_voice_broker_signal_answer_exchange(
        opt.broker_url,
        opt.token,
        opt.session_id,
        exchange_options,
        &exchange_result,
        &http_status,
        &err)) {
    std::cerr << "Failed to run answer exchange";
    if (http_status > 0) std::cerr << " (http status " << http_status << ")";
    if (!err.empty()) std::cerr << ": " << err;
    std::cerr << "\n";
    return 1;
  }

  std::cout
    << "agentd_audio_signal_loopback OK initial_remote_candidate_count="
    << exchange_result.remote_ready.initial_remote_candidates.size()
    << " post_answer_remote_candidate_count="
    << (exchange_result.post_answer_remote_candidate_received ? 1 : 0);
  if (exchange_result.remote_bye_received) {
    std::cout << " remote_bye_reason=" << exchange_result.remote_bye.reason;
  }
  std::cout << "\n";
  return 0;
}
