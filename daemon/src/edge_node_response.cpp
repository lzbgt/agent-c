#include "edge_node_response.h"

#include "edge_consensus_runtime_registry.h"
#include "edge_manifest_bundle.h"
#include "json_util.h"
#include "string_util.h"

#include <string>
#include <vector>

namespace agentd {

Json::Value build_edge_node_summary_json(
  const AgentDb::EdgeNodeRow* row_or_null,
  const Json::Value& runtime_or_null,
  const std::string& fallback_node_id
) {
  Json::Value row(Json::objectValue);
  const std::string runtime_node_id =
    runtime_or_null.isObject() && runtime_or_null.isMember("node_id") && runtime_or_null["node_id"].isString()
      ? trim_copy(runtime_or_null["node_id"].asString())
      : std::string();
  const std::string node_id = row_or_null ? row_or_null->node_id : (!runtime_node_id.empty() ? runtime_node_id : fallback_node_id);
  row["node_id"] = node_id;

  if (row_or_null) {
    if (!row_or_null->model.empty()) row["model"] = row_or_null->model;
    if (!row_or_null->fw_git_sha.empty()) row["fw_git_sha"] = row_or_null->fw_git_sha;
    if (!row_or_null->caps_sha256.empty()) row["caps_sha256"] = row_or_null->caps_sha256;
    row["last_hello_utc_ms"] = (Json::Int64)row_or_null->last_hello_utc_ms;
    row["last_heartbeat_utc_ms"] = (Json::Int64)row_or_null->last_heartbeat_utc_ms;
    if (!row_or_null->health_json.empty()) {
      Json::Value v;
      std::string perr2;
      if (json_parse_any(row_or_null->health_json, &v, &perr2) && v.isObject()) {
        row["health"] = v;
        if (v.isMember("consensus") && v["consensus"].isObject()) row["consensus"] = v["consensus"];
      }
    }
  } else {
    if (runtime_or_null.isObject() && runtime_or_null.isMember("model") && runtime_or_null["model"].isString()) {
      row["model"] = trim_copy(runtime_or_null["model"].asString());
    }
    if (runtime_or_null.isObject() && runtime_or_null.isMember("fw_git_sha") && runtime_or_null["fw_git_sha"].isString()) {
      row["fw_git_sha"] = trim_copy(runtime_or_null["fw_git_sha"].asString());
    }
    row["last_hello_utc_ms"] = (Json::Int64)0;
    row["last_heartbeat_utc_ms"] = (Json::Int64)0;
  }

  if (runtime_or_null.isObject()) row["consensus_runtime"] = runtime_or_null;
  return row;
}

void append_runtime_only_edge_node_summaries(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  size_t limit,
  std::unordered_set<std::string>* seen_node_ids,
  Json::Value* arr
) {
  if (!arr || !arr->isArray() || !seen_node_ids || !db_or_null || !db_or_null->is_open()) return;
  if (arr->size() >= limit) return;
  const size_t remaining = limit - arr->size();
  const std::vector<std::string> runtime_node_ids = edge_consensus_runtime_node_ids(db_or_null, remaining + seen_node_ids->size());
  for (const auto& node_id : runtime_node_ids) {
    if (arr->size() >= limit) break;
    if (seen_node_ids->find(node_id) != seen_node_ids->end()) continue;
    Json::Value runtime = edge_consensus_runtime_status_json_for_node(cfg, db_or_null, node_id);
    if (!runtime.isObject()) continue;
    arr->append(build_edge_node_summary_json(nullptr, runtime, node_id));
    seen_node_ids->insert(node_id);
  }
}

void append_edge_node_detail_json(
  const DaemonConfig& cfg,
  const AgentDb::EdgeNodeRow* row_or_null,
  Json::Value* out
) {
  if (!out || !out->isObject()) return;
  if (!row_or_null) {
    (*out)["has_manifest"] = false;
    return;
  }
  if (!row_or_null->tags_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(row_or_null->tags_json, &v, &perr2) && v.isArray()) (*out)["tags"] = v;
  }
  if (!row_or_null->tools_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(row_or_null->tools_json, &v, &perr2) && v.isArray()) (*out)["tools"] = v;
  }
  if (!row_or_null->hardware_presence_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(row_or_null->hardware_presence_json, &v, &perr2) && v.isObject()) (*out)["hardware_presence"] = v;
  }
  if (!row_or_null->manifest_json.empty()) {
    Json::Value manifest;
    std::string perr2;
    Json::Value verify(Json::nullValue);
    bool have_identity_cert = false;
    std::string verr;
    if (json_parse_any(row_or_null->manifest_json, &manifest, &perr2) && manifest.isObject() &&
        build_edge_manifest_identity_cert_verify(cfg, manifest, &verify, &have_identity_cert, &verr) &&
        have_identity_cert && verify.isObject()) {
      (*out)["identity_cert_verify"] = verify;
    }
  }
  (*out)["has_manifest"] = !row_or_null->manifest_json.empty();
}

}  // namespace agentd
