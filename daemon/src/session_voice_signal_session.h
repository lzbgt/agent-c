#pragma once

#include "session_voice_signal_protocol.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agentd {

enum class VoiceBrokerSignalIngressKind {
  ignored_self,
  ignored_unknown,
  remote_description,
  remote_candidate_queued,
  remote_candidate_ready,
  remote_bye,
};

struct VoiceBrokerSignalIngress {
  VoiceBrokerSignalIngressKind kind = VoiceBrokerSignalIngressKind::ignored_unknown;
  VoiceBrokerSignalDescription description;
  VoiceBrokerSignalCandidate candidate;
  VoiceBrokerSignalBye bye;
};

struct VoiceBrokerSignalRemoteDescriptionReady {
  VoiceBrokerSignalDescription description;
  std::vector<VoiceBrokerSignalCandidate> initial_remote_candidates;
};

class VoiceBrokerSignalSessionState {
 public:
  explicit VoiceBrokerSignalSessionState(std::string self_sender_tag = "");

  const std::string& self_sender_tag() const { return self_sender_tag_; }
  bool remote_offer_seen() const { return remote_offer_seen_; }
  bool remote_description_applied() const { return remote_description_applied_; }
  bool closed_by_remote() const { return closed_by_remote_; }
  const std::string& remote_bye_reason() const { return remote_bye_reason_; }
  size_t received_candidate_count() const { return received_candidate_count_; }
  size_t pending_remote_candidate_count() const { return pending_remote_candidates_.size(); }
  const std::vector<VoiceBrokerSignalCandidate>& pending_remote_candidates() const {
    return pending_remote_candidates_;
  }

  bool ingest_event(
    const VoiceBrokerSignalEvent& ev,
    VoiceBrokerSignalIngress* out_ingress,
    std::string* out_error
  );

  bool mark_remote_description_applied(
    std::vector<VoiceBrokerSignalCandidate>* out_drained_candidates,
    std::string* out_error
  );

 private:
  std::string self_sender_tag_;
  bool remote_offer_seen_ = false;
  bool remote_description_applied_ = false;
  bool closed_by_remote_ = false;
  std::string remote_bye_reason_;
  size_t received_candidate_count_ = 0;
  std::vector<VoiceBrokerSignalCandidate> pending_remote_candidates_;
};

bool finalize_voice_broker_remote_description_ready(
  VoiceBrokerSignalSessionState* io_state,
  const VoiceBrokerSignalIngress& ingress,
  VoiceBrokerSignalRemoteDescriptionReady* out_ready,
  std::string* out_error
);

}  // namespace agentd
