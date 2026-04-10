#pragma once

#include "daemon_config.h"
#include "session_voice_signal_protocol.h"
#include "session_voice_signal_session.h"

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace agentd {

struct VoicePeerMediaRuntimePlan;
struct VoicePeerRuntime;
struct VoicePeerRuntimeSeed;

struct VoicePeerBuiltinAudioChunk {
  std::vector<int16_t> pcm_samples;
  int sample_rate_hz = 0;
  int channels = 0;
  int frame_samples_per_channel = 0;
  std::string codec_name;
};

struct VoicePeerMediaEngineInfo {
  std::string media_engine_kind = "browser_peer";
  bool native_media_supported = false;
  bool native_media_active = false;
  int provider_abi_version = 0;
  std::string provider_name;
  std::string provider_version;
  std::string provider_library_path;
  Json::Value provider_capabilities = Json::Value(Json::nullValue);
};

VoicePeerMediaEngineInfo voice_peer_media_engine_info_for_runtime_kind(
  const DaemonConfig& cfg,
  const std::string& runtime_kind
);

std::string builtin_voice_peer_native_library_path(const DaemonConfig& cfg);

bool builtin_voice_peer_native_media_engine_available(
  const DaemonConfig& cfg,
  VoicePeerMediaEngineInfo* out_info,
  std::string* out_err
);

Json::Value voice_peer_media_engine_provider_json(
  const VoicePeerMediaEngineInfo& info,
  bool include_library_path
);

Json::Value builtin_voice_peer_native_media_engine_probe_json(const DaemonConfig& cfg);

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerMediaRuntimePlan* plan
);

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntimeSeed* seed
);

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntime* runtime
);

void set_voice_peer_media_engine_state(
  VoicePeerRuntimeSeed* seed,
  const std::string& state,
  int64_t ts_unix_ms
);

void set_voice_peer_media_engine_state(
  VoicePeerRuntime* runtime,
  const std::string& state,
  int64_t ts_unix_ms
);

void note_voice_peer_media_engine_event(
  VoicePeerRuntime* runtime,
  const Json::Value& payload
);

class VoicePeerBuiltinMediaEngine {
 public:
  virtual ~VoicePeerBuiltinMediaEngine() = default;

  virtual VoicePeerMediaEngineInfo info() const = 0;

  virtual bool initialize(
    VoicePeerRuntime* runtime,
    Json::Value* out_event,
    std::string* out_err
  ) = 0;

  virtual bool handle_remote_description(
    const VoiceBrokerSignalRemoteDescriptionReady& ready,
    VoiceBrokerSignalDescription* out_answer,
    Json::Value* out_event,
    std::string* out_err
  ) = 0;

  virtual bool handle_remote_candidate(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event,
    std::string* out_err
  ) = 0;

  virtual void handle_remote_bye(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event
  ) = 0;

  virtual void handle_local_shutdown(
    Json::Value* out_event
  ) = 0;

  virtual bool poll_status(
    Json::Value* out_event,
    std::string* out_err
  ) = 0;

  virtual bool drain_audio(
    VoicePeerBuiltinAudioChunk* out_chunk,
    Json::Value* out_event,
    std::string* out_err
  ) = 0;

  virtual bool submit_audio(
    const VoicePeerBuiltinAudioChunk& chunk,
    Json::Value* out_event,
    std::string* out_err
  ) = 0;
};

std::unique_ptr<VoicePeerBuiltinMediaEngine> make_builtin_voice_peer_media_engine(
  const DaemonConfig& cfg,
  std::string* out_err
);

}  // namespace agentd
