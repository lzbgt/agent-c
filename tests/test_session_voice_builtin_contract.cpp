#include "session_voice_builtin_contract.h"
#include "session_voice_runtime_store.h"

#include <cassert>
#include <string>

namespace {

using agentd::VoicePeerStartPlan;
using agentd::session_voice_builtin_start_contract_json;
using agentd::build_voice_peer_builtin_start_preview;
using agentd::DaemonConfig;
using agentd::voice_peer_runtime_to_json;

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH ""
#endif

static void assert_known_test_provider(
  const std::string& provider_name,
  const Json::Value& capabilities
) {
  if (provider_name == "agentd_builtin_sample_provider") {
    assert(capabilities["sample_provider"].asBool());
    assert(capabilities["transport_family"].asString() == "sample_webrtc");
    return;
  }
  if (provider_name == "agentd_builtin_embedded_transport_provider") {
    assert(capabilities["embedded_transport_provider"].asBool());
    assert(capabilities["transport_family"].asString() == "embedded_transport_primitives");
    assert(capabilities["audio_decode"].asBool());
    assert(capabilities["audio_stage"].asBool());
    assert(capabilities["audio_drain"].asBool());
    assert(capabilities["audio_owner_handoff"].asBool());
    assert(capabilities["audio_codec_pcmu"].asBool());
    assert(capabilities["audio_codec_pcma"].asBool());
    assert(capabilities.isMember("audio_codec_opus"));
    assert(capabilities["ice"].asBool());
    assert(capabilities["srtp"].asBool());
    assert(capabilities["rtp_ingest"].asBool());
    assert(capabilities["sctp"].asBool());
    return;
  }
  assert(false && "unexpected builtin native provider");
}

static bool expected_native_supported_for_provider(const std::string& provider_name) {
  if (provider_name == "agentd_builtin_sample_provider") return false;
  if (provider_name == "agentd_builtin_embedded_transport_provider") return true;
  assert(false && "unexpected builtin native provider");
  return false;
}

static void test_borrowed_broker_session_contract() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.requested_broker_session_id = "sess-1";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.sender_tag = "agentd_runtime_peer";
  plan.deadline_ms = 1111;
  plan.poll_interval_ms = 222;
  plan.tone_hz = 333;
  plan.startup_wait_ms = 444;

