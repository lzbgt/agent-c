#include "session_voice_builtin_media_engine.h"

#include "json_util.h"
#include "session_voice_builtin_media_engine_plugin.h"
#include "session_voice_backend_policy.h"
#include "session_voice_process_plan.h"
#include "session_voice_runtime_internal.h"
#include "string_util.h"

#include <chrono>
#include <cstring>
#include <filesystem>

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

bool json_object_is_empty(const Json::Value& v) {
  return !v.isObject() || v.getMemberNames().empty();
}

std::string provider_name_from_library_path(const std::string& library_path) {
  if (library_path.empty()) return "";
  return trim_copy(std::filesystem::path(library_path).filename().string());
}

bool parse_provider_capabilities_json(
  const char* raw_json,
  Json::Value* out_caps,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_caps) *out_caps = Json::Value(Json::nullValue);
  const std::string raw = trim_copy(raw_json ? raw_json : "");
  if (raw.empty()) return true;
  Json::Value caps;
  std::string err;
  if (!json_parse_object(raw, &caps, &err)) {
    if (out_err) {
      *out_err = err.empty()
        ? "builtin native media engine provider_capabilities_json must be a JSON object"
        : "builtin native media engine provider_capabilities_json invalid: " + err;
    }
    return false;
  }
  if (out_caps) *out_caps = caps;
  return true;
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
  if (!payload.isMember("native_media_provider")) {
    const Json::Value provider = voice_peer_media_engine_provider_json(info, true);
    if (provider.isObject()) payload["native_media_provider"] = provider;
  }
  if (out_event) *out_event = payload;
  return true;
}

struct VoiceMediaPluginApiBridge {
  uint32_t abi_version = 0;
  int (*create)(void** out_instance, char* err_buf, size_t err_buf_size) = nullptr;
  void (*destroy)(void* instance) = nullptr;
  int (*initialize)(
    void* instance,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  ) = nullptr;
  int (*handle_remote_description)(
    void* instance,
    const char* description_type,
    const char* description_sdp,
    uint64_t initial_remote_candidate_count,
    char* answer_type_buf,
    size_t answer_type_buf_size,
    char* answer_sdp_buf,
    size_t answer_sdp_buf_size,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  ) = nullptr;
  int (*handle_remote_candidate)(
    void* instance,
    const char* candidate,
    const char* sdp_mid,
    int sdp_mline_index,
    int has_sdp_mline_index,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  ) = nullptr;
  void (*handle_remote_bye)(
    void* instance,
    const char* reason,
    char* event_json_buf,
    size_t event_json_buf_size
  ) = nullptr;
  void (*handle_local_shutdown)(
    void* instance,
    char* event_json_buf,
    size_t event_json_buf_size
  ) = nullptr;
  int (*poll_status)(
    void* instance,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  ) = nullptr;
};

struct VoiceMediaPluginProbe {
  void* handle = nullptr;
  VoiceMediaPluginApiBridge api;
  VoicePeerMediaEngineInfo info;
};

bool validate_voice_media_plugin_bridge(
  const VoiceMediaPluginApiBridge& api,
  std::string* out_err
) {
  if (!api.create || !api.destroy || !api.initialize || !api.handle_remote_description ||
      !api.handle_remote_candidate || !api.handle_remote_bye || !api.handle_local_shutdown) {
    if (out_err) *out_err = "builtin native media engine missing required callbacks";
    return false;
  }
  return true;
}

bool validate_voice_media_plugin_bridge_v3(
  const VoiceMediaPluginApiBridge& api,
  std::string* out_err
) {
  if (!validate_voice_media_plugin_bridge(api, out_err)) return false;
  if (!api.poll_status) {
    if (out_err) *out_err = "builtin native media engine missing required poll_status callback";
    return false;
  }
  return true;
}

