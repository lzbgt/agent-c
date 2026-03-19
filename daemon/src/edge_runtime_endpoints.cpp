#include "edge_runtime_endpoints.h"

#include "daemon_auth.h"
#include "edge_consensus_runtime_backend.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_model.h"
#include "edge_consensus_runtime_policy.h"
#include "edge_consensus_runtime_store.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agentd {
namespace {

static std::vector<std::string> dedupe_safe_edge_ids(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (const auto& raw : in) {
    const std::string s = trim_copy(raw);
    if (!edge_id_is_safe(s)) continue;
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  }
  return out;
}

static std::mutex g_edge_consensus_runtime_mu;
static std::unordered_map<std::string, std::shared_ptr<EdgeConsensusRuntime>> g_edge_consensus_runtime_by_node;

static std::shared_ptr<EdgeConsensusRuntime> edge_consensus_runtime_lookup_locked(const std::string& node_id) {
  const auto it = g_edge_consensus_runtime_by_node.find(node_id);
  return it == g_edge_consensus_runtime_by_node.end() ? nullptr : it->second;
}

static Json::Value build_edge_node_consensus_summary(AgentDb* db_or_null, const std::string& node_id, bool* out_exists) {
  if (out_exists) *out_exists = false;
  Json::Value out(Json::nullValue);
  if (!db_or_null || !db_or_null->is_open() || node_id.empty()) return out;
  AgentDb::EdgeNodeRow row;
  std::string err;
  if (!db_or_null->get_edge_node(node_id, &row, &err)) return out;
  if (out_exists) *out_exists = true;
  if (!row.health_json.empty()) {
    Json::Value health(Json::nullValue);
    std::string perr;
    if (json_parse_any(row.health_json, &health, &perr) && health.isObject() &&
        health.isMember("consensus") && health["consensus"].isObject()) {
      out = health["consensus"];
    }
  }
  return out;
}

}  // namespace

Json::Value edge_consensus_runtime_status_json_for_node(const DaemonConfig& cfg, AgentDb* db_or_null, const std::string& node_id) {
  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    auto st = edge_consensus_runtime_lookup_locked(node_id);
    if (st) {
      refresh_edge_consensus_runtime_state(st.get());
      std::string perr;
      (void)persist_edge_consensus_runtime_record(db_or_null, *st, &perr);
      return edge_consensus_runtime_response_json(cfg, *st);
    }
  }
  if (!db_or_null || !db_or_null->is_open()) return Json::Value(Json::nullValue);
  std::shared_ptr<EdgeConsensusRuntime> persisted;
  Json::Value updates(Json::objectValue);
  std::string err;
  if (!recover_edge_consensus_runtime_record(cfg, db_or_null, node_id, &persisted, &updates, &err)) {
    return Json::Value(Json::nullValue);
  }
  if (!persisted) return Json::Value(Json::nullValue);
  if (persisted && persisted->running) {
    refresh_edge_consensus_runtime_state(persisted.get());
    if (persisted->running && persisted->runtime_kind == "external") {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      g_edge_consensus_runtime_by_node[node_id] = persisted;
      std::string perr;
      (void)persist_edge_consensus_runtime_record(db_or_null, *persisted, &perr);
      return edge_consensus_runtime_response_json(cfg, *persisted);
    }
    std::string cerr;
    (void)clear_edge_consensus_runtime_record(db_or_null, node_id, &cerr);
    bool artifacts_deleted = false;
    std::string aerr;
    (void)remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr);
    return Json::Value(Json::nullValue);
  }
  return edge_consensus_runtime_response_json(cfg, *persisted);
}

std::vector<std::string> edge_consensus_runtime_node_ids(AgentDb* db_or_null, size_t limit) {
  if (limit == 0) return {};
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    ids.reserve(g_edge_consensus_runtime_by_node.size());
    for (const auto& kv : g_edge_consensus_runtime_by_node) ids.push_back(kv.first);
  }
  if (db_or_null && db_or_null->is_open()) {
    std::vector<AgentDb::MetaRow> rows;
    std::string err;
    const size_t db_limit = std::max<size_t>(limit, 256);
    if (db_or_null->list_meta_prefix("edge.consensus_runtime.", db_limit, &rows, &err)) {
      for (const auto& row : rows) {
        constexpr size_t kPrefixLen = sizeof("edge.consensus_runtime.") - 1;
        if (row.key.size() <= kPrefixLen) continue;
        ids.push_back(row.key.substr(kPrefixLen));
      }
    }
  }
  ids = dedupe_safe_edge_ids(ids);
  std::sort(ids.begin(), ids.end());
  if (ids.size() > limit) ids.resize(limit);
  return ids;
}

