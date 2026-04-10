#include "session_voice_builtin_media_engine_plugin.h"

#include <juice/juice.h>
#include <srtp2/srtp.h>
#include <usrsctp.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr const char* kProviderName = "agentd_builtin_embedded_transport_provider";
constexpr const char* kProviderVersion = "0.2.0";
constexpr const char* kCapabilitiesJson =
  "{\"signaling\":true,\"audio_capture\":false,\"audio_render\":false,"
  "\"ice\":true,\"dtls\":false,\"srtp\":true,\"sctp\":true,"
  "\"transport_family\":\"embedded_transport_primitives\","
  "\"embedded_transport_provider\":true,\"sample_provider\":false,"
  "\"real_media_engine\":false,\"remote_description_optional\":true,"
  "\"candidate_trickle_ingest\":true,\"candidate_trickle_emit\":false}";

struct EmbeddedTransportState {
  juice_agent_t* agent = nullptr;
  std::string local_description;
  std::string libjuice_state = "disconnected";
  std::string last_remote_description_error;
  std::string selected_local_candidate;
  std::string selected_remote_candidate;
  std::string selected_local_address;
  std::string selected_remote_address;
  uint64_t offers_seen = 0;
  uint64_t remote_candidates_seen = 0;
  uint64_t local_candidates_observed = 0;
  bool gathering_done = false;
  bool gather_started = false;
  bool remote_description_applied = false;
  bool transport_connectivity_ready = false;
};

std::mutex g_transport_runtime_mu;
size_t g_transport_runtime_refs = 0;
bool g_srtp_initialized = false;
bool g_usrsctp_initialized = false;

bool copy_text(const std::string& value, char* out, size_t out_size) {
  if (!out || out_size == 0) return false;
  if (value.size() + 1 > out_size) return false;
  std::memset(out, 0, out_size);
  std::memcpy(out, value.data(), value.size());
  out[value.size()] = '\0';
  return true;
}

void write_error(const std::string& value, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  (void)copy_text(value.empty() ? std::string("unknown") : value, out, out_size);
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string juice_state_text(juice_state_t state) {
  const char* raw = juice_state_to_string(state);
  return raw ? std::string(raw) : std::string("unknown");
}

bool refresh_local_description(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine || !engine->agent) {
    if (out_err) *out_err = "missing libjuice agent";
    return false;
  }
  char description[JUICE_MAX_SDP_STRING_LEN];
  std::memset(description, 0, sizeof(description));
  if (juice_get_local_description(engine->agent, description, sizeof(description)) != JUICE_ERR_SUCCESS) {
    if (out_err) *out_err = "libjuice local description generation failed";
    return false;
  }
  engine->local_description = description;
  return true;
}

void refresh_transport_snapshot(EmbeddedTransportState* engine) {
  if (!engine || !engine->agent) return;

  const juice_state_t state = juice_get_state(engine->agent);
  engine->libjuice_state = juice_state_text(state);

  char local_candidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  char remote_candidate[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
  std::memset(local_candidate, 0, sizeof(local_candidate));
  std::memset(remote_candidate, 0, sizeof(remote_candidate));
  if (juice_get_selected_candidates(
        engine->agent,
        local_candidate,
        sizeof(local_candidate),
        remote_candidate,
        sizeof(remote_candidate)) == JUICE_ERR_SUCCESS) {
    engine->selected_local_candidate = local_candidate;
    engine->selected_remote_candidate = remote_candidate;
    engine->transport_connectivity_ready = true;
  }

  char local_address[JUICE_MAX_ADDRESS_STRING_LEN];
  char remote_address[JUICE_MAX_ADDRESS_STRING_LEN];
  std::memset(local_address, 0, sizeof(local_address));
  std::memset(remote_address, 0, sizeof(remote_address));
  if (juice_get_selected_addresses(
        engine->agent,
        local_address,
        sizeof(local_address),
        remote_address,
        sizeof(remote_address)) == JUICE_ERR_SUCCESS) {
    engine->selected_local_address = local_address;
    engine->selected_remote_address = remote_address;
    engine->transport_connectivity_ready = true;
  }

  if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
    engine->transport_connectivity_ready = true;
  }
}

