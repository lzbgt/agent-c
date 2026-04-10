#include "session_voice_builtin_media_engine.h"

#include "session_voice_runtime_internal.h"

#include <juice/juice.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

using agentd::DaemonConfig;
using agentd::VoiceBrokerSignalDescription;
using agentd::VoiceBrokerSignalRemoteDescriptionReady;
using agentd::VoicePeerRuntime;
using agentd::make_builtin_voice_peer_media_engine;
using agentd::note_voice_peer_media_engine_event;

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH ""
#endif

struct LoopbackRemoteContext {
  agentd::VoicePeerBuiltinMediaEngine* engine = nullptr;
  VoicePeerRuntime* runtime = nullptr;
  juice_state_t remote_state = JUICE_STATE_DISCONNECTED;
  Json::Value last_candidate_event = Json::Value(Json::nullValue);
  std::string last_error;
  int candidate_events = 0;
  bool gathering_done = false;
};

void on_loopback_remote_state_changed(juice_agent_t*, juice_state_t state, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  ctx->remote_state = state;
}

void on_loopback_remote_candidate(juice_agent_t*, const char* sdp, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  assert(ctx->engine);
  assert(ctx->runtime);

  agentd::VoiceBrokerSignalIngress ingress;
  ingress.kind = agentd::VoiceBrokerSignalIngressKind::remote_candidate_ready;
  ingress.candidate.candidate = sdp ? sdp : "";

  Json::Value event(Json::nullValue);
  std::string err;
  const bool ok = ctx->engine->handle_remote_candidate(ingress, &event, &err);
  assert(ok);
  assert(err.empty());
  note_voice_peer_media_engine_event(ctx->runtime, event);
  ctx->last_candidate_event = event;
  ctx->candidate_events += 1;
}

void on_loopback_remote_gathering_done(juice_agent_t*, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  ctx->gathering_done = true;
}

std::string make_libjuice_offer_sdp() {
  juice_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  juice_agent_t* agent = juice_create(&cfg);
  assert(agent);

  char description[JUICE_MAX_SDP_STRING_LEN];
  std::memset(description, 0, sizeof(description));
  const int rc = juice_get_local_description(agent, description, sizeof(description));
  assert(rc == JUICE_ERR_SUCCESS);
  const std::string offer = description;
  juice_destroy(agent);
  return offer;
}

std::string make_browser_style_offer_sdp() {
  return
    "v=0\r\n"
    "o=- 4962323985234234 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=ice-ufrag:remoteUfrag\r\n"
    "a=ice-pwd:remotePassword123\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=candidate:1 1 UDP 2113667327 192.168.0.11 40000 typ host\r\n"
    "a=end-of-candidates\r\n";
}

static void test_embedded_transport_provider_loads_and_answers_remote_offer() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().media_engine_kind == "builtin_native_plugin");
  assert(!engine->info().native_media_supported);
  assert(engine->info().provider_name == "agentd_builtin_embedded_transport_provider");
  assert(engine->info().provider_capabilities["embedded_transport_provider"].asBool());
  assert(engine->info().provider_capabilities["transport_family"].asString() ==
         "embedded_transport_primitives");
  assert(engine->info().provider_capabilities["ice"].asBool());
  assert(engine->info().provider_capabilities["dtls"].asBool());
  assert(engine->info().provider_capabilities["dtls_handshake"].asBool());
  assert(engine->info().provider_capabilities["dtls_srtp_export"].asBool());
  assert(engine->info().provider_capabilities["srtp"].asBool());
  assert(engine->info().provider_capabilities["sctp"].asBool());
  assert(engine->info().provider_capabilities["real_media_engine"].asBool() == false);

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(init_event["event"].asString() == "media_engine_initialized");
  assert(init_event["transport_family"].asString() == "embedded_transport_primitives");
  assert(init_event["libjuice_local_description_bytes"].asUInt64() > 0);
  assert(init_event["gather_started"].asBool());
  assert(init_event["dtls_identity_ready"].asBool());
  assert(init_event["dtls_handshake_ready"].asBool() == false);
  assert(init_event["dtls_exporter_ready"].asBool() == false);
  assert(init_event["dtls_handshake_state"].asString() == "ready_for_client_hello");
  assert(init_event["dtls_setup_role"].asString() == "passive");
  assert(!init_event["dtls_fingerprint_sha256"].asString().empty());
  assert(!init_event["dtls_certificate_subject"].asString().empty());
  assert(init_event["usrsctp_initialized"].asBool());
  assert(!init_event["srtp_version"].asString().empty());
  assert(runtime.native_media_provider["name"].asString() ==
         "agentd_builtin_embedded_transport_provider");

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_libjuice_offer_sdp();

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  assert(answer.type == "answer");
  assert(!answer.sdp.empty());
  assert(answer.sdp.find("a=ice-ufrag:") != std::string::npos);
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer_event["event"].asString() == "embedded_transport_answer_ready");
  assert(answer_event["remote_description_applied"].asBool());
  assert(answer_event["transport_family"].asString() == "embedded_transport_primitives");
  assert(answer_event["media_engine_state"].asString() == "answer_ready");
  assert(answer_event["sdp_answer_shape"].asString() == "ice_only");
  assert(runtime.media_engine_state == "answer_ready");
}

