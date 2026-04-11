#include "config_endpoint.h"

#include "daemon_auth.h"
#include "edge_consensus_membership_bundle.h"
#include "edge_consensus_runtime_policy.h"
#include "edge_platform_bundle.h"
#include "edge_util.h"
#include "http_util.h"
#include "json_util.h"
#include "runtime_config.h"
#include "string_util.h"

#include "agent/edge_interop.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace agentd {
namespace {

static bool lineage_contains_epoch(
  const std::vector<EdgeConsensusMembershipLineageEntry>& lineage,
  int64_t membership_epoch
) {
  for (const auto& entry : lineage) {
    if (entry.membership_epoch == membership_epoch) return true;
  }
  return false;
}

static std::vector<EdgeConsensusMembershipLineageEntry> build_membership_lineage(
  const EdgeConsensusClusterPolicy* current_policy,
  int64_t current_epoch,
  const std::vector<std::string>& current_members
) {
  std::vector<EdgeConsensusMembershipLineageEntry> lineage;
  if (current_epoch > 0 && !current_members.empty()) {
    EdgeConsensusMembershipLineageEntry entry;
    entry.membership_epoch = current_epoch;
    entry.member_node_ids = current_members;
    lineage.push_back(std::move(entry));
  }
  if (current_policy) {
    for (const auto& prior : current_policy->membership_lineage) {
      if (prior.membership_epoch <= 0 || prior.member_node_ids.empty()) continue;
      if (prior.membership_epoch >= current_epoch) continue;
      if (lineage_contains_epoch(lineage, prior.membership_epoch)) continue;
      EdgeConsensusMembershipLineageEntry entry;
      entry.membership_epoch = prior.membership_epoch;
      entry.member_node_ids = edge_consensus_normalize_member_node_ids(prior.member_node_ids);
      if (entry.member_node_ids.empty()) continue;
      lineage.push_back(std::move(entry));
      if (lineage.size() >= AGENT_EDGE_CONSENSUS_MEMBERSHIP_LINEAGE_MAX) break;
    }
  }
  return lineage;
}

static bool parse_json_integer_field(
  const Json::Value& args,
  const char* field,
  int64_t* out_value,
  HttpResponse* resp
) {
  if (!args.isMember(field)) return true;
  if (!args[field].isInt64() && !args[field].isUInt64()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string(field) + " must be an integer";
    resp->status = 400;
    resp->body = json_stringify(o);
    return false;
  }
  if (out_value) {
    *out_value = args[field].isInt64() ? args[field].asInt64() : (int64_t)args[field].asUInt64();
  }
  return true;
}

}  // namespace

