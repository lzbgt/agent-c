#pragma once

#include "session_voice_signal_client.h"

#include <string>

namespace agentd {

struct VoiceBrokerSignalAnswerExchangeOptions {
  std::string self_sender_tag;
  int64_t remote_description_timeout_ms = 15000;
  int64_t post_answer_candidate_timeout_ms = 0;
  int64_t remote_bye_timeout_ms = 0;
  VoiceBrokerSignalDescription local_answer;
  VoiceBrokerSignalBye local_bye;
};

struct VoiceBrokerSignalAnswerExchangeResult {
  VoiceBrokerSignalRemoteDescriptionReady remote_ready;
  bool post_answer_remote_candidate_received = false;
  VoiceBrokerSignalCandidate post_answer_remote_candidate;
  bool remote_bye_received = false;
  VoiceBrokerSignalBye remote_bye;
};

bool run_voice_broker_signal_answer_exchange(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalAnswerExchangeOptions& options,
  VoiceBrokerSignalAnswerExchangeResult* out_result,
  long* out_http_status,
  std::string* out_err
);

}  // namespace agentd
