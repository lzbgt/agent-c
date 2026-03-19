#pragma once

#include "session_voice_signal_client.h"

#include <functional>
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

struct VoiceBrokerSignalAnswerExchangeOps {
  std::function<bool(
    const std::string& broker_url,
    const std::string& token,
    const std::string& session_id,
    int64_t timeout_ms,
    VoiceBrokerSignalSessionState* io_state,
    VoiceBrokerSignalRemoteDescriptionReady* out_ready,
    long* out_http_status,
    std::string* out_err)>
    wait_remote_description_ready_with_state;

  std::function<bool(
    const std::string& broker_url,
    const std::string& token,
    const std::string& session_id,
    const VoiceBrokerSignalDescription& answer,
    std::string* out_err)>
    send_answer;

  std::function<bool(
    const std::string& broker_url,
    const std::string& token,
    const std::string& session_id,
    int64_t timeout_ms,
    VoiceBrokerSignalSessionState* io_state,
    VoiceBrokerSignalCandidate* out_candidate,
    long* out_http_status,
    std::string* out_err)>
    wait_remote_candidate_ready;

  std::function<bool(
    const std::string& broker_url,
    const std::string& token,
    const std::string& session_id,
    int64_t timeout_ms,
    VoiceBrokerSignalSessionState* io_state,
    VoiceBrokerSignalBye* out_bye,
    long* out_http_status,
    std::string* out_err)>
    wait_remote_bye;

  std::function<bool(
    const std::string& broker_url,
    const std::string& token,
    const std::string& session_id,
    const VoiceBrokerSignalBye& bye,
    std::string* out_err)>
    send_bye;
};

VoiceBrokerSignalAnswerExchangeOps default_voice_broker_signal_answer_exchange_ops();

bool run_voice_broker_signal_answer_exchange_with_ops(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalAnswerExchangeOptions& options,
  const VoiceBrokerSignalAnswerExchangeOps& ops,
  VoiceBrokerSignalAnswerExchangeResult* out_result,
  long* out_http_status,
  std::string* out_err
);

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
