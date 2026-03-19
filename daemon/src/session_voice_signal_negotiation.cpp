#include "session_voice_signal_negotiation.h"

#include "string_util.h"

namespace agentd {

VoiceBrokerSignalAnswerExchangeOps default_voice_broker_signal_answer_exchange_ops() {
  VoiceBrokerSignalAnswerExchangeOps ops;
  ops.wait_remote_description_ready_with_state = wait_for_voice_broker_signal_remote_description_ready_with_state;
  ops.send_answer = send_voice_broker_answer;
  ops.wait_remote_candidate_ready = wait_for_voice_broker_signal_remote_candidate_ready;
  ops.wait_remote_bye = wait_for_voice_broker_signal_remote_bye;
  ops.send_bye = send_voice_broker_bye;
  return ops;
}

bool run_voice_broker_signal_answer_exchange_with_ops(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalAnswerExchangeOptions& options,
  const VoiceBrokerSignalAnswerExchangeOps& ops,
  VoiceBrokerSignalAnswerExchangeResult* out_result,
  long* out_http_status,
  std::string* out_err
) {
  if (out_http_status) *out_http_status = 0;
  if (out_err) out_err->clear();
  if (!out_result) {
    if (out_err) *out_err = "missing answer exchange output";
    return false;
  }
  *out_result = VoiceBrokerSignalAnswerExchangeResult{};

  if (!ops.wait_remote_description_ready_with_state || !ops.send_answer ||
      !ops.wait_remote_candidate_ready || !ops.wait_remote_bye || !ops.send_bye) {
    if (out_err) *out_err = "answer exchange ops incomplete";
    return false;
  }

  VoiceBrokerSignalSessionState signal_state(options.self_sender_tag);
  if (!ops.wait_remote_description_ready_with_state(
        broker_url,
        token,
        session_id,
        options.remote_description_timeout_ms,
        &signal_state,
        &out_result->remote_ready,
        out_http_status,
        out_err)) {
    return false;
  }

  const std::string offer_type = lower_copy(trim_copy(out_result->remote_ready.description.type));
  if (!offer_type.empty() && offer_type != "offer") {
    if (out_err) *out_err = "expected remote offer, got " + offer_type;
    return false;
  }

  if (!ops.send_answer(
        broker_url, token, session_id, options.local_answer, out_err)) {
    return false;
  }

  if (options.post_answer_candidate_timeout_ms > 0) {
    if (!ops.wait_remote_candidate_ready(
          broker_url,
          token,
          session_id,
          options.post_answer_candidate_timeout_ms,
          &signal_state,
          &out_result->post_answer_remote_candidate,
          out_http_status,
          out_err)) {
      return false;
    }
    out_result->post_answer_remote_candidate_received =
      !trim_copy(out_result->post_answer_remote_candidate.candidate).empty();
  }

  if (options.remote_bye_timeout_ms > 0) {
    if (!ops.wait_remote_bye(
          broker_url,
          token,
          session_id,
          options.remote_bye_timeout_ms,
          &signal_state,
          &out_result->remote_bye,
          out_http_status,
          out_err)) {
      return false;
    }
    out_result->remote_bye_received = true;
  }

  if (!trim_copy(options.local_bye.reason).empty() ||
      !trim_copy(options.local_bye.sender_tag).empty()) {
    if (!ops.send_bye(
          broker_url, token, session_id, options.local_bye, out_err)) {
      return false;
    }
  }

  return true;
}

bool run_voice_broker_signal_answer_exchange(
  const std::string& broker_url,
  const std::string& token,
  const std::string& session_id,
  const VoiceBrokerSignalAnswerExchangeOptions& options,
  VoiceBrokerSignalAnswerExchangeResult* out_result,
  long* out_http_status,
  std::string* out_err
) {
  return run_voice_broker_signal_answer_exchange_with_ops(
    broker_url,
    token,
    session_id,
    options,
    default_voice_broker_signal_answer_exchange_ops(),
    out_result,
    out_http_status,
    out_err);
}

}  // namespace agentd
