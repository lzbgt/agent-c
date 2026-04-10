#include "session_voice_builtin_media_engine.h"

#include "session_voice_runtime_internal.h"

#include <juice/juice.h>

#include <cassert>
#include <cstring>
#include <string>

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
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer_event["event"].asString() == "embedded_transport_answer_ready");
  assert(answer_event["remote_description_applied"].asBool());
  assert(answer_event["transport_family"].asString() == "embedded_transport_primitives");
  assert(answer_event["media_engine_state"].asString() == "answer_ready");
  assert(runtime.media_engine_state == "answer_ready");
}

}  // namespace

int main() {
  test_embedded_transport_provider_loads_and_answers_remote_offer();
  return 0;
}
