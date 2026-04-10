#include "session_voice_builtin_media_engine.h"

#include "json_util.h"
#include "session_voice_builtin_media_engine_plugin.h"
#include "session_voice_backend_policy.h"
#include "session_voice_process_plan.h"
#include "session_voice_runtime_internal.h"
#include "string_util.h"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#define AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER 1
#elif defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#define AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER 1
#else
#define AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER 0
#endif

namespace agentd {
namespace {

constexpr size_t kMediaPluginEventBufBytes = 8192;
constexpr size_t kMediaPluginAnswerBufBytes = 4096;
constexpr size_t kMediaPluginErrBufBytes = 1024;

#if AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER
std::string media_plugin_last_error() {
#if defined(_WIN32)
  const DWORD err = GetLastError();
  if (err == 0) return "unknown";
  LPSTR buf = nullptr;
  const DWORD len = FormatMessageA(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr,
    err,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (LPSTR)&buf,
    0,
    nullptr
  );
  std::string out;
  if (len > 0 && buf) out.assign(buf, len);
  if (buf) LocalFree(buf);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out.empty() ? "unknown" : out;
#else
  const char* err = dlerror();
  return std::string(err ? err : "unknown");
#endif
}

void* media_plugin_open(const char* path) {
#if defined(_WIN32)
  if (!path || !path[0]) return nullptr;
  return (void*)LoadLibraryA(path);
#else
  return dlopen(path, RTLD_NOW);
#endif
}

void* media_plugin_symbol(void* handle, const char* name) {
#if defined(_WIN32)
  if (!handle || !name || !name[0]) return nullptr;
  return (void*)GetProcAddress((HMODULE)handle, name);
#else
  return dlsym(handle, name);
#endif
}

void media_plugin_close(void* handle) {
#if defined(_WIN32)
  if (handle) FreeLibrary((HMODULE)handle);
#else
  if (handle) dlclose(handle);
#endif
}
#endif

template <size_t N>
void clear_c_buffer(char (&buf)[N]) {
  std::memset(buf, 0, N);
}

bool parse_media_plugin_event_json(
  const char* raw_json,
  const VoicePeerMediaEngineInfo& info,
  Json::Value* out_event,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_event) *out_event = Json::Value(Json::nullValue);
  const std::string raw = trim_copy(raw_json ? raw_json : "");
  if (raw.empty()) {
    if (out_err) *out_err = "builtin media engine returned empty event payload";
    return false;
  }
  Json::Value payload;
  std::string err;
  if (!json_parse_object(raw, &payload, &err)) {
    if (out_err) {
      *out_err = err.empty()
        ? "builtin media engine returned invalid event payload"
        : "builtin media engine returned invalid event payload: " + err;
    }
    return false;
  }
  if (!payload.isMember("media_engine_kind")) payload["media_engine_kind"] = info.media_engine_kind;
  if (!payload.isMember("native_media_supported")) payload["native_media_supported"] = info.native_media_supported;
  if (!payload.isMember("native_media_active")) payload["native_media_active"] = info.native_media_active;
  if (out_event) *out_event = payload;
  return true;
}

struct VoiceMediaPluginProbe {
  void* handle = nullptr;
  const agentd_voice_media_engine_provider_v1* api = nullptr;
  VoicePeerMediaEngineInfo info;
};

bool probe_builtin_voice_peer_native_media_engine_impl(
  const DaemonConfig& cfg,
  VoiceMediaPluginProbe* out_probe,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_probe) *out_probe = VoiceMediaPluginProbe{};
  const std::string library_path = trim_copy(cfg.audio_webrtc_builtin_native_library_path);
  if (library_path.empty()) {
    if (out_err) *out_err = "audio_webrtc.builtin_native_library_path not configured";
    return false;
  }
#if !AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER
  if (out_err) *out_err = "builtin native media engine loader unsupported on this platform";
  return false;
#else
  void* handle = media_plugin_open(library_path.c_str());
  if (!handle) {
    if (out_err) *out_err = "failed to load builtin native media engine library: " + media_plugin_last_error();
    return false;
  }

