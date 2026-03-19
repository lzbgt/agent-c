#include "edge_runtime_endpoints.h"

#include "daemon_auth.h"
#include "edge_consensus_runtime_backend.h"
#include "edge_consensus_runtime_control.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_model.h"
#include "edge_consensus_runtime_policy.h"
#include "edge_consensus_runtime_recovery.h"
#include "edge_consensus_runtime_reuse.h"
#include "edge_consensus_runtime_registry.h"
#include "edge_consensus_runtime_snapshot.h"
#include "edge_consensus_runtime_store.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <memory>
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
    EdgeConsensusRuntimeSnapshot snapshot;
    std::string lerr;
    Json::Value recovery_updates(Json::objectValue);
    if (!edge_consensus_runtime_resolve_snapshot(cfg, db_or_null, node_id, &snapshot, &recovery_updates, &lerr)) {
      out["error"] = lerr.empty() ? "failed to resolve persisted consensus runtime state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    edge_consensus_runtime_append_recovery_updates(recovery_updates, &out);
    std::shared_ptr<EdgeConsensusRuntime> st = snapshot.runtime;
    EdgeConsensusRuntimeStopResult stop_result;
    if (!edge_consensus_runtime_stop(
          cfg, db_or_null, st, edge_consensus_runtime_registry_mutex(), &stop_result, &lerr)) {
      out["error"] = lerr.empty() ? "failed to stop consensus runtime" : lerr;
      out["runtime"] = st ? edge_consensus_runtime_response_json(cfg, *st) : Json::Value(Json::nullValue);
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    out["runtime"] = stop_result.runtime;
    if (stop_result.disposition == EdgeConsensusRuntimeStopDisposition::unsupported) {
      out["error"] = "consensus_runtime stop unsupported on Windows";
      resp->status = 501;
      resp->body = json_stringify(out);
      return;
    }
    if (stop_result.disposition == EdgeConsensusRuntimeStopDisposition::not_running) {
      out["ok"] = true;
      out["stopped"] = false;
      out["reason"] = "not_running";
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    out["ok"] = true;
    out["stopped"] = true;
    resp->status = 200;
    resp->body = json_stringify(out);
    return;
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
    EdgeConsensusRuntimeSnapshot snapshot;
    std::string lerr;
    Json::Value recovery_updates(Json::objectValue);
    if (!edge_consensus_runtime_resolve_snapshot(cfg, db_or_null, node_id, &snapshot, &recovery_updates, &lerr)) {
      out["error"] = lerr.empty() ? "failed to resolve persisted consensus runtime state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    edge_consensus_runtime_append_recovery_updates(recovery_updates, &out);
    auto st = snapshot.runtime;
    if (st && st->running) {
      EdgeConsensusRuntimeReuseResult reuse;
      std::string reuse_err;
      if (!edge_consensus_runtime_evaluate_reuse(cfg, body, runtime_kind, *st, &reuse, &reuse_err)) {
        out["error"] = reuse_err.empty() ? "failed to evaluate running consensus runtime reuse" : reuse_err;
        resp->status = 400;
        resp->body = json_stringify(out);
        return;
      }
      out["runtime"] = reuse.runtime;
      if (reuse.disposition == EdgeConsensusRuntimeReuseDisposition::invalid_request) {
        out["error"] = reuse.error;
        resp->status = 400;
        resp->body = json_stringify(out);
        return;
      }
      if (reuse.disposition == EdgeConsensusRuntimeReuseDisposition::conflict) {
        out["error"] = reuse.error;
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

#if defined(_WIN32)
  out["error"] = "consensus_runtime start unsupported on Windows";
  resp->status = 501;
  resp->body = json_stringify(out);
  return;
#else
  std::shared_ptr<EdgeConsensusRuntime> spawned;
  std::string serr;
  const EdgeConsensusRuntimePersistFn persist_on_exit =
    make_edge_consensus_runtime_persist_on_exit(db_or_null);
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
  Json::Value runtime(Json::nullValue);
  if (!edge_consensus_runtime_activate_started(
        cfg, db_or_null, node_id, spawned, edge_consensus_runtime_registry_mutex(), &runtime, &serr)) {
    out["error"] = serr.empty() ? "failed to start consensus runtime" : serr;
    out["startup_confirmed"] = false;
    out["runtime"] = runtime;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  out["startup_confirmed"] = true;
  out["runtime"] = runtime;
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

  EdgeConsensusRuntimeSnapshot snapshot;
  std::string lerr;
  Json::Value recovery_updates(Json::objectValue);
  if (!edge_consensus_runtime_resolve_snapshot(cfg, db_or_null, *nid, &snapshot, &recovery_updates, &lerr)) {
    out["error"] = lerr.empty() ? "failed to resolve persisted consensus runtime state" : lerr;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  edge_consensus_runtime_append_recovery_updates(recovery_updates, &out);
  std::shared_ptr<EdgeConsensusRuntime> st = snapshot.runtime;
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