bool populate_probe_from_api_v3(
  const std::string& library_path,
  const agentd_voice_media_engine_provider_v3* api,
  VoiceMediaPluginProbe* out_probe,
  std::string* out_err
) {
  if (!api) {
    if (out_err) *out_err = "builtin native media engine returned null v3 API";
    return false;
  }
  if (api->abi_version != AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V3) {
    if (out_err) *out_err = "builtin native media engine ABI v3 mismatch";
    return false;
  }
  const std::string kind = trim_copy(api->media_engine_kind ? api->media_engine_kind : "");
  if (kind != "builtin_native_plugin") {
    if (out_err) *out_err = "builtin native media engine reported unsupported media_engine_kind";
    return false;
  }
  const std::string provider_name = trim_copy(api->provider_name ? api->provider_name : "");
  if (provider_name.empty()) {
    if (out_err) *out_err = "builtin native media engine missing provider_name";
    return false;
  }
  Json::Value capabilities(Json::nullValue);
  if (!parse_provider_capabilities_json(api->provider_capabilities_json, &capabilities, out_err)) {
    return false;
  }

  VoiceMediaPluginApiBridge bridge;
  bridge.abi_version = api->abi_version;
  bridge.create = api->create;
  bridge.destroy = api->destroy;
  bridge.initialize = api->initialize;
  bridge.handle_remote_description = api->handle_remote_description;
  bridge.handle_remote_candidate = api->handle_remote_candidate;
  bridge.handle_remote_bye = api->handle_remote_bye;
  bridge.handle_local_shutdown = api->handle_local_shutdown;
  bridge.poll_status = api->poll_status;
  if (!validate_voice_media_plugin_bridge_v3(bridge, out_err)) return false;

  if (out_probe) {
    out_probe->api = bridge;
    out_probe->info.media_engine_kind = kind;
    out_probe->info.native_media_supported = api->native_media_supported != 0;
    out_probe->info.native_media_active = false;
    out_probe->info.provider_abi_version = static_cast<int>(api->abi_version);
    out_probe->info.provider_name = provider_name;
    out_probe->info.provider_version = trim_copy(api->provider_version ? api->provider_version : "");
    out_probe->info.provider_library_path = library_path;
    out_probe->info.provider_capabilities = capabilities;
  }
  return true;
}

bool populate_probe_from_api_v2(
  const std::string& library_path,
  const agentd_voice_media_engine_provider_v2* api,
  VoiceMediaPluginProbe* out_probe,
  std::string* out_err
) {
  if (!api) {
    if (out_err) *out_err = "builtin native media engine returned null v2 API";
    return false;
  }
  if (api->abi_version != AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V2) {
    if (out_err) *out_err = "builtin native media engine ABI v2 mismatch";
    return false;
  }
  const std::string kind = trim_copy(api->media_engine_kind ? api->media_engine_kind : "");
  if (kind != "builtin_native_plugin") {
    if (out_err) *out_err = "builtin native media engine reported unsupported media_engine_kind";
    return false;
  }
  const std::string provider_name = trim_copy(api->provider_name ? api->provider_name : "");
  if (provider_name.empty()) {
    if (out_err) *out_err = "builtin native media engine missing provider_name";
    return false;
  }
  Json::Value capabilities(Json::nullValue);
  if (!parse_provider_capabilities_json(api->provider_capabilities_json, &capabilities, out_err)) {
    return false;
  }

  VoiceMediaPluginApiBridge bridge;
  bridge.abi_version = api->abi_version;
  bridge.create = api->create;
  bridge.destroy = api->destroy;
  bridge.initialize = api->initialize;
  bridge.handle_remote_description = api->handle_remote_description;
  bridge.handle_remote_candidate = api->handle_remote_candidate;
  bridge.handle_remote_bye = api->handle_remote_bye;
  bridge.handle_local_shutdown = api->handle_local_shutdown;
  if (!validate_voice_media_plugin_bridge(bridge, out_err)) return false;

  if (out_probe) {
    out_probe->api = bridge;
    out_probe->info.media_engine_kind = kind;
    out_probe->info.native_media_supported = api->native_media_supported != 0;
    out_probe->info.native_media_active = false;
    out_probe->info.provider_abi_version = static_cast<int>(api->abi_version);
    out_probe->info.provider_name = provider_name;
    out_probe->info.provider_version = trim_copy(api->provider_version ? api->provider_version : "");
    out_probe->info.provider_library_path = library_path;
    out_probe->info.provider_capabilities = capabilities;
  }
  return true;
}