  const auto get_api = reinterpret_cast<agentd_voice_media_engine_get_api_v1_fn>(
    media_plugin_symbol(handle, AGENTD_VOICE_MEDIA_ENGINE_GET_API_V1_SYMBOL));
  if (!get_api) {
    if (out_err) {
      *out_err = "builtin native media engine missing " AGENTD_VOICE_MEDIA_ENGINE_GET_API_V1_SYMBOL;
    }
    media_plugin_close(handle);
    return false;
  }

  const agentd_voice_media_engine_provider_v1* api = get_api();
  if (!api) {
    if (out_err) *out_err = "builtin native media engine returned null API";
    media_plugin_close(handle);
    return false;
  }
  if (api->abi_version != AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V1) {
    if (out_err) *out_err = "builtin native media engine ABI mismatch";
    media_plugin_close(handle);
    return false;
  }
  if (!api->create || !api->destroy || !api->initialize || !api->handle_remote_description ||
      !api->handle_remote_candidate || !api->handle_remote_bye || !api->handle_local_shutdown) {
    if (out_err) *out_err = "builtin native media engine missing required callbacks";
    media_plugin_close(handle);
    return false;
  }

  const std::string kind = trim_copy(api->media_engine_kind ? api->media_engine_kind : "");
  if (kind != "builtin_native_plugin") {
    if (out_err) *out_err = "builtin native media engine reported unsupported media_engine_kind";
    media_plugin_close(handle);
    return false;
  }

  if (out_probe) {
    out_probe->handle = handle;
    out_probe->api = api;
    out_probe->info.media_engine_kind = kind;
    out_probe->info.native_media_supported = api->native_media_supported != 0;
    out_probe->info.native_media_active = false;
    return true;
  }

  media_plugin_close(handle);
  return true;
#endif
}

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Json::Value make_engine_event(
  const VoicePeerMediaEngineInfo& info,
  const std::string& event,
  const std::string& state
) {
  Json::Value payload(Json::objectValue);
  payload["ok"] = true;
  payload["event"] = event;
  payload["media_engine_kind"] = info.media_engine_kind;
  payload["media_engine_state"] = state;
  payload["native_media_supported"] = info.native_media_supported;
  payload["native_media_active"] = info.native_media_active;
  return payload;
}

class BuiltinVoicePeerSignalingStubEngine final : public VoicePeerBuiltinMediaEngine {
 public:
  VoicePeerMediaEngineInfo info() const override {
    VoicePeerMediaEngineInfo out;
    out.media_engine_kind = "builtin_signaling_stub";
    out.native_media_supported = false;
    out.native_media_active = false;
    return out;
  }

  bool initialize(
    VoicePeerRuntime* runtime,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (runtime) apply_voice_peer_media_engine_info(info(), runtime);
    if (out_event) *out_event = make_engine_event(info(), "media_engine_initialized", "signaling_ready");
    return true;
  }

  bool handle_remote_description(
    const VoiceBrokerSignalRemoteDescriptionReady& ready,
    VoiceBrokerSignalDescription* out_answer,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    const std::string desc_type = lower_copy(trim_copy(ready.description.type));
    if (!desc_type.empty() && desc_type != "offer") {
      if (out_err) *out_err = "expected remote offer, got " + desc_type;
      return false;
    }

    if (out_answer) {
      out_answer->type = "answer";
      out_answer->sdp = "stub-answer";
    }
    if (out_event) {
      Json::Value payload = make_engine_event(info(), "stub_answer_ready", "answer_ready");
      payload["initial_remote_candidate_count"] = Json::UInt64(ready.initial_remote_candidates.size());
      *out_event = payload;
    }
    return true;
  }

  bool handle_remote_candidate(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (out_event) {
      Json::Value payload = make_engine_event(info(), "remote_candidate_ready", "signaling_active");
      payload["candidate"] = ingress.candidate.candidate;
      if (!trim_copy(ingress.candidate.sdp_mid).empty()) payload["sdpMid"] = ingress.candidate.sdp_mid;
      if (ingress.candidate.has_sdp_mline_index) payload["sdpMLineIndex"] = ingress.candidate.sdp_mline_index;
      *out_event = payload;
    }
    return true;
  }