void handle_edge_consensus_membership_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto cluster_id_q = query_get(req.query, "cluster_id");
  const std::string cluster_id = cluster_id_q ? trim_copy(*cluster_id_q) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_consensus_membership_bundle(cfg, cluster_id, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr.rfind("attest_sign_config_invalid:", 0) == 0) {
      o["error"] = "attest_sign_config_invalid";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_config_invalid:").size()));
      resp->status = 500;
    } else if (berr.rfind("attest_sign_failed:", 0) == 0) {
      o["error"] = "attest_sign_failed";
      o["details"] = trim_copy(berr.substr(std::string("attest_sign_failed:").size()));
      resp->status = 500;
    } else if (berr == "consensus_membership_unavailable") {
      o["error"] = berr;
      o["cluster_id"] = cluster_id;
      resp->status = 404;
    } else {
      o["error"] = berr.empty() ? "consensus_membership_unavailable" : berr;
      resp->status = 500;
    }
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cluster_id"] = cluster_id;
  o["membership"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_consensus_membership_rotate_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store || !db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  const DaemonConfig cur = cfg_store->snapshot();
  if (!daemon_require_auth(cur, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  const std::string cluster_id = args.isMember("cluster_id") && args["cluster_id"].isString()
    ? trim_copy(args["cluster_id"].asString()) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? trim_copy(args["mode"].asString()) : "replace";
  if (mode != "merge" && mode != "replace") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "mode must be merge or replace";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }
  if (!args.isMember("member_node_ids") || !args["member_node_ids"].isArray()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "member_node_ids must be an array";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  std::vector<std::string> incoming_members;
  for (const auto& item : args["member_node_ids"]) {
    if (!item.isString()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "member_node_ids entries must be strings";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    incoming_members.push_back(item.asString());
  }
  incoming_members = edge_consensus_normalize_member_node_ids(incoming_members);
  if (incoming_members.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "member_node_ids must not be empty";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  int64_t new_epoch = 0;
  const auto cur_it = cur.edge_consensus_clusters.find(cluster_id);
  const int64_t cur_epoch = cur_it == cur.edge_consensus_clusters.end() ? 0 : cur_it->second.membership_epoch;
  const std::vector<std::string> previous_members =
    cur_it == cur.edge_consensus_clusters.end()
      ? std::vector<std::string>{}
      : edge_consensus_normalize_member_node_ids(cur_it->second.member_node_ids);
  new_epoch = cur_epoch + 1;
  if (args.isMember("membership_epoch")) {
    if (!args["membership_epoch"].isInt64() && !args["membership_epoch"].isUInt64()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "membership_epoch must be an integer";
      resp->status = 400;
      resp->body = json_stringify(o);
      return;
    }
    new_epoch = args["membership_epoch"].isInt64() ? args["membership_epoch"].asInt64() : (int64_t)args["membership_epoch"].asUInt64();
  }
  if (new_epoch < 0 ||
      !agent_edge_consensus_membership_epoch_can_advance(
        (uint64_t)std::max<int64_t>(0, cur_epoch),
        (uint64_t)new_epoch)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "membership_epoch must be strictly greater than current epoch";
    o["current_epoch"] = (Json::Int64)cur_epoch;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  EdgeConsensusClusterPolicy next_pol;
  if (cur_it != cur.edge_consensus_clusters.end() && mode == "merge") next_pol = cur_it->second;
  for (const auto& member : incoming_members) {
    edge_consensus_append_member_node_id_if_unique(&next_pol.member_node_ids, member);
  }
  next_pol.member_node_ids = edge_consensus_normalize_member_node_ids(next_pol.member_node_ids);
  if (next_pol.member_node_ids.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "effective member_node_ids must not be empty";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }
  next_pol.membership_epoch = new_epoch;
  next_pol.previous_membership_epoch = std::max<int64_t>(0, cur_epoch);
  next_pol.previous_member_node_ids = previous_members;
  next_pol.membership_lineage = build_membership_lineage(
    cur_it == cur.edge_consensus_clusters.end() ? nullptr : &cur_it->second,
    cur_epoch,
    previous_members);
  next_pol.updated_utc_ms = edge_unix_ms_now();
  if (!parse_json_integer_field(args, "campaign_delay_ms", &next_pol.campaign_delay_ms, resp)) return;
  if (!parse_json_integer_field(args, "campaign_retry_ms", &next_pol.campaign_retry_ms, resp)) return;
  if (!parse_json_integer_field(args, "campaign_retry_max_ms", &next_pol.campaign_retry_max_ms, resp)) return;
  if (!parse_json_integer_field(args, "campaign_retry_backoff_factor", &next_pol.campaign_retry_backoff_factor, resp)) return;
  if (!parse_json_integer_field(args, "leader_heartbeat_ms", &next_pol.leader_heartbeat_ms, resp)) return;
  if (!parse_json_integer_field(args, "leader_lease_ms", &next_pol.leader_lease_ms, resp)) return;
  if (!parse_json_integer_field(args, "lease_expiry_recampaign_delay_ms", &next_pol.lease_expiry_recampaign_delay_ms, resp)) return;
  if (!parse_json_integer_field(args, "stale_runtime_recovery_grace_ms", &next_pol.stale_runtime_recovery_grace_ms, resp)) return;
  edge_consensus_normalize_policy_timing(&next_pol);

  DaemonConfig next = cur;
  next.edge_consensus_clusters[cluster_id] = next_pol;

  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  cfg_store->replace(next);

  Json::Value bundle;
  std::string berr;
  if (!build_edge_consensus_membership_bundle(next, cluster_id, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = berr.empty() ? "consensus_membership_unavailable" : berr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cluster_id"] = cluster_id;
  o["membership_epoch"] = (Json::Int64)next_pol.membership_epoch;
  o["previous_membership_epoch"] = (Json::Int64)next_pol.previous_membership_epoch;
  o["updated_utc_ms"] = (Json::Int64)next_pol.updated_utc_ms;
  o["campaign_delay_ms"] = (Json::Int64)next_pol.campaign_delay_ms;
  o["campaign_retry_ms"] = (Json::Int64)next_pol.campaign_retry_ms;
  o["campaign_retry_max_ms"] = (Json::Int64)next_pol.campaign_retry_max_ms;
  o["campaign_retry_backoff_factor"] = (Json::Int64)next_pol.campaign_retry_backoff_factor;
  o["leader_heartbeat_ms"] = (Json::Int64)next_pol.leader_heartbeat_ms;
  o["leader_lease_ms"] = (Json::Int64)next_pol.leader_lease_ms;
  o["lease_expiry_recampaign_delay_ms"] = (Json::Int64)next_pol.lease_expiry_recampaign_delay_ms;
  o["stale_runtime_recovery_grace_ms"] = (Json::Int64)next_pol.stale_runtime_recovery_grace_ms;
  o["member_count"] = (Json::UInt64)next_pol.member_node_ids.size();
  o["previous_member_count"] = (Json::UInt64)next_pol.previous_member_node_ids.size();
  o["membership"] = bundle;
  resp->body = json_stringify(o);
}

void handle_edge_consensus_membership_send_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }
  const std::string cluster_id = args.isMember("cluster_id") && args["cluster_id"].isString()
    ? trim_copy(args["cluster_id"].asString()) : "";
  const std::string target_node_id = args.isMember("target_node_id") && args["target_node_id"].isString()
    ? trim_copy(args["target_node_id"].asString()) : "";
  const std::string confidential_kid = args.isMember("confidential_kid") && args["confidential_kid"].isString()
    ? trim_copy(args["confidential_kid"].asString()) : "";
  if (!edge_id_is_safe(cluster_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid cluster_id");
    return;
  }
  if (!edge_consensus_member_node_id_is_valid(target_node_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid target_node_id");
    return;
  }

  Json::Value bundle;
  std::string berr;
  if (!build_edge_consensus_membership_bundle(cfg, cluster_id, &bundle, &berr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    if (berr == "consensus_membership_unavailable") {
      o["error"] = berr;
      o["cluster_id"] = cluster_id;
      resp->status = 404;
    } else {
      o["error"] = berr.empty() ? "consensus_membership_unavailable" : berr;
      resp->status = 500;
    }
    resp->body = json_stringify(o);
    return;
  }

  int64_t outbox_id = 0;
  std::string oerr;
  if (!enqueue_edge_platform_bundle(
        db,
        target_node_id,
        AGENT_UM_BMP_TYPE_PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE,
        "membership",
        bundle,
        &cfg.edge_confidentiality_keys,
        confidential_kid,
        &outbox_id,
        &oerr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = oerr.empty() ? "failed to enqueue consensus membership bundle" : oerr;
    resp->status = o["error"].asString() == "target node not found" ? 404 : 400;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["cluster_id"] = cluster_id;
  o["target_node_id"] = target_node_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  if (!confidential_kid.empty()) o["confidential_kid"] = confidential_kid;
  o["membership"] = bundle;
  resp->body = json_stringify(o);
}

}  // namespace agentd