void wait_for_local_gathering(EmbeddedTransportState* engine, int timeout_ms) {
  if (!engine || !engine->agent || timeout_ms <= 0) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    refresh_transport_snapshot(engine);
    if (engine->gathering_done || engine->local_candidates_observed > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void wait_for_transport_progress(EmbeddedTransportState* engine, int timeout_ms) {
  if (!engine || !engine->agent || timeout_ms <= 0) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    refresh_transport_snapshot(engine);
    if (engine->transport_connectivity_ready) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

bool reserve_transport_runtime(std::string* out_err) {
  if (out_err) out_err->clear();
  std::lock_guard<std::mutex> lk(g_transport_runtime_mu);
  if (g_transport_runtime_refs == 0) {
    const srtp_err_status_t srtp_status = srtp_init();
    if (srtp_status != srtp_err_status_ok) {
      if (out_err) {
        *out_err = "libsrtp initialization failed with status " +
                   std::to_string(static_cast<int>(srtp_status));
      }
      return false;
    }
    g_srtp_initialized = true;
    usrsctp_init(0, nullptr, nullptr);
    g_usrsctp_initialized = true;
  }
  g_transport_runtime_refs += 1;
  return true;
}

void release_transport_runtime() {
  std::lock_guard<std::mutex> lk(g_transport_runtime_mu);
  if (g_transport_runtime_refs == 0) return;
  g_transport_runtime_refs -= 1;
  if (g_transport_runtime_refs != 0) return;
  if (g_usrsctp_initialized) {
    (void)usrsctp_finish();
    g_usrsctp_initialized = false;
  }
  if (g_srtp_initialized) {
    (void)srtp_shutdown();
    g_srtp_initialized = false;
  }
}

void on_state_changed(juice_agent_t*, juice_state_t state, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine) return;
  engine->libjuice_state = juice_state_text(state);
}

void on_candidate(juice_agent_t*, const char*, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine) return;
  engine->local_candidates_observed += 1;
}

void on_gathering_done(juice_agent_t*, void* user_ptr) {
  auto* engine = static_cast<EmbeddedTransportState*>(user_ptr);
  if (!engine) return;
  engine->gathering_done = true;
}

bool ensure_agent(EmbeddedTransportState* engine, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!engine) {
    if (out_err) *out_err = "missing instance";
    return false;
  }
  if (engine->agent) return true;

  juice_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  cfg.cb_state_changed = &on_state_changed;
  cfg.cb_candidate = &on_candidate;
  cfg.cb_gathering_done = &on_gathering_done;
  cfg.user_ptr = engine;

  engine->agent = juice_create(&cfg);
  if (!engine->agent) {
    if (out_err) *out_err = "libjuice agent creation failed";
    return false;
  }
  engine->libjuice_state = juice_state_text(juice_get_state(engine->agent));

  if (juice_gather_candidates(engine->agent) != JUICE_ERR_SUCCESS) {
    juice_destroy(engine->agent);
    engine->agent = nullptr;
    if (out_err) *out_err = "libjuice candidate gathering failed";
    return false;
  }
  engine->gather_started = true;
  wait_for_local_gathering(engine, 500);
  if (!refresh_local_description(engine, out_err)) {
    juice_destroy(engine->agent);
    engine->agent = nullptr;
    return false;
  }
  refresh_transport_snapshot(engine);
  return true;
}

