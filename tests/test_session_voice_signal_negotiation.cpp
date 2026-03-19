#include "session_voice_signal_negotiation.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::VoiceBrokerSignalAnswerExchangeOps;
using agentd::VoiceBrokerSignalAnswerExchangeOptions;
using agentd::VoiceBrokerSignalAnswerExchangeResult;
using agentd::VoiceBrokerSignalBye;
using agentd::VoiceBrokerSignalCandidate;
using agentd::VoiceBrokerSignalDescription;
using agentd::VoiceBrokerSignalRemoteDescriptionReady;
using agentd::VoiceBrokerSignalSessionState;
using agentd::default_voice_broker_signal_answer_exchange_ops;
using agentd::run_voice_broker_signal_answer_exchange_with_ops;

static void test_default_ops_are_complete() {
  const VoiceBrokerSignalAnswerExchangeOps ops = default_voice_broker_signal_answer_exchange_ops();
  assert(ops.wait_remote_description_ready_with_state);
  assert(ops.send_answer);
  assert(ops.wait_remote_candidate_ready);
  assert(ops.wait_remote_bye);
  assert(ops.send_bye);
}

static void test_answer_exchange_runs_full_sequence() {
  std::vector<std::string> calls;
  VoiceBrokerSignalAnswerExchangeOps ops;
  ops.wait_remote_description_ready_with_state =
    [&calls](
      const std::string&,
      const std::string&,
      const std::string&,
      int64_t timeout_ms,
      VoiceBrokerSignalSessionState* io_state,
      VoiceBrokerSignalRemoteDescriptionReady* out_ready,
      long* out_http_status,
      std::string* out_err) {
      calls.push_back("wait_remote_description_ready_with_state");
      assert(timeout_ms == 3210);
      assert(io_state);
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_ready) {
        out_ready->description.type = "offer";
        out_ready->description.sdp = "stub-offer";
        VoiceBrokerSignalCandidate c;
        c.candidate = "cand-pre";
        out_ready->initial_remote_candidates.push_back(c);
      }
      return true;
    };
  ops.send_answer =
    [&calls](
      const std::string&,
      const std::string&,
      const std::string&,
      const VoiceBrokerSignalDescription& answer,
      std::string* out_err) {
      calls.push_back("send_answer");
      assert(answer.type == "answer");
      assert(answer.sdp == "stub-answer");
      if (out_err) out_err->clear();
      return true;
    };
  ops.wait_remote_candidate_ready =
    [&calls](
      const std::string&,
      const std::string&,
      const std::string&,
      int64_t timeout_ms,
      VoiceBrokerSignalSessionState* io_state,
      VoiceBrokerSignalCandidate* out_candidate,
      long* out_http_status,
      std::string* out_err) {
      calls.push_back("wait_remote_candidate_ready");
      assert(timeout_ms == 4567);
      assert(io_state);
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_candidate) out_candidate->candidate = "cand-post";
      return true;
    };
  ops.wait_remote_bye =
    [&calls](
      const std::string&,
      const std::string&,
      const std::string&,
      int64_t timeout_ms,
      VoiceBrokerSignalSessionState* io_state,
      VoiceBrokerSignalBye* out_bye,
      long* out_http_status,
      std::string* out_err) {
      calls.push_back("wait_remote_bye");
      assert(timeout_ms == 7890);
      assert(io_state);
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_bye) out_bye->reason = "remote_done";
      return true;
    };
  ops.send_bye =
    [&calls](
      const std::string&,
      const std::string&,
      const std::string&,
      const VoiceBrokerSignalBye& bye,
      std::string* out_err) {
      calls.push_back("send_bye");
      assert(bye.reason == "local_done");
      if (out_err) out_err->clear();
      return true;
    };

  VoiceBrokerSignalAnswerExchangeOptions options;
  options.self_sender_tag = "agentd-runtime";
  options.remote_description_timeout_ms = 3210;
  options.post_answer_candidate_timeout_ms = 4567;
  options.remote_bye_timeout_ms = 7890;
  options.local_answer.type = "answer";
  options.local_answer.sdp = "stub-answer";
  options.local_bye.reason = "local_done";

  VoiceBrokerSignalAnswerExchangeResult result;
  long http_status = 0;
  std::string err;
  assert(run_voice_broker_signal_answer_exchange_with_ops(
    "http://broker",
    "tok",
    "sess-1",
    options,
    ops,
    &result,
    &http_status,
    &err));
  assert(err.empty());
  assert(http_status == 200);
  assert(result.remote_ready.description.sdp == "stub-offer");
  assert(result.remote_ready.initial_remote_candidates.size() == 1);
  assert(result.post_answer_remote_candidate_received);
  assert(result.post_answer_remote_candidate.candidate == "cand-post");
  assert(result.remote_bye_received);
  assert(result.remote_bye.reason == "remote_done");
  assert((calls == std::vector<std::string>{
    "wait_remote_description_ready_with_state",
    "send_answer",
    "wait_remote_candidate_ready",
    "wait_remote_bye",
    "send_bye"}));
}

static void test_answer_exchange_rejects_non_offer_description() {
  VoiceBrokerSignalAnswerExchangeOps ops = default_voice_broker_signal_answer_exchange_ops();
  ops.wait_remote_description_ready_with_state =
    [](
      const std::string&,
      const std::string&,
      const std::string&,
      int64_t,
      VoiceBrokerSignalSessionState*,
      VoiceBrokerSignalRemoteDescriptionReady* out_ready,
      long* out_http_status,
      std::string* out_err) {
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_ready) {
        out_ready->description.type = "answer";
        out_ready->description.sdp = "unexpected-answer";
      }
      return true;
    };
  ops.send_answer = [](
      const std::string&, const std::string&, const std::string&, const VoiceBrokerSignalDescription&, std::string*) {
    assert(false && "send_answer should not be called for non-offer");
    return false;
  };

  VoiceBrokerSignalAnswerExchangeOptions options;
  options.local_answer.type = "answer";
  options.local_answer.sdp = "stub-answer";

  VoiceBrokerSignalAnswerExchangeResult result;
  long http_status = 0;
  std::string err;
  assert(!run_voice_broker_signal_answer_exchange_with_ops(
    "http://broker",
    "tok",
    "sess-1",
    options,
    ops,
    &result,
    &http_status,
    &err));
  assert(err == "expected remote offer, got answer");
}

static void test_answer_exchange_requires_complete_ops() {
  VoiceBrokerSignalAnswerExchangeOps ops;
  VoiceBrokerSignalAnswerExchangeOptions options;
  VoiceBrokerSignalAnswerExchangeResult result;
  long http_status = 0;
  std::string err;
  assert(!run_voice_broker_signal_answer_exchange_with_ops(
    "http://broker",
    "tok",
    "sess-1",
    options,
    ops,
    &result,
    &http_status,
    &err));
  assert(err == "answer exchange ops incomplete");
}

}  // namespace

int main() {
  test_default_ops_are_complete();
  test_answer_exchange_runs_full_sequence();
  test_answer_exchange_rejects_non_offer_description();
  test_answer_exchange_requires_complete_ops();
  return 0;
}
