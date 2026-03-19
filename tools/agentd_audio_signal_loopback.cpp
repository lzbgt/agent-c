#include "session_voice_signal_client.h"
#include "session_voice_signal_protocol.h"

#include <iostream>
#include <string>

using namespace agentd;

struct Options {
  std::string broker_url;
  std::string token;
  std::string session_id;
  int64_t stream_timeout_ms = 15000;
  std::string send_bye_reason;
};

static void print_usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_audio_signal_loopback")
    << " --broker-url <url> --token <token> --session-id <id>"
    << " [--stream-timeout-ms <ms>] [--send-bye-reason <reason>]\n";
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

  VoiceBrokerSignalEvent offer_event;
  long http_status = 0;
  std::string err;
  if (!wait_for_voice_broker_signal_type(
        opt.broker_url,
        opt.token,
        opt.session_id,
        "offer",
        opt.stream_timeout_ms,
        &offer_event,
        &http_status,
        &err)) {
    std::cerr << "Failed to read offer";
    if (http_status > 0) std::cerr << " (http status " << http_status << ")";
    if (!err.empty()) std::cerr << ": " << err;
    std::cerr << "\n";
    return 1;
  }

  VoiceBrokerSignalDescription offer;
  if (!parse_voice_broker_signal_description_payload(offer_event.payload, &offer, &err)) {
    std::cerr << "Failed to parse offer payload: " << err << "\n";
    return 1;
  }

  VoiceBrokerSignalDescription answer;
  answer.type = "answer";
  answer.sdp = "stub-answer";
  if (!send_voice_broker_answer(opt.broker_url, opt.token, opt.session_id, answer, &err)) {
    std::cerr << "Failed to send answer: " << err << "\n";
    return 1;
  }

  VoiceBrokerSignalBye bye;
  bye.reason = opt.send_bye_reason;
  if (!bye.reason.empty() && !send_voice_broker_bye(opt.broker_url, opt.token, opt.session_id, bye, &err)) {
    std::cerr << "Failed to send bye: " << err << "\n";
    return 1;
  }

  std::cout << "agentd_audio_signal_loopback OK\n";
  return 0;
}
