#include "agent_db.h"
#include "json_util.h"
#include "session_voice_runtime_store.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::VoicePeerRuntime;
using agentd::json_stringify;
using agentd::load_voice_peer_runtime_record;
using agentd::persist_voice_peer_runtime_record;
using agentd::recover_voice_peer_runtime_record;
using agentd::voice_peer_runtime_to_json;

static std::filesystem::path make_temp_db_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".sqlite");
}

static void open_test_db(const std::filesystem::path& path, AgentDb* out_db) {
  assert(out_db);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::string err;
  const bool ok = out_db->open(path.string(), &err);
  assert(ok);
  (void)err;
}

static void test_persist_rejects_planned_runtime_preview() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("voice_runtime_store_planned_reject");
  AgentDb db;
  open_test_db(db_path, &db);

  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.status_source = "planned";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";

  std::string err;
  assert(!persist_voice_peer_runtime_record(&db, st, &err));
  assert(err == "refusing to persist planned voice runtime preview");

  std::string raw;
  err.clear();
  assert(db.meta_get("session.voice_webrtc_peer.voice-sid", &raw, &err));
  assert(raw.empty());
#endif
}

static void test_load_self_heals_planned_runtime_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("voice_runtime_store_planned_load");
  AgentDb db;
  open_test_db(db_path, &db);

  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.status_source = "planned";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";
  st.ready = false;
  st.running = false;

  std::string err;
  assert(db.meta_set("session.voice_webrtc_peer.voice-sid", json_stringify(voice_peer_runtime_to_json(st)), &err));

  bool self_healed = false;
  std::shared_ptr<VoicePeerRuntime> loaded;
  err.clear();
  assert(load_voice_peer_runtime_record(&db, "voice-sid", &loaded, &self_healed, &err));
  assert(!loaded);
  assert(self_healed);
  assert(err.empty());

  std::string raw;
  assert(db.meta_get("session.voice_webrtc_peer.voice-sid", &raw, &err));
  assert(raw.empty());
#endif
}

