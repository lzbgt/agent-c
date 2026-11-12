#include "workflow_sop_registry.h"
#include "workflow_endpoints.h"
#include "workflow_sop.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>

namespace agentd {
namespace {
std::string stringify(const Json::Value& v) {
  Json::StreamWriterBuilder w; w["indentation"] = ""; return Json::writeString(w, v);
}
bool safe_id(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (const unsigned char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':')) return false;
  return true;
}
bool has_credentials(const Json::Value& v) {
  static const std::unordered_set<std::string> keys = {"api_key", "auth_token", "authorization", "proxy-authorization",
    "password", "client_secret", "access_token", "refresh_token", "private_key", "secret_access_key"};
  if (v.isObject()) {
    for (const auto& key : v.getMemberNames()) {
      std::string lower = key;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
      if (keys.count(lower) || has_credentials(v[key])) return true;
    }
  } else if (v.isArray()) {
    for (const auto& item : v) if (has_credentials(item)) return true;
  }
  return false;
}
bool parse_positive(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  int64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9' || v > (std::numeric_limits<int64_t>::max() - (c - '0')) / 10) return false;
    v = v * 10 + (c - '0');
  }
  if (v <= 0) return false;
  *out = v; return true;
}
Json::Value metadata(const AgentDb::WorkflowSopRow& row) {
  Json::Value v(Json::objectValue);
  v["registry_id"] = (Json::Int64)row.registry_id;
  v["id"] = row.definition_id; v["version"] = row.version;
  v["definition_hash"] = row.definition_hash; v["title"] = row.title;
  v["created_unix_ms"] = (Json::Int64)row.created_unix_ms;
  return v;
}
bool verified_definition(const AgentDb::WorkflowSopRow& row, Json::Value* sop) {
  std::string error;
  if (!json_parse_object(row.sop_json, sop, &error)) return false;
  Json::Value args(Json::objectValue); args["sop"] = *sop;
  return workflow_compile_sop(&args, &error) && (*sop)["id"] == row.definition_id &&
    (*sop)["version"] == row.version && args["sop_compiled"]["definition_hash"] == row.definition_hash &&
    !has_credentials(*sop);
}
}

int workflow_resolve_sop_reference(AgentDb* db, Json::Value* args, std::string* error) {
  if (error) error->clear();
  const auto fail = [&](int status, const std::string& message) { if (error) *error = message; return status; };
  if (!args || !args->isObject()) return fail(400, "expected workflow object");
  if (!args->isMember("sop_ref")) return 0;
  if (args->isMember("tasks") || args->isMember("sop") || args->isMember("sop_compiled")) return fail(400, "supply exactly one of tasks, sop or sop_ref");
  const auto& ref = (*args)["sop_ref"];
  if (!ref.isObject() || !ref["id"].isString() || !ref["version"].isString() ||
      !safe_id(ref["id"].asString()) || !safe_id(ref["version"].asString())) return fail(400, "sop_ref requires id and version");
  for (const auto& key : ref.getMemberNames())
    if (key != "id" && key != "version" && key != "definition_hash") return fail(400, "unknown sop_ref field");
  if (ref.isMember("definition_hash") && (!ref["definition_hash"].isString() || ref["definition_hash"].asString().empty() || ref["definition_hash"].asString().size() > 96))
    return fail(400, "invalid sop_ref definition_hash");
  if (!db || !db->is_open()) return fail(503, "db not available");
  AgentDb::WorkflowSopRow row; std::string db_error;
  if (!db->get_workflow_sop(ref["id"].asString(), ref["version"].asString(), &row, &db_error))
    return fail(db_error.empty() ? 404 : 500, db_error.empty() ? "SOP version not found" : "failed to load SOP version");
  if (ref.isMember("definition_hash") && ref["definition_hash"] != row.definition_hash) return fail(409, "SOP definition hash mismatch");
  Json::Value sop;
  if (!verified_definition(row, &sop)) return fail(500, "stored SOP integrity check failed");
  (*args)["sop"] = std::move(sop);
  (*args)["sop_ref"]["definition_hash"] = row.definition_hash;
  return 0;
}

void handle_workflow_sops_endpoint(const DaemonConfig& cfg, const CorsConfig& cors_cfg,
    AgentDb* db, const HttpRequest& req, HttpResponse* resp) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  const auto fail = [&](int status, const std::string& message) { resp->status = status; resp->body = json_error_body(message); };
  if (!db || !db->is_open()) { fail(503, "db not available"); return; }
  Json::Value result(Json::objectValue); result["ok"] = true;
  std::string error;
  if (req.method == "POST") {
    if (req.body.size() > 128 * 1024) { fail(400, "SOP save request exceeds 128 KiB"); return; }
    Json::Value payload;
    if (!json_parse_object(req.body, &payload, &error) || payload.size() != 1 || !payload.isMember("sop")) {
      fail(400, "expected an object containing only sop"); return;
    }
    if (has_credentials(payload["sop"])) { fail(400, "saved SOPs cannot contain inline credential fields; use server-side references"); return; }
    Json::Value compiled = payload;
    if (!workflow_compile_sop(&compiled, &error)) { fail(400, error); return; }
    const auto& sop = payload["sop"];
    AgentDb::WorkflowSopRow def, saved; bool created = false;
    def.definition_id = sop["id"].asString(); def.version = sop["version"].asString();
    def.title = sop.get("title", "").asString(); def.sop_json = stringify(sop);
    def.definition_hash = compiled["sop_compiled"]["definition_hash"].asString();
    if (!db->create_workflow_sop(def, &saved, &created, &error)) { fail(500, "failed to save SOP version"); return; }
    if (saved.definition_hash != def.definition_hash) { fail(409, "SOP version already exists with different content; save a new version"); return; }
    Json::Value stored;
    if (!verified_definition(saved, &stored)) { fail(500, "stored SOP integrity check failed"); return; }
    result["created"] = created; result["definition"] = metadata(saved); result["definition"]["sop"] = std::move(stored);
    resp->status = created ? 201 : 200;
  } else {
    const auto id = query_get(req.query, "id"), version = query_get(req.query, "version");
    if (id || version) {
      if (!id || !version || !safe_id(*id) || !safe_id(*version)) { fail(400, "supply both id and version"); return; }
      AgentDb::WorkflowSopRow row;
      if (!db->get_workflow_sop(*id, *version, &row, &error)) { fail(error.empty() ? 404 : 500, error.empty() ? "SOP version not found" : "failed to load SOP version"); return; }
      Json::Value sop;
      if (!verified_definition(row, &sop)) { fail(500, "stored SOP integrity check failed"); return; }
      result["definition"] = metadata(row); result["definition"]["sop"] = std::move(sop);
    } else {
      int64_t limit = 25, before = 0;
      const auto l = query_get(req.query, "limit"), b = query_get(req.query, "before_id");
      if ((l && (!parse_positive(*l, &limit) || limit > 100)) || (b && !parse_positive(*b, &before))) { fail(400, "invalid SOP page: limit 1..100 and positive before_id required"); return; }
      std::vector<AgentDb::WorkflowSopRow> rows;
      if (!db->list_workflow_sops(before, (size_t)limit + 1, &rows, &error)) { fail(500, "failed to list SOP versions"); return; }
      const bool more = rows.size() > (size_t)limit;
      if (more) rows.resize((size_t)limit);
      result["definitions"] = Json::Value(Json::arrayValue);
      for (const auto& row : rows) result["definitions"].append(metadata(row));
      if (more) result["next_before_id"] = (Json::Int64)rows.back().registry_id;
    }
  }
  resp->body = stringify(result);
}
}  // namespace agentd
