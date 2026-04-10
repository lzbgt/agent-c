#include "session_voice_builtin_backend.h"
#include "session_voice_backend_policy.h"
#include "session_voice_builtin_service.h"
#include "session_voice_runtime_registry.h"
#include "session_voice_runtime.h"
#include "session_voice_runtime_store.h"

#include <cassert>
#include <string>

namespace {

using agentd::DaemonConfig;
using agentd::VoicePeerBackendStartResult;
using agentd::VoicePeerStartPlan;
using agentd::voice_peer_runtime_registry_mutex;
using agentd::start_voice_peer_builtin_backend;
using agentd::voice_peer_runtime_to_json;

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH ""
#endif

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_LEGACY_V1_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_LEGACY_V1_PLUGIN_PATH ""
#endif

static void assert_known_test_provider(
  const std::string& provider_name,
  const Json::Value& capabilities
) {
  if (provider_name == "agentd_builtin_sample_provider") {
    assert(capabilities["sample_provider"].asBool());
    assert(capabilities["transport_family"].asString() == "sample_webrtc");
    assert(capabilities["real_media_engine"].asBool() == false);
    return;
  }
  if (provider_name == "agentd_builtin_embedded_transport_provider") {
    assert(capabilities["embedded_transport_provider"].asBool());
    assert(capabilities["transport_family"].asString() == "embedded_transport_primitives");
    assert(capabilities["ice"].asBool());
    assert(capabilities["srtp"].asBool());
    assert(capabilities["sctp"].asBool());
    assert(capabilities["real_media_engine"].asBool() == false);
    return;
  }
  assert(false && "unexpected builtin native provider");
}

static VoicePeerStartPlan make_plan() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "builtin";
  plan.effective_broker_url = "http://broker";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.sender_tag = "agentd_runtime_peer";
  plan.deadline_ms = 1111;
  plan.poll_interval_ms = 222;
  plan.tone_hz = 333;
  return plan;
}

static void test_builtin_backend_returns_planned_runtime_state() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  VoicePeerBackendStartResult result;

  assert(!start_voice_peer_builtin_backend(cfg, "voice-sid", make_plan(), &result));
  assert(result.http_status == 501);
  assert(result.state);
  assert(result.state->runtime_kind == "builtin");
  assert(result.state->media_engine_kind == "builtin_reserved");
  assert(result.state->media_engine_state == "planned");
  assert(result.state->status_source == "planned");
  assert(result.state->session_id == "voice-sid");
  assert(result.state->broker_url == "http://broker");
  assert(result.state->managed_broker_session);
  assert(result.state->broker_agent_id == "agent-a");
  assert(result.state->broker_deployment_id == "deploy-b");
  assert(result.state->tool_path == "@builtin");
  assert(result.state->node_bin == "@builtin");
  assert(result.state->ready_file_path == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/ready.json");
  assert(result.state->stdout_log_path == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/stdout.jsonl");
  assert(result.state->stderr_log_path == "/tmp/agentd-state/voice_webrtc_peers/voice-sid/stderr.log");
  assert(!result.state->running);
  assert(!result.state->ready);
  assert(result.state->last_error == result.error);
  assert(result.state->media_events_total == 0);
  assert(result.state->native_media_supported == false);
  assert(result.state->native_media_active == false);
  assert(result.backend_info["builtin_start_contract"].isObject());
  assert(result.backend_info["builtin_start_contract"]["media_runtime_plan"]["schema"].asString() ==
         "voice_webrtc_peer_media_runtime_plan_v1");
  assert(result.backend_info["builtin_start_contract"]["media_runtime_plan"]["session_id"].asString() ==
         "voice-sid");
  assert(voice_peer_runtime_to_json(*result.state) ==
         result.backend_info["builtin_start_contract"]["planned_runtime"]);
}

static void test_builtin_backend_borrowed_session_preview_is_not_managed() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  VoicePeerStartPlan plan = make_plan();
  plan.requested_broker_session_id = "sess-1";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.broker_agent_id.clear();
  plan.broker_deployment_id.clear();

  VoicePeerBackendStartResult result;
  assert(!start_voice_peer_builtin_backend(cfg, "voice-sid", plan, &result));
  assert(result.state);
  assert(result.state->status_source == "planned");
  assert(!result.state->managed_broker_session);
  assert(result.state->broker_session_id == "sess-1");
  assert(voice_peer_runtime_to_json(*result.state) ==
         result.backend_info["builtin_start_contract"]["planned_runtime"]);
}

