#include "session_voice_signal_protocol.h"

#include "session_voice_sdp_candidate.h"
#include "string_util.h"

#include <memory>

namespace agentd {
namespace {

std::string payload_sender_tag(const Json::Value& payload) {
  if (!payload.isObject()) return "";
  if (!payload.isMember("sender_tag") || !payload["sender_tag"].isString()) return "";
  return trim_copy(payload["sender_tag"].asString());
}

}  // namespace

bool parse_voice_broker_signal_event_json(const std::string& raw, VoiceBrokerSignalEvent* out_event) {
  if (!out_event) return false;
  Json::CharReaderBuilder rb;
  rb["collectComments"] = false;
  std::string errs;
  Json::Value root(Json::nullValue);
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  if (!reader->parse(raw.data(), raw.data() + raw.size(), &root, &errs)) return false;
  if (!root.isObject()) return false;

  VoiceBrokerSignalEvent ev;
  ev.raw = root;
  if (root.isMember("type") && root["type"].isString()) ev.type = trim_copy(root["type"].asString());
  if (root.isMember("payload") && root["payload"].isObject()) ev.payload = root["payload"];
  else ev.payload = Json::Value(Json::objectValue);
  if (root.isMember("from") && root["from"].isString()) ev.from = trim_copy(root["from"].asString());
  ev.sender_tag = payload_sender_tag(ev.payload);
  if (root.isMember("ts_unix_ms") && (root["ts_unix_ms"].isInt64() || root["ts_unix_ms"].isUInt64())) {
    ev.ts_unix_ms = root["ts_unix_ms"].asInt64();
  }
  *out_event = std::move(ev);
  return true;
}

bool parse_voice_broker_signal_description_payload(
  const Json::Value& payload,
  VoiceBrokerSignalDescription* out_desc,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_desc) {
    if (out_err) *out_err = "missing description output";
    return false;
  }
  if (!payload.isObject()) {
    if (out_err) *out_err = "description payload must be an object";
    return false;
  }
  if (!payload.isMember("sdp") || !payload["sdp"].isString() || trim_copy(payload["sdp"].asString()).empty()) {
    if (out_err) *out_err = "description payload missing sdp";
    return false;
  }

  VoiceBrokerSignalDescription desc;
  if (payload.isMember("type") && payload["type"].isString()) desc.type = trim_copy(payload["type"].asString());
  desc.sdp = strip_sdp_relay_candidate_lines(trim_copy(payload["sdp"].asString()));
  desc.sender_tag = payload_sender_tag(payload);
  *out_desc = std::move(desc);
  return true;
}

bool parse_voice_broker_signal_candidate_payload(
  const Json::Value& payload,
  VoiceBrokerSignalCandidate* out_candidate,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_candidate) {
    if (out_err) *out_err = "missing candidate output";
    return false;
  }
  if (!payload.isObject()) {
    if (out_err) *out_err = "candidate payload must be an object";
    return false;
  }
  if (!payload.isMember("candidate") || !payload["candidate"].isString()) {
    if (out_err) *out_err = "candidate payload missing candidate";
    return false;
  }

  VoiceBrokerSignalCandidate candidate;
  const std::string raw_candidate = trim_copy(payload["candidate"].asString());
  candidate.candidate = sdp_candidate_is_end_marker(raw_candidate)
    ? std::string("a=end-of-candidates")
    : normalize_sdp_candidate_line(raw_candidate);
  if (payload.isMember("sdpMid") && payload["sdpMid"].isString()) {
    candidate.sdp_mid = trim_copy(payload["sdpMid"].asString());
  }
  if (payload.isMember("sdpMLineIndex") &&
      (payload["sdpMLineIndex"].isInt() || payload["sdpMLineIndex"].isUInt() ||
       payload["sdpMLineIndex"].isInt64() || payload["sdpMLineIndex"].isUInt64())) {
    candidate.sdp_mline_index = payload["sdpMLineIndex"].asInt();
    candidate.has_sdp_mline_index = true;
  }
  if (payload.isMember("usernameFragment") && payload["usernameFragment"].isString()) {
    candidate.username_fragment = trim_copy(payload["usernameFragment"].asString());
  }
  candidate.sender_tag = payload_sender_tag(payload);
  *out_candidate = std::move(candidate);
  return true;
}

bool parse_voice_broker_signal_bye_payload(
  const Json::Value& payload,
  VoiceBrokerSignalBye* out_bye,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_bye) {
    if (out_err) *out_err = "missing bye output";
    return false;
  }
  if (!payload.isObject()) {
    if (out_err) *out_err = "bye payload must be an object";
    return false;
  }

  VoiceBrokerSignalBye bye;
  if (payload.isMember("reason") && payload["reason"].isString()) bye.reason = trim_copy(payload["reason"].asString());
  bye.sender_tag = payload_sender_tag(payload);
  *out_bye = std::move(bye);
  return true;
}

Json::Value make_voice_broker_description_payload(const VoiceBrokerSignalDescription& desc) {
  Json::Value payload(Json::objectValue);
  if (!trim_copy(desc.type).empty()) payload["type"] = trim_copy(desc.type);
  payload["sdp"] = strip_sdp_relay_candidate_lines(desc.sdp);
  if (!trim_copy(desc.sender_tag).empty()) payload["sender_tag"] = trim_copy(desc.sender_tag);
  return payload;
}

Json::Value make_voice_broker_candidate_payload(const VoiceBrokerSignalCandidate& candidate) {
  Json::Value payload(Json::objectValue);
  payload["candidate"] = candidate.candidate;
  if (!trim_copy(candidate.sdp_mid).empty()) payload["sdpMid"] = trim_copy(candidate.sdp_mid);
  if (candidate.has_sdp_mline_index) payload["sdpMLineIndex"] = candidate.sdp_mline_index;
  if (!trim_copy(candidate.username_fragment).empty()) {
    payload["usernameFragment"] = trim_copy(candidate.username_fragment);
  }
  if (!trim_copy(candidate.sender_tag).empty()) payload["sender_tag"] = trim_copy(candidate.sender_tag);
  return payload;
}

Json::Value make_voice_broker_bye_payload(const VoiceBrokerSignalBye& bye) {
  Json::Value payload(Json::objectValue);
  if (!trim_copy(bye.reason).empty()) payload["reason"] = trim_copy(bye.reason);
  if (!trim_copy(bye.sender_tag).empty()) payload["sender_tag"] = trim_copy(bye.sender_tag);
  return payload;
}

bool voice_broker_signal_is_from_sender(const VoiceBrokerSignalEvent& ev, const std::string& sender_tag) {
  const std::string expected = trim_copy(sender_tag);
  return !expected.empty() && ev.sender_tag == expected;
}

}  // namespace agentd