  void handle_remote_bye(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event
  ) override {
    if (!out_event) return;
    Json::Value payload = make_engine_event(info(), "remote_bye", "stopped");
    if (!trim_copy(ingress.bye.reason).empty()) payload["reason"] = ingress.bye.reason;
    *out_event = payload;
  }

  void handle_local_shutdown(
    Json::Value* out_event
  ) override {
    if (!out_event) return;
    Json::Value payload = make_engine_event(info(), "local_bye_sent", "stopping");
    payload["reason"] = "agentd_builtin_stop";
    *out_event = payload;
  }
};

class BuiltinVoicePeerNativePluginEngine final : public VoicePeerBuiltinMediaEngine {
 public:
  BuiltinVoicePeerNativePluginEngine(
    void* handle,
    const agentd_voice_media_engine_provider_v1* api,
    const VoicePeerMediaEngineInfo& info
  )
      : handle_(handle), api_(api), info_(info) {}

  ~BuiltinVoicePeerNativePluginEngine() override {
    if (api_ && api_->destroy && instance_) api_->destroy(instance_);
#if AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER
    if (handle_) media_plugin_close(handle_);
#endif
  }

  bool create_instance(std::string* out_err) {
    if (out_err) out_err->clear();
    if (instance_) return true;
    char err_buf[kMediaPluginErrBufBytes];
    clear_c_buffer(err_buf);
    void* instance = nullptr;
    if (!api_->create(&instance, err_buf, sizeof(err_buf)) || !instance) {
      if (out_err) {
        *out_err = trim_copy(err_buf).empty()
          ? "builtin native media engine create failed"
          : trim_copy(err_buf);
      }
      return false;
    }
    instance_ = instance;
    return true;
  }

  VoicePeerMediaEngineInfo info() const override {
    return info_;
  }

  bool initialize(
    VoicePeerRuntime* runtime,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (runtime) apply_voice_peer_media_engine_info(info_, runtime);
    char event_buf[kMediaPluginEventBufBytes];
    char err_buf[kMediaPluginErrBufBytes];
    clear_c_buffer(event_buf);
    clear_c_buffer(err_buf);
    if (!api_->initialize(instance_, event_buf, sizeof(event_buf), err_buf, sizeof(err_buf))) {
      if (out_err) {
        *out_err = trim_copy(err_buf).empty()
          ? "builtin native media engine initialize failed"
          : trim_copy(err_buf);
      }
      return false;
    }
    return parse_media_plugin_event_json(event_buf, info_, out_event, out_err);
  }

  bool handle_remote_description(
    const VoiceBrokerSignalRemoteDescriptionReady& ready,
    VoiceBrokerSignalDescription* out_answer,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    char answer_type[kMediaPluginAnswerBufBytes];
    char answer_sdp[kMediaPluginAnswerBufBytes];
    char event_buf[kMediaPluginEventBufBytes];
    char err_buf[kMediaPluginErrBufBytes];
    clear_c_buffer(answer_type);
    clear_c_buffer(answer_sdp);
    clear_c_buffer(event_buf);
    clear_c_buffer(err_buf);
    if (!api_->handle_remote_description(
          instance_,
          ready.description.type.c_str(),
          ready.description.sdp.c_str(),
          static_cast<uint64_t>(ready.initial_remote_candidates.size()),
          answer_type,
          sizeof(answer_type),
          answer_sdp,
          sizeof(answer_sdp),
          event_buf,
          sizeof(event_buf),
          err_buf,
          sizeof(err_buf))) {
      if (out_err) {
        *out_err = trim_copy(err_buf).empty()
          ? "builtin native media engine remote description failed"
          : trim_copy(err_buf);
      }
      return false;
    }
    if (out_answer) {
      out_answer->type = trim_copy(answer_type);
      out_answer->sdp = trim_copy(answer_sdp);
    }
    return parse_media_plugin_event_json(event_buf, info_, out_event, out_err);
  }