  const Json::Value out = session_voice_builtin_start_contract_json(cfg, "voice-sid", plan);
  assert(out["session_id"].asString() == "voice-sid");
  assert(out["runtime_kind"].asString() == "builtin");
  assert(out["signaling_surface"].asString() == "voice_webrtc_peer");
  assert(out["broker_url"].asString() == "http://broker");
  assert(out["sender_tag"].asString() == "agentd_runtime_peer");
  assert(out["deadline_ms"].asInt64() == 1111);
  assert(out["poll_interval_ms"].asInt64() == 222);
  assert(out["tone_hz"].asInt64() == 333);
  assert(out["startup_wait_ms"].asInt64() == 444);
  assert(out["mutating_broker_actions_deferred"].asBool());
  assert(out["startup_sequence"].isArray());
  assert(out["startup_sequence"].size() == 4);
  assert(out["startup_sequence"][0]["stage"].asString() == "borrowed_broker_session_preflight");
  assert(!out["startup_sequence"][0]["deferred"].asBool());
  assert(out["startup_sequence"][1]["stage"].asString() == "launch_runtime");
  assert(out["startup_sequence"][1]["deferred"].asBool());
  assert(out["runtime_artifacts"]["runtime_dir"].asString() == "/tmp/agentd-state/voice_webrtc_peers/voice-sid");
  assert(out["runtime_artifacts"]["ready_file_path"].asString() == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/ready.json");
  assert(out["runtime_artifacts"]["stdout_log_path"].asString() == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/stdout.jsonl");
  assert(out["runtime_artifacts"]["stderr_log_path"].asString() == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/stderr.log");
  assert(out["media_runtime_plan"]["schema"].asString() == "voice_webrtc_peer_media_runtime_plan_v1");
  assert(out["media_runtime_plan"]["signaling_surface"].asString() == "voice_webrtc_peer");
  assert(out["media_runtime_plan"]["runtime_kind"].asString() == "builtin");
  assert(out["media_runtime_plan"]["media_engine_kind"].asString() == "builtin_reserved");
  assert(out["media_runtime_plan"]["session_id"].asString() == "voice-sid");
  assert(out["media_runtime_plan"]["broker_session_id"].asString() == "sess-1");
  assert(out["media_runtime_plan"]["managed_broker_session"].asBool() == false);
  assert(out["media_runtime_plan"]["sender_tag"].asString() == "agentd_runtime_peer");
  assert(out["media_runtime_plan"]["ready_file_path"].asString() == out["runtime_artifacts"]["ready_file_path"].asString());
  assert(out["media_runtime_plan"]["deadline_ms"].asInt64() == 1111);
  assert(out["media_runtime_plan"]["poll_interval_ms"].asInt64() == 222);
  assert(out["media_runtime_plan"]["tone_hz"].asInt64() == 333);
  assert(out["media_runtime_plan"]["native_media_supported"].asBool() == false);
  assert(out["planned_runtime"]["schema"].asString() == "session_voice_webrtc_peer_runtime_v1");
  assert(out["planned_runtime"]["runtime_kind"].asString() == "builtin");
  assert(out["planned_runtime"]["media_engine_kind"].asString() == "builtin_reserved");
  assert(out["planned_runtime"]["media_engine_state"].asString() == "planned");
  assert(out["planned_runtime"]["status_source"].asString() == "planned");
  assert(out["planned_runtime"]["session_id"].asString() == "voice-sid");
  assert(out["planned_runtime"]["broker_session_id"].asString() == "sess-1");
  assert(out["planned_runtime"]["managed_broker_session"].asBool() == false);
  assert(out["planned_runtime"]["tool_path"].asString() == "@builtin");
  assert(out["planned_runtime"]["node_bin"].asString() == "@builtin");
  assert(out["planned_runtime"]["ready"].asBool() == false);
  assert(out["planned_runtime"]["running"].asBool() == false);
  assert(out["planned_runtime"]["media_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_process_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_pcm_samples_processed_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_last_process_samples"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_render_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_pcm_samples_rendered_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_last_render_samples"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_playback_enabled"].asBool() == false);
  assert(out["planned_runtime"]["audio_playback_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_pcm_samples_played_total"].asInt64() == 0);
  assert(out["planned_runtime"]["ready_file_path"].asString() == out["runtime_artifacts"]["ready_file_path"].asString());
  assert(out["broker_session"]["mode"].asString() == "borrowed");
  assert(out["broker_session"]["session_id"].asString() == "sess-1");
  assert(out["broker_session"]["preflighted"].asBool());
  assert(out["broker_session"]["session_mode"].asString() == "webrtc");

  const agentd::VoicePeerBuiltinStartPreview preview =
    build_voice_peer_builtin_start_preview(cfg, "voice-sid", plan);
  assert(preview.contract == out);
  assert(voice_peer_runtime_to_json(preview.planned_runtime) == out["planned_runtime"]);
}

static void test_auto_create_broker_session_contract() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.sender_tag = "agentd_runtime_peer";

  const Json::Value out = session_voice_builtin_start_contract_json(cfg, "voice-sid", plan);
  assert(out["broker_session"]["mode"].asString() == "auto_create");
  assert(!out["broker_session"]["preflighted"].asBool());
  assert(out["broker_session"]["agent_id"].asString() == "agent-a");
  assert(out["broker_session"]["deployment_id"].asString() == "deploy-b");
  assert(out["startup_sequence"].isArray());
  assert(out["startup_sequence"].size() == 4);
  assert(out["startup_sequence"][0]["stage"].asString() == "auto_create_broker_session");
  assert(out["startup_sequence"][0]["deferred"].asBool());
  assert(out["runtime_artifacts"]["stdout_format"].asString() == "jsonl");
  assert(out["runtime_artifacts"]["stderr_format"].asString() == "text");
  assert(out["media_runtime_plan"]["schema"].asString() == "voice_webrtc_peer_media_runtime_plan_v1");
  assert(out["media_runtime_plan"]["runtime_kind"].asString() == "builtin");
  assert(out["media_runtime_plan"]["media_engine_kind"].asString() == "builtin_reserved");
  assert(out["media_runtime_plan"]["session_id"].asString() == "voice-sid");
  assert(out["media_runtime_plan"]["broker_session_id"].isNull());
  assert(out["media_runtime_plan"]["managed_broker_session"].asBool() == true);
  assert(out["media_runtime_plan"]["broker_agent_id"].asString() == "agent-a");
  assert(out["media_runtime_plan"]["broker_deployment_id"].asString() == "deploy-b");
  assert(out["media_runtime_plan"]["ready_file_path"].asString() == out["runtime_artifacts"]["ready_file_path"].asString());
  assert(out["planned_runtime"]["schema"].asString() == "session_voice_webrtc_peer_runtime_v1");
  assert(out["planned_runtime"]["runtime_kind"].asString() == "builtin");
  assert(out["planned_runtime"]["media_engine_kind"].asString() == "builtin_reserved");
  assert(out["planned_runtime"]["media_engine_state"].asString() == "planned");
  assert(out["planned_runtime"]["status_source"].asString() == "planned");
  assert(out["planned_runtime"]["session_id"].asString() == "voice-sid");
  assert(out["planned_runtime"]["managed_broker_session"].asBool() == true);
  assert(out["planned_runtime"]["broker_agent_id"].asString() == "agent-a");
  assert(out["planned_runtime"]["broker_deployment_id"].asString() == "deploy-b");
  assert(out["planned_runtime"]["tool_path"].asString() == "@builtin");
  assert(out["planned_runtime"]["node_bin"].asString() == "@builtin");
  assert(out["planned_runtime"]["broker_session_id"].isNull());
  assert(out["planned_runtime"]["media_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_process_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_pcm_samples_processed_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_last_process_samples"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_render_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_pcm_samples_rendered_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_last_render_samples"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_playback_enabled"].asBool() == false);
  assert(out["planned_runtime"]["audio_playback_events_total"].asInt64() == 0);
  assert(out["planned_runtime"]["audio_pcm_samples_played_total"].asInt64() == 0);
  assert(out["planned_runtime"]["stdout_log_path"].asString() == out["runtime_artifacts"]["stdout_log_path"].asString());

  const agentd::VoicePeerBuiltinStartPreview preview =
    build_voice_peer_builtin_start_preview(cfg, "voice-sid", plan);
  assert(preview.contract == out);
  assert(voice_peer_runtime_to_json(preview.planned_runtime) == out["planned_runtime"]);
}

static void test_enabled_builtin_contract_marks_runtime_live_path() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  cfg.audio_webrtc_builtin_mode = "signaling_stub";
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.requested_broker_session_id = "sess-enabled";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.sender_tag = "agentd_runtime_peer";

  const Json::Value out = session_voice_builtin_start_contract_json(cfg, "voice-sid", plan);
  assert(out["mutating_broker_actions_deferred"].asBool() == false);
  assert(out["startup_sequence"][1]["deferred"].asBool() == false);
  assert(out["media_runtime_plan"]["media_engine_kind"].asString() == "builtin_signaling_stub");
  assert(out["media_runtime_plan"]["native_media_supported"].asBool() == false);
  assert(out["planned_runtime"].isObject());
  assert(out["planned_runtime"]["media_engine_kind"].asString() == "builtin_signaling_stub");
  assert(out["planned_runtime"]["media_engine_state"].asString() == "planned");
  assert(out["planned_runtime"].isMember("last_error") == false);
}

static void test_native_plugin_builtin_contract_marks_native_media_path() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH;
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.requested_broker_session_id = "sess-native";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.sender_tag = "agentd_runtime_peer";