std::string build_event_json(
  const EmbeddedTransportState& state,
  const std::string& event_name,
  const std::string& media_engine_state,
  uint64_t initial_remote_candidate_count
) {
  const char* srtp_version = srtp_get_version_string();
  std::string json =
    std::string("{\"ok\":true,\"event\":\"") + json_escape(event_name) +
    "\",\"media_engine_state\":\"" + json_escape(media_engine_state) +
    "\",\"media_engine_kind\":\"builtin_native_plugin\""
    ",\"native_media_supported\":false,\"native_media_active\":false"
    ",\"provider\":\"" + std::string(kProviderName) + "\""
    ",\"transport_family\":\"embedded_transport_primitives\""
    ",\"libjuice_state\":\"" + json_escape(state.libjuice_state) + "\""
    ",\"libjuice_local_description_bytes\":" + std::to_string(state.local_description.size()) +
    ",\"local_candidates_observed\":" + std::to_string(state.local_candidates_observed) +
    ",\"remote_candidates_seen\":" + std::to_string(state.remote_candidates_seen) +
    ",\"offers_seen\":" + std::to_string(state.offers_seen) +
    ",\"initial_remote_candidate_count\":" + std::to_string(initial_remote_candidate_count) +
    ",\"gather_started\":" + std::string(state.gather_started ? "true" : "false") +
    ",\"remote_description_applied\":" +
    std::string(state.remote_description_applied ? "true" : "false") +
    ",\"gathering_done\":" + std::string(state.gathering_done ? "true" : "false") +
    ",\"transport_connectivity_ready\":" +
    std::string(state.transport_connectivity_ready ? "true" : "false") +
    ",\"srtp_version\":\"" + json_escape(srtp_version ? std::string(srtp_version) : std::string("unknown")) + "\""
    ",\"usrsctp_initialized\":true";
  if (!state.selected_local_candidate.empty()) {
    json += ",\"libjuice_selected_local_candidate\":\"" +
            json_escape(state.selected_local_candidate) + "\"";
  }
  if (!state.selected_remote_candidate.empty()) {
    json += ",\"libjuice_selected_remote_candidate\":\"" +
            json_escape(state.selected_remote_candidate) + "\"";
  }
  if (!state.selected_local_address.empty()) {
    json += ",\"libjuice_selected_local_address\":\"" +
            json_escape(state.selected_local_address) + "\"";
  }
  if (!state.selected_remote_address.empty()) {
    json += ",\"libjuice_selected_remote_address\":\"" +
            json_escape(state.selected_remote_address) + "\"";
  }
  if (!state.last_remote_description_error.empty()) {
    json += ",\"remote_description_error\":\"" + json_escape(state.last_remote_description_error) + "\"";
  }
  json += "}";
  return json;
}

