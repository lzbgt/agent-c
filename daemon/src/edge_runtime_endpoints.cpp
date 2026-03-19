#include "edge_runtime_endpoints.h"

#include "daemon_auth.h"
#include "edge_consensus_runtime_backend.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_model.h"
#include "edge_consensus_runtime_policy.h"
#include "edge_consensus_runtime_recovery.h"
#include "edge_consensus_runtime_registry.h"
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
#include <thread>
#include <vector>

namespace agentd {
namespace {

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
    st = edge_consensus_runtime_lookup_active(node_id);
    if (st) {
      std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
      refresh_edge_consensus_runtime_state(st.get());
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
      edge_consensus_runtime_append_recovery_updates(updates, &out);
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
      EdgeConsensusPersistedRunningReconcileResult reconcile;
      std::string rerr;
      if (!edge_consensus_runtime_reconcile_persisted_running(
            cfg, db_or_null, node_id, st, &reconcile, &rerr)) {
        out["error"] = rerr.empty() ? "failed to reconcile persisted consensus runtime state" : rerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
      if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::active_external) {
        edge_consensus_runtime_remember_active(st);
      } else if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::stale_cleared) {
        out["cleanup_on_stale_record"] = reconcile.cleanup;
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
    if (!edge_consensus_runtime_kill_best_effort(st, edge_consensus_runtime_registry_mutex(), &signal_used, &serr)) {
      out["error"] = serr.empty() ? "failed to stop consensus runtime" : serr;
      {
        std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
        out["runtime"] = edge_consensus_runtime_response_json(cfg, *st);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
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
    auto st = edge_consensus_runtime_lookup_active(node_id);
    if (st) {
      std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
      refresh_edge_consensus_runtime_state(st.get());
    }
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
    if (st && !st->running) {
      edge_consensus_runtime_forget_active(node_id);
    }
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
    edge_consensus_runtime_append_recovery_updates(updates, &out);
    if (persisted && persisted->running) {
      EdgeConsensusPersistedRunningReconcileResult reconcile;
      std::string rerr;
      if (!edge_consensus_runtime_reconcile_persisted_running(
            cfg, db_or_null, node_id, persisted, &reconcile, &rerr)) {
        out["error"] = rerr.empty() ? "failed to reconcile persisted consensus runtime state" : rerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
      }
      if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::active_external) {
        edge_consensus_runtime_remember_active(persisted);
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
      if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::stale_cleared) {
        out["cleanup_on_stale_record"] = reconcile.cleanup;
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
        cfg, db_or_null, body, edge_consensus_runtime_registry_mutex(), persist_on_exit, &spawned, &serr)
    : edge_consensus_runtime_start_builtin(
        cfg, db_or_null, body, edge_consensus_runtime_registry_mutex(), persist_on_exit, &spawned, &serr);
  if (!started) {
    out["error"] = serr.empty() ? "failed to start consensus runtime" : serr;
    out["startup_confirmed"] = false;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  Json::Value startup_runtime(Json::nullValue);
  if (!edge_consensus_runtime_confirm_startup(spawned, edge_consensus_runtime_registry_mutex(), &startup_runtime, &serr)) {
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
  edge_consensus_runtime_remember_active(spawned);
  out["runtime"] = edge_consensus_runtime_response_json(cfg, *spawned);
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
  st = edge_consensus_runtime_lookup_active(*nid);
  if (st) {
    std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
    refresh_edge_consensus_runtime_state(st.get());
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
    edge_consensus_runtime_append_recovery_updates(updates, &out);
  }
  if (st && st->status_source == "persisted" && st->running) {
    EdgeConsensusPersistedRunningReconcileResult reconcile;
    std::string rerr;
    if (!edge_consensus_runtime_reconcile_persisted_running(
          cfg, db_or_null, *nid, st, &reconcile, &rerr)) {
      out["error"] = rerr.empty() ? "failed to reconcile persisted consensus runtime state" : rerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::active_external) {
      edge_consensus_runtime_remember_active(st);
    } else if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::stale_cleared) {
      out["cleanup_on_stale_record"] = reconcile.cleanup;
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