  const Json::Value out = session_voice_builtin_start_contract_json(cfg, "voice-sid", plan);
  assert(out["mutating_broker_actions_deferred"].asBool() == false);
  assert(out["media_runtime_plan"]["media_engine_kind"].asString() == "builtin_native_plugin");
  assert(out["media_runtime_plan"]["native_media_provider"]["abi_version"].asInt() == 4);
  assert(!out["media_runtime_plan"]["native_media_provider"]["name"].asString().empty());
  assert(out["media_runtime_plan"]["native_media_supported"].asBool() ==
         expected_native_supported_for_provider(
           out["media_runtime_plan"]["native_media_provider"]["name"].asString()));
  assert_known_test_provider(
    out["media_runtime_plan"]["native_media_provider"]["name"].asString(),
    out["media_runtime_plan"]["native_media_provider"]["capabilities"]);
  assert(out["planned_runtime"]["media_engine_kind"].asString() == "builtin_native_plugin");
  assert(out["planned_runtime"]["native_media_supported"].asBool() ==
         expected_native_supported_for_provider(
           out["planned_runtime"]["native_media_provider"]["name"].asString()));
  assert(out["planned_runtime"]["native_media_active"].asBool() == false);
  assert(out["planned_runtime"]["native_media_provider"]["abi_version"].asInt() == 4);
  assert(out["planned_runtime"]["native_media_provider"]["name"].asString() ==
         out["media_runtime_plan"]["native_media_provider"]["name"].asString());
  assert(out["planned_runtime"].isMember("last_error") == false);
}

}  // namespace

int main() {
  test_borrowed_broker_session_contract();
  test_auto_create_broker_session_contract();
  test_enabled_builtin_contract_marks_runtime_live_path();
  test_native_plugin_builtin_contract_marks_native_media_path();
  return 0;
}