int embedded_create(void** out_instance, char* err_buf, size_t err_buf_size) {
  if (!out_instance) {
    write_error("missing out_instance", err_buf, err_buf_size);
    return 0;
  }
  std::string err;
  if (!reserve_transport_runtime(&err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  *out_instance = new EmbeddedTransportState();
  return 1;
}

void embedded_destroy(void* instance) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (engine) {
    if (engine->agent) {
      juice_destroy(engine->agent);
      engine->agent = nullptr;
    }
    delete engine;
  }
  release_transport_runtime();
}

int embedded_initialize(
  void* instance,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  std::string err;
  if (!ensure_agent(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  refresh_transport_snapshot(engine);
  const std::string payload = build_event_json(*engine, "media_engine_initialized", "signaling_ready", 0);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

int embedded_handle_remote_description(
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
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  std::string err;
  if (!ensure_agent(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  const std::string type = description_type ? std::string(description_type) : std::string();
  if (!type.empty() && type != "offer") {
    write_error("expected remote offer", err_buf, err_buf_size);
    return 0;
  }
  engine->offers_seen += 1;
  engine->remote_description_applied = false;
  engine->last_remote_description_error.clear();

  const std::string remote_sdp = description_sdp ? std::string(description_sdp) : std::string();
  if (!remote_sdp.empty()) {
    const int rc = juice_set_remote_description(engine->agent, remote_sdp.c_str());
    if (rc == JUICE_ERR_SUCCESS) {
      engine->remote_description_applied = true;
      refresh_transport_snapshot(engine);
    } else {
      engine->last_remote_description_error =
        "libjuice remote description rejected with code " + std::to_string(rc);
    }
  } else {
    engine->last_remote_description_error = "remote SDP was empty";
  }
  if (initial_remote_candidate_count == 0) {
    (void)juice_set_remote_gathering_done(engine->agent);
  }
  wait_for_transport_progress(engine, 250);
  if (!refresh_local_description(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  refresh_transport_snapshot(engine);

  if (!copy_text("answer", answer_type_buf, answer_type_buf_size) ||
      !copy_text(engine->local_description, answer_sdp_buf, answer_sdp_buf_size)) {
    write_error("answer buffer too small", err_buf, err_buf_size);
    return 0;
  }

  const std::string payload = build_event_json(*engine, "embedded_transport_answer_ready", "answer_ready",
                                               initial_remote_candidate_count);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

int embedded_handle_remote_candidate(
  void* instance,
  const char* candidate,
  const char*,
  int,
  int,
  char* event_json_buf,
  size_t event_json_buf_size,
  char* err_buf,
  size_t err_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  std::string err;
  if (!ensure_agent(engine, &err)) {
    write_error(err, err_buf, err_buf_size);
    return 0;
  }
  const std::string candidate_sdp = candidate ? std::string(candidate) : std::string();
  if (candidate_sdp.empty()) {
    write_error("remote candidate missing", err_buf, err_buf_size);
    return 0;
  }
  const int rc = juice_add_remote_candidate(engine->agent, candidate_sdp.c_str());
  if (rc != JUICE_ERR_SUCCESS) {
    write_error("libjuice remote candidate rejected with code " + std::to_string(rc), err_buf, err_buf_size);
    return 0;
  }
  engine->remote_candidates_seen += 1;
  wait_for_transport_progress(engine, 1500);
  refresh_transport_snapshot(engine);

  const std::string payload =
    build_event_json(*engine, "remote_candidate_ready", "signaling_active", 0);
  if (!copy_text(payload, event_json_buf, event_json_buf_size)) {
    write_error("event buffer too small", err_buf, err_buf_size);
    return 0;
  }
  return 1;
}

void embedded_handle_remote_bye(
  void* instance,
  const char* reason,
  char* event_json_buf,
  size_t event_json_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) return;
  refresh_transport_snapshot(engine);
  std::string payload = build_event_json(*engine, "remote_bye", "stopped", 0);
  payload.pop_back();
  if (reason && reason[0]) {
    payload += ",\"reason\":\"" + json_escape(reason) + "\"";
  }
  payload += "}";
  (void)copy_text(payload, event_json_buf, event_json_buf_size);
}

void embedded_handle_local_shutdown(
  void* instance,
  char* event_json_buf,
  size_t event_json_buf_size
) {
  auto* engine = static_cast<EmbeddedTransportState*>(instance);
  if (!engine) return;
  refresh_transport_snapshot(engine);
  std::string payload = build_event_json(*engine, "local_bye_sent", "stopping", 0);
  payload.pop_back();
  payload += ",\"reason\":\"agentd_builtin_stop\"}";
  (void)copy_text(payload, event_json_buf, event_json_buf_size);
}

const agentd_voice_media_engine_provider_v2 kEmbeddedProvider = {
  AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V2,
  "builtin_native_plugin",
  0,
  kProviderName,
  kProviderVersion,
  kCapabilitiesJson,
  &embedded_create,
  &embedded_destroy,
  &embedded_initialize,
  &embedded_handle_remote_description,
  &embedded_handle_remote_candidate,
  &embedded_handle_remote_bye,
  &embedded_handle_local_shutdown,
};

}  // namespace

extern "C" const agentd_voice_media_engine_provider_v2* agentd_voice_media_engine_get_api_v2() {
  return &kEmbeddedProvider;
}