static void test_embedded_transport_provider_mirrors_browser_offer_shape_with_dtls_identity() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().provider_name == "agentd_builtin_embedded_transport_provider");

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(runtime.dtls_identity_ready);
  assert(!runtime.dtls_fingerprint_sha256.empty());
  assert(runtime.dtls_setup_role == "passive");
  assert(!runtime.dtls_certificate_subject.empty());

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_browser_style_offer_sdp();
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8") != std::string::npos);
  assert(answer.sdp.find("a=mid:0") != std::string::npos);
  assert(answer.sdp.find("a=inactive") != std::string::npos);
  assert(answer.sdp.find("a=setup:passive") != std::string::npos);
  assert(answer.sdp.find("a=fingerprint:sha-256 ") != std::string::npos);
  assert(answer.sdp.find(runtime.dtls_fingerprint_sha256) != std::string::npos);
  assert(answer.sdp.find("a=ice-ufrag:") != std::string::npos);
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  assert(answer_event["dtls_identity_ready"].asBool());
  assert(answer_event["dtls_setup_role"].asString() == "passive");
  assert(answer_event["sdp_answer_shape"].asString() == "browser_offer_mirrored_inactive");
  assert(answer_event["dtls_fingerprint_sha256"].asString() == runtime.dtls_fingerprint_sha256);
}

static void test_embedded_transport_provider_reaches_local_ice_connectivity() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().provider_name == "agentd_builtin_embedded_transport_provider");

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);

  LoopbackRemoteContext ctx;
  ctx.engine = engine.get();
  ctx.runtime = &runtime;

  juice_config_t remote_cfg;
  std::memset(&remote_cfg, 0, sizeof(remote_cfg));
  remote_cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  remote_cfg.cb_state_changed = &on_loopback_remote_state_changed;
  remote_cfg.cb_candidate = &on_loopback_remote_candidate;
  remote_cfg.cb_gathering_done = &on_loopback_remote_gathering_done;
  remote_cfg.user_ptr = &ctx;

  juice_agent_t* remote_agent = juice_create(&remote_cfg);
  assert(remote_agent);

  char offer_buf[JUICE_MAX_SDP_STRING_LEN];
  std::memset(offer_buf, 0, sizeof(offer_buf));
  assert(juice_get_local_description(remote_agent, offer_buf, sizeof(offer_buf)) == JUICE_ERR_SUCCESS);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = offer_buf;
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  assert(juice_set_remote_description(remote_agent, answer.sdp.c_str()) == JUICE_ERR_SUCCESS);
  assert(juice_set_remote_gathering_done(remote_agent) == JUICE_ERR_SUCCESS);
  assert(juice_gather_candidates(remote_agent) == JUICE_ERR_SUCCESS);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx.candidate_events > 0 &&
        ctx.remote_state != JUICE_STATE_DISCONNECTED) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  assert(ctx.gathering_done);
  assert(ctx.candidate_events > 0);
  assert(ctx.remote_state != JUICE_STATE_DISCONNECTED);
  assert(ctx.last_error.empty());
  assert(ctx.last_candidate_event["event"].asString() == "remote_candidate_ready");
  assert(ctx.last_candidate_event["remote_candidates_seen"].asUInt64() >= 1);
  assert(ctx.last_candidate_event["local_candidates_observed"].asUInt64() >= 1);
  assert(!ctx.last_candidate_event["libjuice_state"].asString().empty());
  assert(ctx.last_candidate_event.isMember("transport_connectivity_ready"));
  if (ctx.last_candidate_event["transport_connectivity_ready"].asBool()) {
    assert(!ctx.last_candidate_event["libjuice_selected_local_candidate"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_remote_candidate"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_local_address"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_remote_address"].asString().empty());
  }
  assert(runtime.media_remote_candidates_seen >= 1);
  juice_destroy(remote_agent);
}

}  // namespace

int main() {
  test_embedded_transport_provider_loads_and_answers_remote_offer();
  test_embedded_transport_provider_mirrors_browser_offer_shape_with_dtls_identity();
  test_embedded_transport_provider_reaches_local_ice_connectivity();
  return 0;
}
