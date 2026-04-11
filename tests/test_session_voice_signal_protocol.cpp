#include "session_voice_signal_protocol.h"

#include <cassert>
#include <string>

namespace {

using agentd::VoiceBrokerSignalBye;
using agentd::VoiceBrokerSignalCandidate;
using agentd::VoiceBrokerSignalDescription;
using agentd::VoiceBrokerSignalEvent;
using agentd::make_voice_broker_bye_payload;
using agentd::make_voice_broker_candidate_payload;
using agentd::make_voice_broker_description_payload;
using agentd::parse_voice_broker_signal_bye_payload;
using agentd::parse_voice_broker_signal_candidate_payload;
using agentd::parse_voice_broker_signal_description_payload;
using agentd::parse_voice_broker_signal_event_json;
using agentd::voice_broker_signal_is_from_sender;

static void test_event_json_roundtrip_extracts_sender_tag() {
  const std::string raw =
    R"({"type":"candidate","payload":{"candidate":"cand-1","sdpMid":"0","sdpMLineIndex":1,"usernameFragment":"ufrag-1","sender_tag":"agentd-runtime"},"from":"webui","ts_unix_ms":123})";
  VoiceBrokerSignalEvent ev;
  assert(parse_voice_broker_signal_event_json(raw, &ev));
  assert(ev.type == "candidate");
  assert(ev.from == "webui");
  assert(ev.sender_tag == "agentd-runtime");
  assert(ev.ts_unix_ms == 123);
  assert(voice_broker_signal_is_from_sender(ev, "agentd-runtime"));
  assert(!voice_broker_signal_is_from_sender(ev, "other"));
}

static void test_description_payload_parse_and_build() {
  Json::Value payload(Json::objectValue);
  payload["type"] = "offer";
  payload["sdp"] = "stub-offer";
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalDescription desc;
  std::string err;
  assert(parse_voice_broker_signal_description_payload(payload, &desc, &err));
  assert(err.empty());
  assert(desc.type == "offer");
  assert(desc.sdp == "stub-offer");
  assert(desc.sender_tag == "webui-peer");

  VoiceBrokerSignalDescription answer;
  answer.type = "answer";
  answer.sdp = "stub-answer";
  answer.sender_tag = "agentd-runtime";
  const Json::Value built = make_voice_broker_description_payload(answer);
  assert(built.isObject());
  assert(built["type"].asString() == "answer");
  assert(built["sdp"].asString() == "stub-answer");
  assert(built["sender_tag"].asString() == "agentd-runtime");
}

static void test_candidate_payload_parse() {
  Json::Value payload(Json::objectValue);
  payload["candidate"] = "candidate:1 1 UDP 2122260223 127.0.0.1 55555 typ host";
  payload["sdpMid"] = "audio";
  payload["sdpMLineIndex"] = 0;
  payload["usernameFragment"] = "ufrag-1";
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalCandidate candidate;
  std::string err;
  assert(parse_voice_broker_signal_candidate_payload(payload, &candidate, &err));
  assert(err.empty());
  assert(candidate.candidate == "a=candidate:1 1 udp 2122260223 127.0.0.1 55555 typ host");
  assert(candidate.sdp_mid == "audio");
  assert(candidate.has_sdp_mline_index);
  assert(candidate.sdp_mline_index == 0);
  assert(candidate.username_fragment == "ufrag-1");
  assert(candidate.sender_tag == "webui-peer");

  const Json::Value built = make_voice_broker_candidate_payload(candidate);
  assert(built.isObject());
  assert(built["candidate"].asString() == "a=candidate:1 1 udp 2122260223 127.0.0.1 55555 typ host");
  assert(built["sdpMid"].asString() == "audio");
  assert(built["sdpMLineIndex"].asInt() == 0);
  assert(built["usernameFragment"].asString() == "ufrag-1");
  assert(built["sender_tag"].asString() == "webui-peer");
}

static void test_empty_candidate_payload_is_end_marker() {
  Json::Value payload(Json::objectValue);
  payload["candidate"] = "";
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalCandidate candidate;
  std::string err;
  assert(parse_voice_broker_signal_candidate_payload(payload, &candidate, &err));
  assert(err.empty());
  assert(candidate.candidate == "a=end-of-candidates");
  assert(candidate.sender_tag == "webui-peer");
}

static void test_malformed_end_marker_candidate_payload_is_preserved() {
  Json::Value payload(Json::objectValue);
  payload["candidate"] = "a=end-of-candidates:malformed";
  payload["sender_tag"] = "webui-peer";

  VoiceBrokerSignalCandidate candidate;
  std::string err;
  assert(parse_voice_broker_signal_candidate_payload(payload, &candidate, &err));
  assert(err.empty());
  assert(candidate.candidate == "a=end-of-candidates:malformed");
  assert(candidate.sender_tag == "webui-peer");
}

static void test_bye_payload_parse_and_build() {
  Json::Value payload(Json::objectValue);
  payload["reason"] = "done";
  payload["sender_tag"] = "agentd-runtime";

  VoiceBrokerSignalBye bye;
  std::string err;
  assert(parse_voice_broker_signal_bye_payload(payload, &bye, &err));
  assert(err.empty());
  assert(bye.reason == "done");
  assert(bye.sender_tag == "agentd-runtime");

  const Json::Value built = make_voice_broker_bye_payload(bye);
  assert(built.isObject());
  assert(built["reason"].asString() == "done");
  assert(built["sender_tag"].asString() == "agentd-runtime");
}

static void test_invalid_payloads_rejected() {
  VoiceBrokerSignalDescription desc;
  VoiceBrokerSignalCandidate candidate;
  std::string err;

  Json::Value missing_sdp(Json::objectValue);
  missing_sdp["type"] = "offer";
  assert(!parse_voice_broker_signal_description_payload(missing_sdp, &desc, &err));
  assert(err == "description payload missing sdp");

  Json::Value missing_candidate(Json::objectValue);
  missing_candidate["sdpMid"] = "0";
  assert(!parse_voice_broker_signal_candidate_payload(missing_candidate, &candidate, &err));
  assert(err == "candidate payload missing candidate");
}

}  // namespace

int main() {
  test_event_json_roundtrip_extracts_sender_tag();
  test_description_payload_parse_and_build();
  test_candidate_payload_parse();
  test_empty_candidate_payload_is_end_marker();
  test_malformed_end_marker_candidate_payload_is_preserved();
  test_bye_payload_parse_and_build();
  test_invalid_payloads_rejected();
  return 0;
}
