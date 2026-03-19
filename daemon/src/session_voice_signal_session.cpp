#include "session_voice_signal_session.h"

#include "string_util.h"

namespace agentd {

VoiceBrokerSignalSessionState::VoiceBrokerSignalSessionState(std::string self_sender_tag)
    : self_sender_tag_(trim_copy(self_sender_tag)) {}

bool VoiceBrokerSignalSessionState::ingest_event(
  const VoiceBrokerSignalEvent& ev,
  VoiceBrokerSignalIngress* out_ingress,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_ingress) *out_ingress = VoiceBrokerSignalIngress{};

  VoiceBrokerSignalIngress ingress;
  if (voice_broker_signal_is_from_sender(ev, self_sender_tag_)) {
    ingress.kind = VoiceBrokerSignalIngressKind::ignored_self;
    if (out_ingress) *out_ingress = std::move(ingress);
    return true;
  }

  if (ev.type == "offer" || ev.type == "answer") {
    if (!parse_voice_broker_signal_description_payload(ev.payload, &ingress.description, out_error)) return false;
    ingress.kind = VoiceBrokerSignalIngressKind::remote_description;
    if (ev.type == "offer") remote_offer_seen_ = true;
    if (out_ingress) *out_ingress = std::move(ingress);
    return true;
  }

  if (ev.type == "candidate") {
    if (!parse_voice_broker_signal_candidate_payload(ev.payload, &ingress.candidate, out_error)) return false;
    received_candidate_count_ += 1;
    if (remote_description_applied_) {
      ingress.kind = VoiceBrokerSignalIngressKind::remote_candidate_ready;
    } else {
      pending_remote_candidates_.push_back(ingress.candidate);
      ingress.kind = VoiceBrokerSignalIngressKind::remote_candidate_queued;
    }
    if (out_ingress) *out_ingress = std::move(ingress);
    return true;
  }

  if (ev.type == "bye") {
    if (!parse_voice_broker_signal_bye_payload(ev.payload, &ingress.bye, out_error)) return false;
    closed_by_remote_ = true;
    remote_bye_reason_ = ingress.bye.reason;
    ingress.kind = VoiceBrokerSignalIngressKind::remote_bye;
    if (out_ingress) *out_ingress = std::move(ingress);
    return true;
  }

  ingress.kind = VoiceBrokerSignalIngressKind::ignored_unknown;
  if (out_ingress) *out_ingress = std::move(ingress);
  return true;
}

bool VoiceBrokerSignalSessionState::mark_remote_description_applied(
  std::vector<VoiceBrokerSignalCandidate>* out_drained_candidates,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_drained_candidates) *out_drained_candidates = pending_remote_candidates_;
  remote_description_applied_ = true;
  pending_remote_candidates_.clear();
  return true;
}

}  // namespace agentd