void handle_edge_node_consensus_runtime_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
    return;
  }

  const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
  if (!edge_id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid node_id");
    return;
  }
  const std::string action = body.isMember("action") && body["action"].isString() ? lower_copy(trim_copy(body["action"].asString())) : "";
  if (action != "start" && action != "stop") {
    resp->status = 400;
    resp->body = json_error_body("action must be start or stop");
    return;
  }
  const std::string req_cluster_id =
    body.isMember("cluster_id") && body["cluster_id"].isString() ? trim_copy(body["cluster_id"].asString()) : "";

  Json::Value out = edge_consensus_runtime_backend_metadata_json(cfg);
  out["ok"] = false;
  out["node_id"] = node_id;
  const auto pol_it = cfg.edge_consensus_clusters.find(req_cluster_id);
  if (pol_it != cfg.edge_consensus_clusters.end()) out["cluster_policy"] = edge_consensus_cluster_policy_to_json(pol_it->first, pol_it->second);

  if (action == "stop") {
    std::shared_ptr<EdgeConsensusRuntime> st;
    {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      st = edge_consensus_runtime_lookup_locked(node_id);
      if (st) refresh_edge_consensus_runtime_state(st.get());
    }
    if (!st) {
      std::string lerr;
      Json::Value updates(Json::objectValue);
      if (!recover_edge_consensus_runtime_record(cfg, db_or_null, node_id, &st, &updates, &lerr)) {
        out["error"] = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
      if (updates.isObject()) {
        if (updates.isMember("cleanup_on_corrupt_record")) {
          out["cleanup_on_corrupt_record"] = updates["cleanup_on_corrupt_record"];
        }
        if (updates.isMember("cleanup_on_stale_record")) {
          out["cleanup_on_stale_record"] = updates["cleanup_on_stale_record"];
        }
      }
    }
    if (!st) {
      out["ok"] = true;
      out["stopped"] = false;
      out["reason"] = "not_running";
      out["runtime"] = Json::Value(Json::nullValue);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    if (st->status_source == "persisted" && st->running) {
      refresh_edge_consensus_runtime_state(st.get());
      if (st->running && st->runtime_kind == "external") {
        std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
        g_edge_consensus_runtime_by_node[node_id] = st;
      } else if (st->running) {
        Json::Value cleanup(Json::objectValue);
        cleanup["persisted_record_cleared"] = clear_edge_consensus_runtime_record(db_or_null, node_id, nullptr);
        bool artifacts_deleted = false;
        std::string aerr;
        if (remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr)) {
          cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
        } else if (!aerr.empty()) {
          cleanup["runtime_artifacts_delete_error"] = aerr;
        }
        out["cleanup_on_stale_record"] = cleanup;
        out["ok"] = true;
        out["stopped"] = false;
        out["reason"] = "not_running";
        out["runtime"] = Json::Value(Json::nullValue);
        resp->status = 200;
        resp->body = json_stringify(out);
        return;
      }
    }
    if (!st->running) {
      std::string perr;
      (void)persist_edge_consensus_runtime_record(db_or_null, *st, &perr);
      out["ok"] = true;
      out["stopped"] = false;
      out["reason"] = "not_running";
      out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
#if defined(_WIN32)
    out["error"] = "consensus_runtime stop unsupported on Windows";
    out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
#else
    std::string serr;
    int signal_used = 0;
    if (!edge_consensus_runtime_kill_best_effort(st, g_edge_consensus_runtime_mu, &signal_used, &serr)) {
      out["error"] = serr.empty() ? "failed to stop consensus runtime" : serr;
      {
        std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
        out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      refresh_edge_consensus_runtime_state(st.get());
      finalize_recovered_edge_consensus_stop(st.get(), signal_used);
      out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
    }
    std::string perr;
    (void)persist_edge_consensus_runtime_record(db_or_null, *st, &perr);
    out["ok"] = true;
    out["stopped"] = true;
    resp->status = 200;
    resp->body = json_stringify(out);
    return;
#endif
  }

  const std::string cluster_id = req_cluster_id;
  const std::string runtime_kind =
    body.isMember("runtime_kind") && body["runtime_kind"].isString()
      ? lower_copy(trim_copy(body["runtime_kind"].asString()))
      : default_edge_consensus_runtime_kind(cfg);
  if (runtime_kind != "builtin" && runtime_kind != "external") {
    resp->status = 400;
    resp->body = json_error_body("runtime_kind must be builtin or external");
    return;
  }
  const std::string manifest_sha256 =
    body.isMember("manifest_sha256") && body["manifest_sha256"].isString() ? trim_copy(body["manifest_sha256"].asString()) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }
  if (!edge_sha256_token_is_safe(manifest_sha256)) {
    resp->status = 400;
    resp->body = json_error_body("invalid manifest_sha256");
    return;
  }
  if (body.isMember("decision_sha256") && body["decision_sha256"].isString() &&
      !trim_copy(body["decision_sha256"].asString()).empty() &&
      !edge_sha256_token_is_safe(trim_copy(body["decision_sha256"].asString()))) {
    resp->status = 400;
    resp->body = json_error_body("invalid decision_sha256");
    return;
  }

  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    auto st = edge_consensus_runtime_lookup_locked(node_id);
    if (st) refresh_edge_consensus_runtime_state(st.get());
    if (st && st->running) {
      EdgeConsensusHttpRuntimeConfig desired_cfg;
      EdgeConsensusRuntime desired_state;
      std::string conflict_err;
      if (!edge_consensus_runtime_build_config(cfg, body, &desired_cfg, &desired_state, &conflict_err)) {
        out["error"] = conflict_err.empty() ? "failed to validate requested consensus runtime config" : conflict_err;
        out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
        resp->status = 400;
        resp->body = json_stringify(out);
        return;
      }
      desired_state.runtime_kind = runtime_kind;
      desired_state.tool_path = runtime_kind == "external" ? trim_copy(cfg.edge_consensus_node_tool_path) : "@builtin";
      if (!edge_consensus_runtime_same_effective_config(*st, desired_state)) {
        out["error"] = "consensus runtime already running with different config";
        out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
        resp->status = 409;
        resp->body = json_stringify(out);
        return;
      }
      out["ok"] = true;
      out["already_running"] = true;
      out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    if (st && !st->running) g_edge_consensus_runtime_by_node.erase(node_id);
  }
  {
    std::shared_ptr<EdgeConsensusRuntime> persisted;
    std::string lerr;
    Json::Value updates(Json::objectValue);
    if (!recover_edge_consensus_runtime_record(cfg, db_or_null, node_id, &persisted, &updates, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (updates.isObject()) {
      if (updates.isMember("cleanup_on_corrupt_record")) {
        out["cleanup_on_corrupt_record"] = updates["cleanup_on_corrupt_record"];
      }
      if (updates.isMember("cleanup_on_stale_record")) {
        out["cleanup_on_stale_record"] = updates["cleanup_on_stale_record"];
      }
    }
    if (persisted && persisted->running) {
      refresh_edge_consensus_runtime_state(persisted.get());
      if (persisted->running && persisted->runtime_kind == "external") {
        {
          std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
          g_edge_consensus_runtime_by_node[node_id] = persisted;
        }
        EdgeConsensusHttpRuntimeConfig desired_cfg;
        EdgeConsensusRuntime desired_state;
        std::string conflict_err;
        if (!edge_consensus_runtime_build_config(cfg, body, &desired_cfg, &desired_state, &conflict_err)) {
          out["error"] = conflict_err.empty() ? "failed to validate requested consensus runtime config" : conflict_err;
          out["runtime"] = edge_consensus_runtime_response_json(cfg, *persisted);
          resp->status = 400;
          resp->body = json_stringify(out);
          return;
        }
        desired_state.runtime_kind = runtime_kind;
        desired_state.tool_path = runtime_kind == "external" ? trim_copy(cfg.edge_consensus_node_tool_path) : "@builtin";
        out["runtime"] = edge_consensus_runtime_response_json(cfg, *persisted);
        std::string perr;
        (void)persist_edge_consensus_runtime_record(db_or_null, *persisted, &perr);
        if (!edge_consensus_runtime_same_effective_config(*persisted, desired_state)) {
          out["error"] = "consensus runtime already running with different config";
          resp->status = 409;
          resp->body = json_stringify(out);
          return;
        }
        out["ok"] = true;
        out["already_running"] = true;
        resp->status = 200;
        resp->body = json_stringify(out);
        return;
      }
      if (persisted->running) {
        Json::Value cleanup(Json::objectValue);
        cleanup["persisted_record_cleared"] = clear_edge_consensus_runtime_record(db_or_null, node_id, nullptr);
        bool artifacts_deleted = false;
        std::string aerr;
        if (remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr)) {
          cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
        } else if (!aerr.empty()) {
          cleanup["runtime_artifacts_delete_error"] = aerr;
        }
        out["cleanup_on_stale_record"] = cleanup;
      }
    }
  }

#if defined(_WIN32)
  out["error"] = "consensus_runtime start unsupported on Windows";
  resp->status = 501;
  resp->body = json_stringify(out);
  return;
#else
  std::shared_ptr<EdgeConsensusRuntime> spawned;
  std::string serr;
  const EdgeConsensusRuntimePersistFn persist_on_exit =
    [db = db_or_null](const EdgeConsensusRuntime& snapshot) {
      std::string perr;
      (void)persist_edge_consensus_runtime_record(db, snapshot, &perr);
    };
  const bool started = runtime_kind == "external"
    ? edge_consensus_runtime_spawn_external(
        cfg, db_or_null, body, g_edge_consensus_runtime_mu, persist_on_exit, &spawned, &serr)
    : edge_consensus_runtime_start_builtin(
        cfg, db_or_null, body, g_edge_consensus_runtime_mu, persist_on_exit, &spawned, &serr);
  if (!started) {
    out["error"] = serr.empty() ? "failed to start consensus runtime" : serr;
    out["startup_confirmed"] = false;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  Json::Value startup_runtime(Json::nullValue);
  if (!edge_consensus_runtime_confirm_startup(spawned, g_edge_consensus_runtime_mu, &startup_runtime, &serr)) {
    out["error"] = serr.empty() ? "failed to start consensus runtime" : serr;
    out["startup_confirmed"] = false;
    out["runtime"] = startup_runtime;
    std::string cerr;
    (void)clear_edge_consensus_runtime_record(db_or_null, node_id, &cerr);
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  out["startup_confirmed"] = true;
  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    g_edge_consensus_runtime_by_node[node_id] = spawned;
    out["runtime"] = edge_consensus_runtime_response_json(cfg, *spawned);
  }
  std::string perr;
  (void)persist_edge_consensus_runtime_record(db_or_null, *spawned, &perr);
  out["ok"] = true;
  out["started"] = true;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

void handle_edge_node_consensus_runtime_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !edge_id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid node_id");
    return;
  }

  Json::Value out = edge_consensus_runtime_backend_metadata_json(cfg);
  out["ok"] = true;
  out["node_id"] = *nid;

  bool node_exists = false;
  Json::Value consensus = build_edge_node_consensus_summary(db_or_null, *nid, &node_exists);
  out["node_exists"] = node_exists;
  if (consensus.isObject()) out["node_consensus"] = consensus;

  std::shared_ptr<EdgeConsensusRuntime> st;
  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    st = edge_consensus_runtime_lookup_locked(*nid);
    if (st) refresh_edge_consensus_runtime_state(st.get());
  }
  if (!st) {
    std::string lerr;
    Json::Value updates(Json::objectValue);
    if (!recover_edge_consensus_runtime_record(cfg, db_or_null, *nid, &st, &updates, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (updates.isObject()) {
      if (updates.isMember("cleanup_on_corrupt_record")) {
        out["cleanup_on_corrupt_record"] = updates["cleanup_on_corrupt_record"];
      }
      if (updates.isMember("cleanup_on_stale_record")) {
        out["cleanup_on_stale_record"] = updates["cleanup_on_stale_record"];
      }
    }
  }
  if (st && st->status_source == "persisted" && st->running) {
    refresh_edge_consensus_runtime_state(st.get());
    if (st->running && st->runtime_kind == "external") {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      g_edge_consensus_runtime_by_node[*nid] = st;
    } else if (st->running) {
      Json::Value cleanup(Json::objectValue);
      cleanup["persisted_record_cleared"] = clear_edge_consensus_runtime_record(db_or_null, *nid, nullptr);
      bool artifacts_deleted = false;
      std::string aerr;
      if (remove_edge_consensus_runtime_artifacts(cfg, *nid, &artifacts_deleted, &aerr)) {
        cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
      } else if (!aerr.empty()) {
        cleanup["runtime_artifacts_delete_error"] = aerr;
      }
      out["cleanup_on_stale_record"] = cleanup;
      st.reset();
    }
  }
  out["runtime"] = st ? edge_consensus_runtime_response_json(cfg, *st) : Json::Value(Json::nullValue);
  if (st) {
    std::string perr;
    (void)persist_edge_consensus_runtime_record(db_or_null, *st, &perr);
  }
  if (st) {
    const auto pol_it = cfg.edge_consensus_clusters.find(st->cluster_id);
    if (pol_it != cfg.edge_consensus_clusters.end()) out["cluster_policy"] = edge_consensus_cluster_policy_to_json(pol_it->first, pol_it->second);
  }
  out["running"] = st ? st->running : false;
  resp->status = 200;
  resp->body = json_stringify(out);
}

}  // namespace agentd
