#include "session_voice_builtin_media_engine_plugin.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct MockVoiceMediaEngineState {
  uint64_t offers_seen = 0;
  uint64_t candidates_seen = 0;
};

bool copy_text(const std::string& value, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  if (value.size() + 1 > out_size) return false;
  std::memset(out, 0, out_size);
  std::memcpy(out, value.data(), value.size());
  out[value.size()] = '\0';
  return true;
}

void write_error(const char* message, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  const std::string text = message ? std::string(message) : std::string("unknown");
  copy_text(text, out, out_size);
}

int mock_create(void** out_instance, char* err_buf, size_t err_buf_size) {
  if (!out_instance) {
    write_error("missing out_instance", err_buf, err_buf_size);
    return 0;
  }
  *out_instance = new MockVoiceMediaEngineState();
  return 1;
}

void mock_destroy(void* instance) {
  delete static_cast<MockVoiceMediaEngineState*>(instance);
}

int mock_initialize(
  void* instance,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  if (!instance) {
    write_error("missing instance", err_buf, err_buf_size);
    return 0;
  }
  return copy_text(
           "{\"ok\":true,\"event\":\"media_engine_initialized\",\"media_engine_state\":\"signaling_ready\","
           "\"media_engine_kind\":\"builtin_native_plugin\",\"native_media_supported\":true,"
           "\"native_media_active\":true,\"provider\":\"mock_native_plugin\"}",
           event_json_buf,
           event_json_buf_size)
    ? 1
    : (write_error("event buffer too small", err_buf, err_buf_size), 0);
}

int mock_handle_remote_description(
  void* instance,
  const char* description_type,
  const char*,
  uint64_t initial_remote_candidate_count,
  char* answer_type_buf,
  size_t answer_type_buf_size,
  char* answer_sdp_buf,
  size_t answer_sdp_buf_size,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* state = static_cast<MockVoiceMediaEngineState*>(instance);
  if (!state) {
    write_error("missing instance", err_buf, err_buf_size);
    return 0;
  }
  const std::string type = description_type ? std::string(description_type) : std::string();
  if (!type.empty() && type != "offer") {
    write_error("expected remote offer", err_buf, err_buf_size);
    return 0;
  }
  state->offers_seen += 1;
  if (!copy_text("answer", answer_type_buf, answer_type_buf_size) ||
      !copy_text("native-plugin-answer", answer_sdp_buf, answer_sdp_buf_size)) {
    write_error("answer buffer too small", err_buf, err_buf_size);
    return 0;
  }
  char event_json[512];
  std::snprintf(
    event_json,
    sizeof(event_json),
    "{\"ok\":true,\"event\":\"native_answer_ready\",\"media_engine_state\":\"answer_ready\","
    "\"media_engine_kind\":\"builtin_native_plugin\",\"native_media_supported\":true,"
    "\"native_media_active\":true,\"initial_remote_candidate_count\":%llu}",
    static_cast<unsigned long long>(initial_remote_candidate_count));
  return copy_text(event_json, event_json_buf, event_json_buf_size)
    ? 1
    : (write_error("event buffer too small", err_buf, err_buf_size), 0);
}

int mock_handle_remote_candidate(
  void* instance,
  const char*,
  const char*,
  int,
  int,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* state = static_cast<MockVoiceMediaEngineState*>(instance);
  if (!state) {
    write_error("missing instance", err_buf, err_buf_size);
    return 0;
  }
  state->candidates_seen += 1;
  char event_json[512];
  std::snprintf(
    event_json,
    sizeof(event_json),
    "{\"ok\":true,\"event\":\"remote_candidate_ready\",\"media_engine_state\":\"signaling_active\","
    "\"media_engine_kind\":\"builtin_native_plugin\",\"native_media_supported\":true,"
    "\"native_media_active\":true,\"candidate_count\":%llu}",
    static_cast<unsigned long long>(state->candidates_seen));
  return copy_text(event_json, event_json_buf, event_json_buf_size)
    ? 1
    : (write_error("event buffer too small", err_buf, err_buf_size), 0);
}

void mock_handle_remote_bye(
  void*,
  const char* reason,
  char* event_json_buf,
  size_t event_json_buf_size
) {
  const std::string why = reason ? std::string(reason) : std::string();
  std::string payload =
    std::string("{\"ok\":true,\"event\":\"remote_bye\",\"media_engine_state\":\"stopped\","
                "\"media_engine_kind\":\"builtin_native_plugin\",\"native_media_supported\":true,"
                "\"native_media_active\":false");
  if (!why.empty()) payload += ",\"reason\":\"" + why + "\"";
  payload += "}";
  copy_text(payload, event_json_buf, event_json_buf_size);
}

void mock_handle_local_shutdown(
  void*,
  char* event_json_buf,
  size_t event_json_buf_size
) {
  copy_text(
    "{\"ok\":true,\"event\":\"local_bye_sent\",\"media_engine_state\":\"stopping\","
    "\"media_engine_kind\":\"builtin_native_plugin\",\"native_media_supported\":true,"
    "\"native_media_active\":false,\"reason\":\"agentd_builtin_stop\"}",
    event_json_buf,
    event_json_buf_size);
}

const agentd_voice_media_engine_provider_v1 kMockProvider = {
  AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V1,
  "builtin_native_plugin",
  1,
  &mock_create,
  &mock_destroy,
  &mock_initialize,
  &mock_handle_remote_description,
  &mock_handle_remote_candidate,
  &mock_handle_remote_bye,
  &mock_handle_local_shutdown,
};

}  // namespace

extern "C" const agentd_voice_media_engine_provider_v1* agentd_voice_media_engine_get_api_v1() {
  return &kMockProvider;
}