bool populate_probe_from_api_v1(
  const std::string& library_path,
  const agentd_voice_media_engine_provider_v1* api,
  VoiceMediaPluginProbe* out_probe,
  std::string* out_err
) {
  if (!api) {
    if (out_err) *out_err = "builtin native media engine returned null v1 API";
    return false;
  }
  if (api->abi_version != AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V1) {
    if (out_err) *out_err = "builtin native media engine ABI v1 mismatch";
    return false;
  }
  const std::string kind = trim_copy(api->media_engine_kind ? api->media_engine_kind : "");
  if (kind != "builtin_native_plugin") {
    if (out_err) *out_err = "builtin native media engine reported unsupported media_engine_kind";
    return false;
  }

  VoiceMediaPluginApiBridge bridge;
  bridge.abi_version = api->abi_version;
  bridge.create = api->create;
  bridge.destroy = api->destroy;
  bridge.initialize = api->initialize;
  bridge.handle_remote_description = api->handle_remote_description;
  bridge.handle_remote_candidate = api->handle_remote_candidate;
  bridge.handle_remote_bye = api->handle_remote_bye;
  bridge.handle_local_shutdown = api->handle_local_shutdown;
  if (!validate_voice_media_plugin_bridge(bridge, out_err)) return false;

  if (out_probe) {
    Json::Value caps(Json::objectValue);
    caps["legacy_abi_v1"] = true;
    out_probe->api = bridge;
    out_probe->info.media_engine_kind = kind;
    out_probe->info.native_media_supported = api->native_media_supported != 0;
    out_probe->info.native_media_active = false;
    out_probe->info.provider_abi_version = static_cast<int>(api->abi_version);
    out_probe->info.provider_name = provider_name_from_library_path(library_path);
    if (out_probe->info.provider_name.empty()) out_probe->info.provider_name = "legacy_builtin_native_plugin";
    out_probe->info.provider_version = "legacy_abi_v1";
    out_probe->info.provider_library_path = library_path;
    out_probe->info.provider_capabilities = caps;
  }
  return true;
}

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

  const auto get_api_v3 = reinterpret_cast<agentd_voice_media_engine_get_api_v3_fn>(
    media_plugin_symbol(handle, AGENTD_VOICE_MEDIA_ENGINE_GET_API_V3_SYMBOL));
  if (get_api_v3) {
    VoiceMediaPluginProbe probe;
    if (!populate_probe_from_api_v3(library_path, get_api_v3(), &probe, out_err)) {
      media_plugin_close(handle);
      return false;
    }
    if (out_probe) {
      *out_probe = probe;
      out_probe->handle = handle;
      return true;
    }
    media_plugin_close(handle);
    return true;
  }

  const auto get_api_v2 = reinterpret_cast<agentd_voice_media_engine_get_api_v2_fn>(
    media_plugin_symbol(handle, AGENTD_VOICE_MEDIA_ENGINE_GET_API_V2_SYMBOL));
  if (get_api_v2) {
    VoiceMediaPluginProbe probe;
    if (!populate_probe_from_api_v2(library_path, get_api_v2(), &probe, out_err)) {
      media_plugin_close(handle);
      return false;
    }
    if (out_probe) {
      *out_probe = probe;
      out_probe->handle = handle;
      return true;
    }
    media_plugin_close(handle);
    return true;
  }

  const auto get_api_v1 = reinterpret_cast<agentd_voice_media_engine_get_api_v1_fn>(
    media_plugin_symbol(handle, AGENTD_VOICE_MEDIA_ENGINE_GET_API_V1_SYMBOL));
  if (!get_api_v1) {
    if (out_err) {
      *out_err =
        "builtin native media engine missing " AGENTD_VOICE_MEDIA_ENGINE_GET_API_V3_SYMBOL
        ", " AGENTD_VOICE_MEDIA_ENGINE_GET_API_V2_SYMBOL
        " and " AGENTD_VOICE_MEDIA_ENGINE_GET_API_V1_SYMBOL;
    }
    media_plugin_close(handle);
    return false;
  }
  VoiceMediaPluginProbe probe;
  if (!populate_probe_from_api_v1(library_path, get_api_v1(), &probe, out_err)) {
    media_plugin_close(handle);
    return false;
  }
  if (out_probe) {
    *out_probe = probe;
    out_probe->handle = handle;
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
  const Json::Value provider = voice_peer_media_engine_provider_json(info, true);
  if (provider.isObject()) payload["native_media_provider"] = provider;
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

  bool poll_status(
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (out_event) *out_event = Json::Value(Json::nullValue);
    return true;
  }
};

class BuiltinVoicePeerNativePluginEngine final : public VoicePeerBuiltinMediaEngine {
 public:
  BuiltinVoicePeerNativePluginEngine(
    void* handle,
    const VoiceMediaPluginApiBridge& api,
    const VoicePeerMediaEngineInfo& info
  )
      : handle_(handle), api_(api), info_(info) {}

