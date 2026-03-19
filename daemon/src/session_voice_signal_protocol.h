#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>

namespace agentd {

struct VoiceBrokerSignalEvent {
  std::string type;
  Json::Value payload = Json::Value(Json::objectValue);
  std::string from;
  std::string sender_tag;
  int64_t ts_unix_ms = 0;
  Json::Value raw = Json::Value(Json::objectValue);
};

struct VoiceBrokerSignalDescription {
  std::string type;
  std::string sdp;
  std::string sender_tag;
};

struct VoiceBrokerSignalCandidate {
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;
  bool has_sdp_mline_index = false;
  std::string username_fragment;
  std::string sender_tag;
};

struct VoiceBrokerSignalBye {
  std::string reason;
  std::string sender_tag;
};

bool parse_voice_broker_signal_event_json(const std::string& raw, VoiceBrokerSignalEvent* out_event);

bool parse_voice_broker_signal_description_payload(
  const Json::Value& payload,
  VoiceBrokerSignalDescription* out_desc,
  std::string* out_err
);

bool parse_voice_broker_signal_candidate_payload(
  const Json::Value& payload,
  VoiceBrokerSignalCandidate* out_candidate,
  std::string* out_err
);

bool parse_voice_broker_signal_bye_payload(
  const Json::Value& payload,
  VoiceBrokerSignalBye* out_bye,
  std::string* out_err
);

Json::Value make_voice_broker_description_payload(const VoiceBrokerSignalDescription& desc);
Json::Value make_voice_broker_bye_payload(const VoiceBrokerSignalBye& bye);

bool voice_broker_signal_is_from_sender(const VoiceBrokerSignalEvent& ev, const std::string& sender_tag);

}  // namespace agentd