  bool handle_remote_candidate(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    char event_buf[kMediaPluginEventBufBytes];
    char err_buf[kMediaPluginErrBufBytes];
    clear_c_buffer(event_buf);
    clear_c_buffer(err_buf);
    if (!api_->handle_remote_candidate(
          instance_,
          ingress.candidate.candidate.c_str(),
          ingress.candidate.sdp_mid.c_str(),
          ingress.candidate.sdp_mline_index,
          ingress.candidate.has_sdp_mline_index ? 1 : 0,
          event_buf,
          sizeof(event_buf),
          err_buf,
          sizeof(err_buf))) {
      if (out_err) {
        *out_err = trim_copy(err_buf).empty()
          ? "builtin native media engine remote candidate failed"
          : trim_copy(err_buf);
      }
      return false;
    }
    return parse_media_plugin_event_json(event_buf, info_, out_event, out_err);
  }

  void handle_remote_bye(
    const VoiceBrokerSignalIngress& ingress,
    Json::Value* out_event
  ) override {
    char event_buf[kMediaPluginEventBufBytes];
    clear_c_buffer(event_buf);
    api_->handle_remote_bye(instance_, ingress.bye.reason.c_str(), event_buf, sizeof(event_buf));
    std::string ignored_err;
    parse_media_plugin_event_json(event_buf, info_, out_event, &ignored_err);
  }

  void handle_local_shutdown(
    Json::Value* out_event
  ) override {
    char event_buf[kMediaPluginEventBufBytes];
    clear_c_buffer(event_buf);
    api_->handle_local_shutdown(instance_, event_buf, sizeof(event_buf));
    std::string ignored_err;
    parse_media_plugin_event_json(event_buf, info_, out_event, &ignored_err);
  }

 private:
  void* handle_ = nullptr;
  const agentd_voice_media_engine_provider_v1* api_ = nullptr;
  void* instance_ = nullptr;
  VoicePeerMediaEngineInfo info_;
};

}  // namespace

std::string builtin_voice_peer_native_library_path(const DaemonConfig& cfg) {
  return trim_copy(cfg.audio_webrtc_builtin_native_library_path);
}

bool builtin_voice_peer_native_media_engine_available(
  const DaemonConfig& cfg,
  VoicePeerMediaEngineInfo* out_info,
  std::string* out_err
) {
  VoiceMediaPluginProbe probe;
  const bool ok = probe_builtin_voice_peer_native_media_engine_impl(cfg, &probe, out_err);
  if (ok && out_info) *out_info = probe.info;
#if AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER
  if (probe.handle) media_plugin_close(probe.handle);
#endif
  return ok;
}

VoicePeerMediaEngineInfo voice_peer_media_engine_info_for_runtime_kind(
  const DaemonConfig& cfg,
  const std::string& runtime_kind
) {
  VoicePeerMediaEngineInfo out;
  const std::string kind = lower_copy(trim_copy(runtime_kind));
  if (kind == "bundled" || kind == "external") {
    out.media_engine_kind = "browser_peer";
    return out;
  }
  if (kind == "builtin") {
    if (voice_peer_builtin_runtime_mode(cfg) == "signaling_stub") {
      out.media_engine_kind = "builtin_signaling_stub";
      return out;
    }
    if (voice_peer_builtin_runtime_mode(cfg) == "native_plugin") {
      out.media_engine_kind = "builtin_native_plugin";
      VoicePeerMediaEngineInfo native_info;
      std::string ignored_err;
      if (builtin_voice_peer_native_media_engine_available(cfg, &native_info, &ignored_err)) {
        out = native_info;
      }
      return out;
    }
    out.media_engine_kind = "builtin_reserved";
    return out;
  }
  out.media_engine_kind = "unknown";
  return out;
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerMediaRuntimePlan* plan
) {
  if (!plan) return;
  plan->media_engine_kind = info.media_engine_kind;
  plan->native_media_supported = info.native_media_supported;
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntimeSeed* seed
) {
  if (!seed) return;
  seed->media_engine_kind = info.media_engine_kind;
  seed->native_media_supported = info.native_media_supported;
  seed->native_media_active = info.native_media_active;
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntime* runtime
) {
  if (!runtime) return;
  runtime->media_engine_kind = info.media_engine_kind;
  runtime->native_media_supported = info.native_media_supported;
  runtime->native_media_active = info.native_media_active;
}

