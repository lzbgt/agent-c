#include "session_voice_signal_session.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::VoiceBrokerSignalEvent;
using agentd::VoiceBrokerSignalIngress;
using agentd::VoiceBrokerSignalIngressKind;
using agentd::VoiceBrokerSignalSessionState;

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
  VoiceBrokerSignalIngress ingress;

  Json::Value offer_payload(Json::objectValue);
  offer_payload["type"] = "offer";
  offer_payload["sdp"] = "stub-offer";
  offer_payload["sender_tag"] = "webui-peer";
  assert(state.ingest_event(make_event("offer", offer_payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::remote_description);
  assert(state.remote_offer_seen());
  assert(!state.remote_description_applied());

  Json::Value candidate_payload(Json::objectValue);
  candidate_payload["candidate"] = "cand-1";
  candidate_payload["sdpMid"] = "audio";
  candidate_payload["sdpMLineIndex"] = 0;
  candidate_payload["sender_tag"] = "webui-peer";
  assert(state.ingest_event(make_event("candidate", candidate_payload), &ingress, &err));
  assert(err.empty());
  assert(ingress.kind == VoiceBrokerSignalIngressKind::remote_candidate_queued);
  assert(state.received_candidate_count() == 1);
  assert(state.pending_remote_candidate_count() == 1);

  std::vector<agentd::VoiceBrokerSignalCandidate> drained;
  assert(state.mark_remote_description_applied(&drained, &err));
  assert(err.empty());
  assert(state.remote_description_applied());
  assert(state.pending_remote_candidate_count() == 0);
  assert(drained.size() == 1);
  assert(drained[0].candidate == "cand-1");
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

}  // namespace

int main() {
  test_self_sender_events_are_ignored();
  test_offer_then_candidate_queue_then_drain();
  test_candidate_ready_after_remote_description();
  test_remote_bye_captures_close_reason();
  test_unknown_types_are_ignored();
  test_invalid_candidate_payload_fails_closed();
  return 0;
}
