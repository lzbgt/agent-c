#include "session_voice_signal_session.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::VoiceBrokerSignalEvent;
using agentd::VoiceBrokerSignalIngress;
using agentd::VoiceBrokerSignalIngressKind;
using agentd::VoiceBrokerSignalRemoteDescriptionReady;
using agentd::VoiceBrokerSignalSessionState;
using agentd::finalize_voice_broker_remote_description_ready;

static VoiceBrokerSignalEvent make_event(const std::string& type, const Json::Value& payload) {
  VoiceBrokerSignalEvent ev;
  ev.type = type;
  ev.payload = payload;
  if (payload.isObject() && payload.isMember("sender_tag") && payload["sender_tag"].isString()) {
    ev.sender_tag = payload["sender_tag"].asString();
  }
  return ev;
}

static void test_self_sender_events_are_ignored() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  Json::Value payload(Json::objectValue);
  payload["reason"] = "self";
  payload["sender_tag"] = "agentd-runtime";

  VoiceBrokerSignalIngress ingress;
  std::string err;
  assert(state.ingest_event(make_event("bye", payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::ignored_self);
  assert(!state.closed_by_remote());
}

static void test_offer_then_candidate_queue_then_drain() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  std::string err;
  VoiceBrokerSignalIngress description_ingress;

  Json::Value offer_payload(Json::objectValue);
  offer_payload["type"] = "offer";
  offer_payload["sdp"] = "stub-offer";
  offer_payload["sender_tag"] = "webui-peer";
  assert(state.ingest_event(make_event("offer", offer_payload), &description_ingress, &err));
  assert(err.empty());
  assert(description_ingress.kind == VoiceBrokerSignalIngressKind::remote_description);
  assert(state.remote_offer_seen());
  assert(!state.remote_description_applied());

  Json::Value candidate_payload(Json::objectValue);
  candidate_payload["candidate"] = "cand-1";
  candidate_payload["sdpMid"] = "audio";
  candidate_payload["sdpMLineIndex"] = 0;
  candidate_payload["sender_tag"] = "webui-peer";
  VoiceBrokerSignalIngress candidate_ingress;
  assert(state.ingest_event(make_event("candidate", candidate_payload), &candidate_ingress, &err));
  assert(err.empty());
  assert(candidate_ingress.kind == VoiceBrokerSignalIngressKind::remote_candidate_queued);
  assert(state.received_candidate_count() == 1);
  assert(state.pending_remote_candidate_count() == 1);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  assert(finalize_voice_broker_remote_description_ready(&state, description_ingress, &ready, &err));
  assert(err.empty());
  assert(state.remote_description_applied());
  assert(state.pending_remote_candidate_count() == 0);
  assert(ready.description.sdp == "stub-offer");
  assert(ready.initial_remote_candidates.size() == 1);
  assert(ready.initial_remote_candidates[0].candidate == "cand-1");
}

static void test_candidate_ready_after_remote_description() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  std::string err;
  std::vector<agentd::VoiceBrokerSignalCandidate> drained;
  assert(state.mark_remote_description_applied(&drained, &err));
  assert(err.empty());
  assert(drained.empty());

  Json::Value candidate_payload(Json::objectValue);
  candidate_payload["candidate"] = "cand-2";
  candidate_payload["sdpMid"] = "audio";
  candidate_payload["sdpMLineIndex"] = 1;
  candidate_payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalIngress ingress;
  assert(state.ingest_event(make_event("candidate", candidate_payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::remote_candidate_ready);
  assert(state.received_candidate_count() == 1);
  assert(state.pending_remote_candidate_count() == 0);
  assert(ingress.candidate.candidate == "cand-2");
}

static void test_relay_candidates_are_ignored_before_queue_or_ready() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  std::string err;

  Json::Value relay_payload(Json::objectValue);
  relay_payload["candidate"] =
    "candidate:relay 1 UDP 1677729535 203.0.113.2 60000 typ relay raddr 10.0.0.2 rport 50000";
  relay_payload["sdpMid"] = "audio";
  relay_payload["sdpMLineIndex"] = 0;
  relay_payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalIngress ingress;
  assert(state.ingest_event(make_event("candidate", relay_payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::ignored_relay_candidate);
  assert(state.received_candidate_count() == 0);
  assert(state.pending_remote_candidate_count() == 0);

  std::vector<agentd::VoiceBrokerSignalCandidate> drained;
  assert(state.mark_remote_description_applied(&drained, &err));
  assert(err.empty());
  assert(drained.empty());
  assert(state.ingest_event(make_event("candidate", relay_payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::ignored_relay_candidate);
  assert(state.received_candidate_count() == 0);
}

static void test_empty_candidate_payload_becomes_ready_end_marker() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  std::string err;
  std::vector<agentd::VoiceBrokerSignalCandidate> drained;
  assert(state.mark_remote_description_applied(&drained, &err));
  assert(err.empty());

  Json::Value candidate_payload(Json::objectValue);
  candidate_payload["candidate"] = "";
  candidate_payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalIngress ingress;
  assert(state.ingest_event(make_event("candidate", candidate_payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::remote_candidate_ready);
  assert(ingress.candidate.candidate == "a=end-of-candidates");
  assert(state.received_candidate_count() == 1);
  assert(state.pending_remote_candidate_count() == 0);
}

static void test_remote_bye_captures_close_reason() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  Json::Value payload(Json::objectValue);
  payload["reason"] = "webui_done";
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalIngress ingress;
  std::string err;
  assert(state.ingest_event(make_event("bye", payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::remote_bye);
  assert(state.closed_by_remote());
  assert(state.remote_bye_reason() == "webui_done");
}

static void test_unknown_types_are_ignored() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  Json::Value payload(Json::objectValue);
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalIngress ingress;
  std::string err;
  assert(state.ingest_event(make_event("ping", payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::ignored_unknown);
}

static void test_invalid_candidate_payload_fails_closed() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  Json::Value payload(Json::objectValue);
  payload["sdpMid"] = "audio";
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalIngress ingress;
  std::string err;
  assert(!state.ingest_event(make_event("candidate", payload), &ingress, &err));
  assert(err == "candidate payload missing candidate");
}

static void test_finalize_remote_description_rejects_wrong_ingress_kind() {
  VoiceBrokerSignalSessionState state("agentd-runtime");
  VoiceBrokerSignalIngress ingress;
  ingress.kind = VoiceBrokerSignalIngressKind::remote_bye;

  VoiceBrokerSignalRemoteDescriptionReady ready;
  std::string err;
  assert(!finalize_voice_broker_remote_description_ready(&state, ingress, &ready, &err));
  assert(err == "signal ingress must be a remote description");
}

}  // namespace

int main() {
  test_self_sender_events_are_ignored();
  test_offer_then_candidate_queue_then_drain();
  test_candidate_ready_after_remote_description();
  test_relay_candidates_are_ignored_before_queue_or_ready();
  test_empty_candidate_payload_becomes_ready_end_marker();
  test_remote_bye_captures_close_reason();
  test_unknown_types_are_ignored();
  test_invalid_candidate_payload_fails_closed();
  test_finalize_remote_description_rejects_wrong_ingress_kind();
  return 0;
}