  ~BuiltinVoicePeerNativePluginEngine() override {
    if (api_.destroy && instance_) api_.destroy(instance_);
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
    if (!api_.create(&instance, err_buf, sizeof(err_buf)) || !instance) {
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
    if (!api_.initialize(instance_, event_buf, sizeof(event_buf), err_buf, sizeof(err_buf))) {
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
    if (!api_.handle_remote_description(
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
    if (!api_.handle_remote_candidate(
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
    api_.handle_remote_bye(instance_, ingress.bye.reason.c_str(), event_buf, sizeof(event_buf));
    std::string ignored_err;
    parse_media_plugin_event_json(event_buf, info_, out_event, &ignored_err);
  }

  void handle_local_shutdown(
    Json::Value* out_event
  ) override {
    char event_buf[kMediaPluginEventBufBytes];
    clear_c_buffer(event_buf);
    api_.handle_local_shutdown(instance_, event_buf, sizeof(event_buf));
    std::string ignored_err;
    parse_media_plugin_event_json(event_buf, info_, out_event, &ignored_err);
  }

  bool poll_status(
    Json::Value* out_event,
    std::string* out_err
  ) override {
    if (out_err) out_err->clear();
    if (out_event) *out_event = Json::Value(Json::nullValue);
    if (!api_.poll_status) return true;
    char event_buf[kMediaPluginEventBufBytes];
    char err_buf[kMediaPluginErrBufBytes];
    clear_c_buffer(event_buf);
    clear_c_buffer(err_buf);
    if (!api_.poll_status(instance_, event_buf, sizeof(event_buf), err_buf, sizeof(err_buf))) {
      if (out_err) {
        *out_err = trim_copy(err_buf).empty()
          ? "builtin native media engine poll_status failed"
          : trim_copy(err_buf);
      }
      return false;
    }
    if (trim_copy(event_buf).empty()) return true;
    return parse_media_plugin_event_json(event_buf, info_, out_event, out_err);
  }

 private:
  void* handle_ = nullptr;
  VoiceMediaPluginApiBridge api_;
  void* instance_ = nullptr;
  VoicePeerMediaEngineInfo info_;
};

}  // namespace

std::string builtin_voice_peer_native_library_path(const DaemonConfig& cfg) {
  return trim_copy(cfg.audio_webrtc_builtin_native_library_path);
}

Json::Value voice_peer_media_engine_provider_json(
  const VoicePeerMediaEngineInfo& info,
  bool include_library_path
) {
  Json::Value out(Json::objectValue);
  if (info.provider_abi_version > 0) out["abi_version"] = info.provider_abi_version;
  if (!trim_copy(info.provider_name).empty()) out["name"] = info.provider_name;
  if (!trim_copy(info.provider_version).empty()) out["version"] = info.provider_version;
  if (include_library_path && !trim_copy(info.provider_library_path).empty()) {
    out["library_path"] = info.provider_library_path;
  }
  if (info.provider_capabilities.isObject()) out["capabilities"] = info.provider_capabilities;
  return json_object_is_empty(out) ? Json::Value(Json::nullValue) : out;
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

Json::Value builtin_voice_peer_native_media_engine_probe_json(const DaemonConfig& cfg) {
  Json::Value out(Json::objectValue);
  const std::string library_path = builtin_voice_peer_native_library_path(cfg);
  out["configured"] = !library_path.empty();
  if (!library_path.empty()) out["library_path"] = library_path;

  VoiceMediaPluginProbe probe;
  std::string err;
  const bool ok = probe_builtin_voice_peer_native_media_engine_impl(cfg, &probe, &err);
  out["loadable"] = ok;
  if (ok) {
    out["media_engine_kind"] = probe.info.media_engine_kind;
    out["native_media_supported"] = probe.info.native_media_supported;
    const Json::Value provider = voice_peer_media_engine_provider_json(probe.info, true);
    if (provider.isObject()) out["provider"] = provider;
  } else if (!trim_copy(err).empty()) {
    out["error"] = err;
  }
#if AGENTD_HAVE_VOICE_MEDIA_PLUGIN_LOADER
  if (probe.handle) media_plugin_close(probe.handle);
#endif
  return out;
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
  plan->native_media_provider = voice_peer_media_engine_provider_json(info, true);
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntimeSeed* seed
) {
  if (!seed) return;
  seed->media_engine_kind = info.media_engine_kind;
  seed->native_media_supported = info.native_media_supported;
  seed->native_media_active = info.native_media_active;
  seed->native_media_provider = voice_peer_media_engine_provider_json(info, true);
}

void apply_voice_peer_media_engine_info(
  const VoicePeerMediaEngineInfo& info,
  VoicePeerRuntime* runtime
) {
  if (!runtime) return;
  runtime->media_engine_kind = info.media_engine_kind;
  runtime->native_media_supported = info.native_media_supported;
  runtime->native_media_active = info.native_media_active;
  runtime->native_media_provider = voice_peer_media_engine_provider_json(info, true);
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
  if (payload.isMember("dtls_identity_ready") && payload["dtls_identity_ready"].isBool()) {
    runtime->dtls_identity_ready = payload["dtls_identity_ready"].asBool();
  }
  if (payload.isMember("dtls_handshake_ready") && payload["dtls_handshake_ready"].isBool()) {
    runtime->dtls_handshake_ready = payload["dtls_handshake_ready"].asBool();
  }
  if (payload.isMember("dtls_exporter_ready") && payload["dtls_exporter_ready"].isBool()) {
    runtime->dtls_exporter_ready = payload["dtls_exporter_ready"].asBool();
  }
  if (payload.isMember("srtp_contexts_ready") && payload["srtp_contexts_ready"].isBool()) {
    runtime->srtp_contexts_ready = payload["srtp_contexts_ready"].asBool();
  }
  if (payload.isMember("srtp_inbound_ready") && payload["srtp_inbound_ready"].isBool()) {
    runtime->srtp_inbound_ready = payload["srtp_inbound_ready"].asBool();
  }
  if (payload.isMember("srtp_outbound_ready") && payload["srtp_outbound_ready"].isBool()) {
    runtime->srtp_outbound_ready = payload["srtp_outbound_ready"].asBool();
  }
  if (payload.isMember("dtls_fingerprint_sha256") && payload["dtls_fingerprint_sha256"].isString()) {
    runtime->dtls_fingerprint_sha256 = trim_copy(payload["dtls_fingerprint_sha256"].asString());
  }
  if (payload.isMember("dtls_setup_role") && payload["dtls_setup_role"].isString()) {
    runtime->dtls_setup_role = trim_copy(payload["dtls_setup_role"].asString());
  }
  if (payload.isMember("dtls_certificate_subject") && payload["dtls_certificate_subject"].isString()) {
    runtime->dtls_certificate_subject = trim_copy(payload["dtls_certificate_subject"].asString());
  }
  if (payload.isMember("dtls_handshake_state") && payload["dtls_handshake_state"].isString()) {
    runtime->dtls_handshake_state = trim_copy(payload["dtls_handshake_state"].asString());
  }
  if (payload.isMember("dtls_selected_srtp_profile") && payload["dtls_selected_srtp_profile"].isString()) {
    runtime->dtls_selected_srtp_profile = trim_copy(payload["dtls_selected_srtp_profile"].asString());
  }
  if (payload.isMember("srtp_last_error") && payload["srtp_last_error"].isString()) {
    runtime->srtp_last_error = trim_copy(payload["srtp_last_error"].asString());
  }
  if (payload.isMember("dtls_packets_sent") &&
      (payload["dtls_packets_sent"].isInt64() || payload["dtls_packets_sent"].isUInt64())) {
    runtime->dtls_packets_sent = payload["dtls_packets_sent"].asInt64();
  }
  if (payload.isMember("dtls_packets_received") &&
      (payload["dtls_packets_received"].isInt64() || payload["dtls_packets_received"].isUInt64())) {
    runtime->dtls_packets_received = payload["dtls_packets_received"].asInt64();
  }
  if (payload.isMember("rtp_packets_received") &&
      (payload["rtp_packets_received"].isInt64() || payload["rtp_packets_received"].isUInt64())) {
    runtime->rtp_packets_received = payload["rtp_packets_received"].asInt64();
  }
  if (payload.isMember("rtp_payload_bytes_received") &&
      (payload["rtp_payload_bytes_received"].isInt64() || payload["rtp_payload_bytes_received"].isUInt64())) {
    runtime->rtp_payload_bytes_received = payload["rtp_payload_bytes_received"].asInt64();
  }
  if (payload.isMember("rtp_last_payload_type") &&
      (payload["rtp_last_payload_type"].isInt64() || payload["rtp_last_payload_type"].isUInt64())) {
    runtime->rtp_last_payload_type = payload["rtp_last_payload_type"].asInt64();
  }
  if (payload.isMember("rtp_last_sequence") &&
      (payload["rtp_last_sequence"].isInt64() || payload["rtp_last_sequence"].isUInt64())) {
    runtime->rtp_last_sequence = payload["rtp_last_sequence"].asInt64();
  }
  if (payload.isMember("rtp_last_timestamp") &&
      (payload["rtp_last_timestamp"].isInt64() || payload["rtp_last_timestamp"].isUInt64())) {
    runtime->rtp_last_timestamp = payload["rtp_last_timestamp"].asInt64();
  }
  if (payload.isMember("rtp_last_ssrc") &&
      (payload["rtp_last_ssrc"].isInt64() || payload["rtp_last_ssrc"].isUInt64())) {
    runtime->rtp_last_ssrc = payload["rtp_last_ssrc"].asInt64();
  }
  if (payload.isMember("native_media_provider") && payload["native_media_provider"].isObject()) {
    runtime->native_media_provider = payload["native_media_provider"];
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