void set_voice_peer_media_engine_state(
  VoicePeerRuntimeSeed* seed,
  const std::string& state,
  int64_t ts_unix_ms
) {
  if (!seed) return;
  seed->media_engine_state = trim_copy(state).empty() ? "idle" : trim_copy(state);
  seed->media_state_updated_unix_ms = ts_unix_ms;
}

void set_voice_peer_media_engine_state(
  VoicePeerRuntime* runtime,
  const std::string& state,
  int64_t ts_unix_ms
) {
  if (!runtime) return;
  runtime->media_engine_state = trim_copy(state).empty() ? "idle" : trim_copy(state);
  runtime->media_state_updated_unix_ms = ts_unix_ms > 0 ? ts_unix_ms : now_unix_ms();
}

void note_voice_peer_media_engine_event(
  VoicePeerRuntime* runtime,
  const Json::Value& payload
) {
  if (!runtime || !payload.isObject()) return;
  const std::string event =
    payload.isMember("event") && payload["event"].isString()
      ? lower_copy(trim_copy(payload["event"].asString()))
      : std::string();
  const std::string explicit_state =
    payload.isMember("media_engine_state") && payload["media_engine_state"].isString()
      ? trim_copy(payload["media_engine_state"].asString())
      : std::string();
  const int64_t ts =
    payload.isMember("ts_unix_ms") &&
        (payload["ts_unix_ms"].isInt64() || payload["ts_unix_ms"].isUInt64())
      ? payload["ts_unix_ms"].asInt64()
      : now_unix_ms();

  if (payload.isMember("native_media_supported") && payload["native_media_supported"].isBool()) {
    runtime->native_media_supported = payload["native_media_supported"].asBool();
  }
  if (payload.isMember("native_media_active") && payload["native_media_active"].isBool()) {
    runtime->native_media_active = payload["native_media_active"].asBool();
  }

  runtime->media_events_total += 1;
  if (event == "remote_offer_seen") runtime->media_remote_offers_seen += 1;
  if (event == "stub_answer_sent" || event == "answer_sent") runtime->media_answers_sent += 1;
  if (event == "remote_candidate_ready") runtime->media_remote_candidates_seen += 1;
  if (event == "remote_bye") runtime->media_remote_byes_seen += 1;
  if (event == "local_bye_sent") runtime->media_local_byes_sent += 1;

  if (!explicit_state.empty()) {
    runtime->media_engine_state = explicit_state;
    runtime->media_state_updated_unix_ms = ts;
    return;
  }

  if (event == "builtin_runtime_started") {
    runtime->media_engine_state = "signaling_ready";
    runtime->media_state_updated_unix_ms = ts;
  } else if (event == "builtin_runtime_stopped") {
    runtime->media_engine_state = "stopped";
    runtime->media_state_updated_unix_ms = ts;
  } else if (event == "builtin_runtime_failed") {
    runtime->media_engine_state = "failed";
    runtime->media_state_updated_unix_ms = ts;
  }
}

std::unique_ptr<VoicePeerBuiltinMediaEngine> make_builtin_voice_peer_media_engine(
  const DaemonConfig& cfg,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (voice_peer_builtin_runtime_mode(cfg) == "signaling_stub") {
    return std::make_unique<BuiltinVoicePeerSignalingStubEngine>();
  }
  if (voice_peer_builtin_runtime_mode(cfg) == "native_plugin") {
    VoiceMediaPluginProbe probe;
    std::string probe_err;
    if (!probe_builtin_voice_peer_native_media_engine_impl(cfg, &probe, &probe_err)) {
      if (out_err) *out_err = probe_err;
      return nullptr;
    }
    auto engine = std::make_unique<BuiltinVoicePeerNativePluginEngine>(probe.handle, probe.api, probe.info);
    probe.handle = nullptr;
    if (!engine->create_instance(out_err)) return nullptr;
    return engine;
  }
  if (out_err) *out_err = "builtin voice_webrtc_peer runtime disabled";
  return nullptr;
}

}  // namespace agentd