static void test_builtin_backend_enabled_signaling_stub_starts_runtime() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  cfg.audio_webrtc_builtin_mode = "signaling_stub";
  VoicePeerStartPlan plan = make_plan();
  plan.effective_broker_url = "http://127.0.0.1:9";
  plan.broker_token = "tok";
  plan.requested_broker_session_id = "sess-enabled";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.broker_agent_id.clear();
  plan.broker_deployment_id.clear();

  VoicePeerBackendStartResult result;
  assert(start_voice_peer_builtin_backend(cfg, "voice-sid-enabled", plan, &result));
  assert(result.ok);
  assert(result.http_status == 200);
  assert(result.startup_confirmed);
  assert(result.state);
  assert(result.state->runtime_kind == "builtin");
  assert(result.state->media_engine_kind == "builtin_signaling_stub");
  assert(result.state->media_engine_state == "signaling_ready");
  assert(result.state->running);
  assert(result.state->ready);
  assert(result.state->media_events_total == 2);
  assert(result.state->media_answers_sent == 0);
  assert(result.state->media_remote_offers_seen == 0);
  assert(result.state->native_media_supported == false);
  assert(result.state->native_media_active == false);
  assert(result.state->managed_broker_session == false);
  assert(result.backend_info["builtin_start_contract"]["mutating_broker_actions_deferred"].asBool() == false);
  assert(result.backend_info["builtin_start_contract"]["planned_runtime"].isObject());
  assert(result.backend_info["builtin_start_contract"]["planned_runtime"].isMember("last_error") == false);
  assert(builtin_voice_peer_runtime_enabled(cfg));

  bool stopped = false;
  std::string stop_err;
  assert(stop_builtin_voice_peer_runtime_service(
    result.state,
    voice_peer_runtime_registry_mutex(),
    3000,
    &stopped,
    &stop_err));
  assert(stop_err.empty());
  assert(stopped);
  {
    std::lock_guard<std::mutex> lk(voice_peer_runtime_registry_mutex());
    refresh_builtin_voice_peer_runtime_state(result.state.get());
    assert(!result.state->running);
  }
}

static void test_builtin_backend_enabled_native_plugin_starts_runtime() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH;
  VoicePeerStartPlan plan = make_plan();
  plan.effective_broker_url = "http://127.0.0.1:9";
  plan.broker_token = "tok";
  plan.requested_broker_session_id = "sess-native";
  plan.requested_broker_session_preflighted = true;
  plan.requested_broker_session_mode = "webrtc";
  plan.broker_agent_id.clear();
  plan.broker_deployment_id.clear();

  VoicePeerBackendStartResult result;
  assert(start_voice_peer_builtin_backend(cfg, "voice-sid-native", plan, &result));
  assert(result.ok);
  assert(result.http_status == 200);
  assert(result.state);
  assert(result.state->runtime_kind == "builtin");
  assert(result.state->media_engine_kind == "builtin_native_plugin");
  assert(result.state->native_media_supported == false);
  assert(result.state->native_media_active == false);
  assert(result.state->media_engine_state == "signaling_ready");
  assert(result.state->media_events_total == 2);
  assert(result.state->managed_broker_session == false);
  assert(result.state->native_media_provider["abi_version"].asInt() == 2);
  assert(!result.state->native_media_provider["name"].asString().empty());
  assert_known_test_provider(
    result.state->native_media_provider["name"].asString(),
    result.state->native_media_provider["capabilities"]);

  bool stopped = false;
  std::string stop_err;
  assert(stop_builtin_voice_peer_runtime_service(
    result.state,
    voice_peer_runtime_registry_mutex(),
    3000,
    &stopped,
    &stop_err));
  assert(stop_err.empty());
  assert(stopped);
}

static void test_backend_metadata_reports_native_probe_details() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_PLUGIN_PATH;
  const Json::Value meta = agentd::session_voice_webrtc_backend_metadata_json(cfg);
  assert(meta["builtin_mode"].asString() == "native_plugin");
  assert(meta["builtin_available"].asBool());
  assert(meta["builtin_native_library_path_configured"].asBool());
  assert(meta["builtin_native_probe"]["loadable"].asBool());
  assert(meta["builtin_native_probe"]["native_media_supported"].asBool() == false);
  assert(meta["builtin_native_probe"]["provider"]["abi_version"].asInt() == 2);
  assert(!meta["builtin_native_probe"]["provider"]["name"].asString().empty());
  assert_known_test_provider(
    meta["builtin_native_probe"]["provider"]["name"].asString(),
    meta["builtin_native_probe"]["provider"]["capabilities"]);
}

static void test_backend_metadata_reports_legacy_v1_probe_details() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path = AGENTD_TEST_VOICE_MEDIA_ENGINE_LEGACY_V1_PLUGIN_PATH;
  const Json::Value meta = agentd::session_voice_webrtc_backend_metadata_json(cfg);
  assert(meta["builtin_mode"].asString() == "native_plugin");
  assert(meta["builtin_available"].asBool());
  assert(meta["builtin_native_probe"]["loadable"].asBool());
  assert(meta["builtin_native_probe"]["native_media_supported"].asBool() == false);
  assert(meta["builtin_native_probe"]["provider"]["abi_version"].asInt() == 1);
  assert(meta["builtin_native_probe"]["provider"]["version"].asString() == "legacy_abi_v1");
  assert(meta["builtin_native_probe"]["provider"]["capabilities"]["legacy_abi_v1"].asBool());
}

}  // namespace

int main() {
  test_builtin_backend_returns_planned_runtime_state();
  test_builtin_backend_borrowed_session_preview_is_not_managed();
  test_builtin_backend_enabled_signaling_stub_starts_runtime();
  test_builtin_backend_enabled_native_plugin_starts_runtime();
  test_backend_metadata_reports_native_probe_details();
  test_backend_metadata_reports_legacy_v1_probe_details();
  return 0;
}
