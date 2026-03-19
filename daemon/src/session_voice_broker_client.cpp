#include "session_voice_broker_client.h"

#include "http_client.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

namespace agentd {
namespace {

static bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

static bool is_safe_shellish_token(const std::string& s_in, size_t max_len) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > max_len) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string join_base_path(const std::string& base_in, const std::string& suffix) {
  std::string base = trim_copy(base_in);
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (suffix.empty()) return base;
  if (suffix.front() == '/') return base + suffix;
  return base + "/" + suffix;
}

}  // namespace

std::string effective_voice_broker_url(const DaemonConfig& cfg, const std::string& request_broker_url) {
  const std::string requested = trim_copy(request_broker_url);
  if (!requested.empty()) return requested;
  return trim_copy(cfg.audio_webrtc_broker_url);
}

std::string effective_voice_broker_token(const DaemonConfig& cfg, const std::string& request_broker_token) {
  const std::string requested = trim_copy(request_broker_token);
  if (!requested.empty()) return requested;
  return trim_copy(cfg.audio_webrtc_broker_token);
}

bool validate_voice_broker_token_if_present(const std::string& broker_token, std::string* out_err) {
  if (out_err) out_err->clear();
  if (broker_token.empty()) return true;
  if (!is_safe_printable_field(broker_token, 1024)) {
    if (out_err) *out_err = "invalid configured audio_webrtc_broker_token";
    return false;
  }
  return true;
}

bool broker_create_audio_session(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_agent_id,
  const std::string& broker_deployment_id,
  std::string* out_session_id,
  std::string* out_err
) {
  if (out_session_id) out_session_id->clear();
  if (out_err) out_err->clear();

  Json::Value body(Json::objectValue);
  body["agent_id"] = broker_agent_id;
  body["mode"] = "webrtc";
  if (!broker_deployment_id.empty()) body["deployment_id"] = broker_deployment_id;

  const HttpClientResult result = http_request(
    join_base_path(broker_url, "/v1/audio/sessions"),
    "POST",
    {
      {"Authorization", std::string("Bearer ") + broker_token},
      {"Content-Type", "application/json"},
    },
    json_stringify(body),
    /*timeout_ms=*/10000,
    /*max_response_bytes=*/256 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve_or_null=*/nullptr
  );
  if (!result.ok) {
    if (out_err) *out_err = result.error.empty() ? "broker create request failed" : result.error;
    return false;
  }

  Json::Value parsed(Json::nullValue);
  std::string jerr;
  const bool have_json = json_parse_any(result.response_body, &parsed, &jerr) && parsed.isObject();
  if (result.http_status != 200) {
    std::string err = "broker create audio session failed";
    if (have_json && parsed.isMember("error") && parsed["error"].isString()) err += ": " + parsed["error"].asString();
    else err += ": http " + std::to_string(result.http_status);
    if (out_err) *out_err = err;
    return false;
  }
  if (!have_json) {
    if (out_err) *out_err = jerr.empty() ? "broker create response invalid" : jerr;
    return false;
  }
  if (!parsed.isMember("ok") || !parsed["ok"].asBool()) {
    if (out_err) *out_err = "broker create audio session returned ok=false";
    return false;
  }
  if (!parsed.isMember("session_id") || !parsed["session_id"].isString()) {
    if (out_err) *out_err = "broker create response missing session_id";
    return false;
  }
  const std::string session_id = trim_copy(parsed["session_id"].asString());
  if (!is_safe_shellish_token(session_id, 160)) {
    if (out_err) *out_err = "broker create returned invalid session_id";
    return false;
  }
  if (out_session_id) *out_session_id = session_id;
  return true;
}

bool broker_delete_audio_session(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_session_id,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (trim_copy(broker_session_id).empty()) return true;
  const HttpClientResult result = http_request(
    join_base_path(broker_url, "/v1/audio/sessions/" + broker_session_id),
    "DELETE",
    {
      {"Authorization", std::string("Bearer ") + broker_token},
    },
    "",
    /*timeout_ms=*/10000,
    /*max_response_bytes=*/128 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve_or_null=*/nullptr
  );
  if (!result.ok) {
    if (out_err) *out_err = result.error.empty() ? "broker delete request failed" : result.error;
    return false;
  }
  if (result.http_status == 404) return true;
  if (result.http_status != 200) {
    std::string err = "broker delete audio session failed: http " + std::to_string(result.http_status);
    Json::Value parsed(Json::nullValue);
    std::string jerr;
    if (json_parse_any(result.response_body, &parsed, &jerr) && parsed.isObject() &&
        parsed.isMember("error") && parsed["error"].isString()) {
      err += " " + parsed["error"].asString();
    }
    if (out_err) *out_err = err;
    return false;
  }
  return true;
}

bool broker_audio_session_exists(
  const std::string& broker_url,
  const std::string& broker_token,
  const std::string& broker_session_id,
  bool* out_exists,
  std::string* out_mode,
  std::string* out_err
) {
  if (out_exists) *out_exists = false;
  if (out_mode) out_mode->clear();
  if (out_err) out_err->clear();
  const std::string session_id = trim_copy(broker_session_id);
  if (session_id.empty()) {
    if (out_err) *out_err = "broker_session_id required";
    return false;
  }
  const HttpClientResult result = http_request(
    join_base_path(broker_url, "/v1/audio/sessions/" + session_id),
    "GET",
    {
      {"Authorization", std::string("Bearer ") + broker_token},
    },
    "",
    /*timeout_ms=*/10000,
    /*max_response_bytes=*/128 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve_or_null=*/nullptr
  );
  if (!result.ok) {
    if (out_err) *out_err = result.error.empty() ? "broker inspect request failed" : result.error;
    return false;
  }
  if (result.http_status == 404) {
    if (out_exists) *out_exists = false;
    return true;
  }
  if (result.http_status != 200) {
    std::string err = "broker inspect audio session failed: http " + std::to_string(result.http_status);
    Json::Value parsed(Json::nullValue);
    std::string jerr;
    if (json_parse_any(result.response_body, &parsed, &jerr) && parsed.isObject() &&
        parsed.isMember("error") && parsed["error"].isString()) {
      err += " " + parsed["error"].asString();
    }
    if (out_err) *out_err = err;
    return false;
  }
  Json::Value parsed(Json::nullValue);
  std::string jerr;
  if (!json_parse_any(result.response_body, &parsed, &jerr) || !parsed.isObject()) {
    if (out_err) *out_err = jerr.empty() ? "broker inspect response invalid" : jerr;
    return false;
  }
  if (!parsed.isMember("ok") || !parsed["ok"].asBool()) {
    if (out_err) *out_err = "broker inspect audio session returned ok=false";
    return false;
  }
  if (!parsed.isMember("session") || !parsed["session"].isObject()) {
    if (out_err) *out_err = "broker inspect response missing session";
    return false;
  }
  const Json::Value& session = parsed["session"];
  if (out_mode && session.isMember("mode") && session["mode"].isString()) {
    *out_mode = lower_copy(trim_copy(session["mode"].asString()));
  }
  if (out_exists) *out_exists = true;
  return true;
}

}  // namespace agentd
