#include "client_prefs_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <chrono>

namespace agentd {
namespace {

constexpr size_t kMaxClientIdLen = 128;
constexpr size_t kMaxClientKindLen = 64;
constexpr size_t kMaxPrefsBodyBytes = 64 * 1024;

int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count();
}

bool is_safe_token(const std::string& s_in, size_t max_len) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > max_len) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

std::string client_prefs_key(const std::string& client_kind, const std::string& client_id) {
  return "client.prefs." + client_kind + "." + client_id;
}

void write_json(HttpResponse* resp, const Json::Value& v) {
  if (!resp) return;
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->body = json_stringify(v);
}

bool parse_client_tokens_from_query(
  const HttpRequest& req,
  std::string* out_kind,
  std::string* out_id,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_kind || !out_id) return false;
  const auto cid = query_get(req.query, "client_id");
  if (!cid || cid->empty()) {
    if (out_err) *out_err = "missing client_id";
    return false;
  }
  const std::string client_id = trim_copy(*cid);
  const std::string client_kind = trim_copy(query_get(req.query, "client_kind").value_or("webui"));
  if (!is_safe_token(client_id, kMaxClientIdLen)) {
    if (out_err) *out_err = "invalid client_id";
    return false;
  }
  if (!is_safe_token(client_kind, kMaxClientKindLen)) {
    if (out_err) *out_err = "invalid client_kind";
    return false;
  }
  *out_kind = client_kind;
  *out_id = client_id;
  return true;
}

}  // namespace

void handle_client_prefs_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!resp) return;
  if (!daemon_require_auth(cfg, req, resp)) return;
  resp->headers["Content-Type"] = "application/json; charset=utf-8";

  std::string client_kind;
  std::string client_id;
  std::string err;
  if (!parse_client_tokens_from_query(req, &client_kind, &client_id, &err)) {
    resp->status = 400;
    resp->body = json_error_body(err.empty() ? "invalid client tokens" : err);
    return;
  }
  if (!db) {
    resp->status = 500;
    resp->body = json_error_body("db unavailable");
    return;
  }

  std::string raw;
  if (!db->meta_get(client_prefs_key(client_kind, client_id), &raw, &err)) {
    resp->status = 500;
    resp->body = json_error_body(err.empty() ? "db meta_get failed" : err);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["client_kind"] = client_kind;
  out["client_id"] = client_id;
  if (raw.empty()) {
    out["found"] = false;
    write_json(resp, out);
    return;
  }

  Json::Value stored;
  if (!json_parse_object(raw, &stored, &err)) {
    resp->status = 500;
    resp->body = json_error_body("stored prefs corrupt", "prefs_corrupt");
    return;
  }

  out["found"] = true;
  if (stored.isMember("version")) out["version"] = stored["version"];
  if (stored.isMember("prefs")) out["prefs"] = stored["prefs"];
  if (stored.isMember("updated_utc_ms")) out["updated_utc_ms"] = stored["updated_utc_ms"];
  write_json(resp, out);
}

void handle_client_prefs_post_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!resp) return;
  if (!daemon_require_auth(cfg, req, resp)) return;
  resp->headers["Content-Type"] = "application/json; charset=utf-8";

  if (req.body.size() > kMaxPrefsBodyBytes) {
    resp->status = 413;
    resp->body = json_error_body("prefs body too large", "prefs_too_large");
    return;
  }

  Json::Value root;
  std::string err;
  if (!json_parse_object(req.body, &root, &err)) {
    resp->status = 400;
    resp->body = json_error_body(err.empty() ? "invalid JSON body" : err);
    return;
  }

  const std::string client_id = trim_copy(root.get("client_id", "").asString());
  const std::string client_kind = trim_copy(root.get("client_kind", "webui").asString());
  if (!is_safe_token(client_id, kMaxClientIdLen)) {
    resp->status = 400;
    resp->body = json_error_body("invalid client_id");
    return;
  }
  if (!is_safe_token(client_kind, kMaxClientKindLen)) {
    resp->status = 400;
    resp->body = json_error_body("invalid client_kind");
    return;
  }

  const Json::Value prefs = root["prefs"];
  if (!prefs.isObject()) {
    resp->status = 400;
    resp->body = json_error_body("prefs must be an object");
    return;
  }
  if (!db) {
    resp->status = 500;
    resp->body = json_error_body("db unavailable");
    return;
  }

  Json::Value stored(Json::objectValue);
  stored["version"] = 1;
  stored["client_id"] = client_id;
  stored["client_kind"] = client_kind;
  stored["prefs"] = prefs;
  stored["updated_utc_ms"] = Json::Int64(now_unix_ms());

  if (!db->meta_set(client_prefs_key(client_kind, client_id), json_stringify(stored), &err)) {
    resp->status = 500;
    resp->body = json_error_body(err.empty() ? "db meta_set failed" : err);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["found"] = true;
  out["version"] = stored["version"];
  out["client_id"] = client_id;
  out["client_kind"] = client_kind;
  out["prefs"] = prefs;
  out["updated_utc_ms"] = stored["updated_utc_ms"];
  write_json(resp, out);
}

}  // namespace agentd