static void test_recover_reports_cleanup_for_planned_runtime_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("voice_runtime_store_planned_recover");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("voice_runtime_store_planned_state_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "voice_webrtc_peers" / "voice-sid";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stdout.jsonl").string().c_str(), "w");
    assert(f);
    std::fputs("{\"planned\":true}\n", f);
    std::fclose(f);
  }

  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.status_source = "planned";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";
  st.stdout_log_path = (runtime_dir / "stdout.jsonl").string();
  st.stderr_log_path = (runtime_dir / "stderr.log").string();
  st.ready_file_path = (runtime_dir / "ready.json").string();

  std::string err;
  assert(db.meta_set("session.voice_webrtc_peer.voice-sid", json_stringify(voice_peer_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  std::shared_ptr<VoicePeerRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_voice_peer_runtime_record(cfg, &db, "voice-sid", &recovered, &updates, &err));
  assert(!recovered);
  assert(err.empty());
  assert(updates["cleanup_on_corrupt_record"]["persisted_record_cleared"].asBool());
  assert(updates["cleanup_on_corrupt_record"]["runtime_artifacts_deleted"].asBool());
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

static void test_runtime_json_round_trips_media_engine_fields() {
  VoicePeerRuntime st;
  st.session_id = "voice-sid";
  st.runtime_kind = "builtin";
  st.media_engine_kind = "builtin_signaling_stub";
  st.media_engine_state = "signaling_active";
  st.status_source = "memory";
  st.broker_url = "http://broker";
  st.sender_tag = "agentd_runtime_peer";
  st.tool_path = "@builtin";
  st.node_bin = "@builtin";
  st.media_state_updated_unix_ms = 1234;
  st.media_events_total = 5;
  st.media_remote_offers_seen = 1;
  st.media_answers_sent = 1;
  st.media_remote_candidates_seen = 2;
  st.media_remote_byes_seen = 0;
  st.media_local_byes_sent = 1;
  st.native_media_supported = false;
  st.native_media_active = false;
  st.dtls_identity_ready = true;
  st.dtls_handshake_ready = true;
  st.dtls_exporter_ready = true;
  st.srtp_contexts_ready = true;
  st.srtp_inbound_ready = true;
  st.srtp_outbound_ready = true;
  st.dtls_fingerprint_sha256 = "AA:BB:CC";
  st.dtls_setup_role = "passive";
  st.dtls_certificate_subject = "/CN=agentd builtin embedded transport";
  st.dtls_handshake_state = "connected";
  st.dtls_selected_srtp_profile = "SRTP_AES128_CM_SHA1_80";
  st.srtp_last_error = "none";
  st.dtls_packets_sent = 12;
  st.dtls_packets_received = 9;
  st.native_media_provider["abi_version"] = 3;
  st.native_media_provider["name"] = "agentd_builtin_sample_provider";
  st.native_media_provider["capabilities"]["transport_family"] = "sample_webrtc";
  st.native_media_provider["capabilities"]["sample_provider"] = true;

  const Json::Value json = voice_peer_runtime_to_json(st);
  assert(json["media_engine_kind"].asString() == "builtin_signaling_stub");
  assert(json["media_engine_state"].asString() == "signaling_active");
  assert(json["media_state_updated_unix_ms"].asInt64() == 1234);
  assert(json["media_events_total"].asInt64() == 5);
  assert(json["media_remote_offers_seen"].asInt64() == 1);
  assert(json["media_answers_sent"].asInt64() == 1);
  assert(json["media_remote_candidates_seen"].asInt64() == 2);
  assert(json["media_remote_byes_seen"].asInt64() == 0);
  assert(json["media_local_byes_sent"].asInt64() == 1);
  assert(json["native_media_supported"].asBool() == false);
  assert(json["native_media_active"].asBool() == false);
  assert(json["dtls_identity_ready"].asBool());
  assert(json["dtls_handshake_ready"].asBool());
  assert(json["dtls_exporter_ready"].asBool());
  assert(json["srtp_contexts_ready"].asBool());
  assert(json["srtp_inbound_ready"].asBool());
  assert(json["srtp_outbound_ready"].asBool());
  assert(json["dtls_fingerprint_sha256"].asString() == "AA:BB:CC");
  assert(json["dtls_setup_role"].asString() == "passive");
  assert(json["dtls_certificate_subject"].asString() == "/CN=agentd builtin embedded transport");
  assert(json["dtls_handshake_state"].asString() == "connected");
  assert(json["dtls_selected_srtp_profile"].asString() == "SRTP_AES128_CM_SHA1_80");
  assert(json["srtp_last_error"].asString() == "none");
  assert(json["dtls_packets_sent"].asInt64() == 12);
  assert(json["dtls_packets_received"].asInt64() == 9);
  assert(json["native_media_provider"]["abi_version"].asInt() == 3);
  assert(json["native_media_provider"]["name"].asString() == "agentd_builtin_sample_provider");

  VoicePeerRuntime round_trip;
  std::string err;
  assert(agentd::voice_peer_runtime_from_json(json, &round_trip, &err));
  assert(err.empty());
  assert(round_trip.media_engine_kind == "builtin_signaling_stub");
  assert(round_trip.media_engine_state == "signaling_active");
  assert(round_trip.media_state_updated_unix_ms == 1234);
  assert(round_trip.media_events_total == 5);
  assert(round_trip.media_remote_offers_seen == 1);
  assert(round_trip.media_answers_sent == 1);
  assert(round_trip.media_remote_candidates_seen == 2);
  assert(round_trip.media_remote_byes_seen == 0);
  assert(round_trip.media_local_byes_sent == 1);
  assert(round_trip.native_media_supported == false);
  assert(round_trip.native_media_active == false);
  assert(round_trip.dtls_identity_ready);
  assert(round_trip.dtls_handshake_ready);
  assert(round_trip.dtls_exporter_ready);
  assert(round_trip.srtp_contexts_ready);
  assert(round_trip.srtp_inbound_ready);
  assert(round_trip.srtp_outbound_ready);
  assert(round_trip.dtls_fingerprint_sha256 == "AA:BB:CC");
  assert(round_trip.dtls_setup_role == "passive");
  assert(round_trip.dtls_certificate_subject == "/CN=agentd builtin embedded transport");
  assert(round_trip.dtls_handshake_state == "connected");
  assert(round_trip.dtls_selected_srtp_profile == "SRTP_AES128_CM_SHA1_80");
  assert(round_trip.srtp_last_error == "none");
  assert(round_trip.dtls_packets_sent == 12);
  assert(round_trip.dtls_packets_received == 9);
  assert(round_trip.native_media_provider["abi_version"].asInt() == 3);
  assert(round_trip.native_media_provider["capabilities"]["transport_family"].asString() == "sample_webrtc");
  assert(round_trip.native_media_provider["capabilities"]["sample_provider"].asBool());
}

}  // namespace

int main() {
  test_persist_rejects_planned_runtime_preview();
  test_load_self_heals_planned_runtime_record();
  test_recover_reports_cleanup_for_planned_runtime_record();
  test_runtime_json_round_trips_media_engine_fields();
  return 0;
}
