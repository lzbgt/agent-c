#include "edge_consensus_runtime_model.h"

#include "edge_consensus_runtime_policy.h"
#include "edge_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>

namespace agentd {
namespace {

static int64_t now_unix_ms_model() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static bool is_safe_printable_field_model(const std::string& s, size_t max_len) {
  if (s.empty() || s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

static uint64_t json_to_u64_model(const Json::Value& v, uint64_t fallback) {
  if (v.isUInt64()) return v.asUInt64();
  if (v.isInt64() && v.asInt64() >= 0) return (uint64_t)v.asInt64();
  if (v.isUInt()) return (uint64_t)v.asUInt();
  if (v.isInt() && v.asInt() >= 0) return (uint64_t)v.asInt();
  return fallback;
}

static int64_t json_to_i64_model(const Json::Value& v, int64_t fallback) {
  if (v.isInt64()) return v.asInt64();
  if (v.isUInt64() && v.asUInt64() <= (uint64_t)INT64_MAX) return (int64_t)v.asUInt64();
  if (v.isInt()) return (int64_t)v.asInt();
  if (v.isUInt()) return (int64_t)v.asUInt();
  return fallback;
}

static std::vector<std::string> dedupe_safe_edge_ids_model(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (const auto& raw : in) {
    const std::string s = trim_copy(raw);
    if (!edge_id_is_safe(s)) continue;
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  }
  return out;
}

static uint64_t safe_config_epoch_u64_model(int64_t v) {
  return v <= 0 ? 0 : (uint64_t)v;
}

static std::string default_local_daemon_url_model(const DaemonConfig& cfg) {
  std::string host = trim_copy(cfg.listen_host);
  if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]") host = "127.0.0.1";
  return "http://" + host + ":" + std::to_string((int)cfg.listen_port);
}

}  // namespace

Json::Value edge_consensus_cluster_policy_to_json(
  const std::string& cluster_id,
  const EdgeConsensusClusterPolicy& pol
) {
  Json::Value out(Json::objectValue);
  out["schema"] = "edge_consensus_cluster_policy_v1";
  out["cluster_id"] = cluster_id;
  out["membership_epoch"] = (Json::Int64)pol.membership_epoch;
  out["updated_utc_ms"] = (Json::Int64)pol.updated_utc_ms;
  out["campaign_delay_ms"] = (Json::Int64)pol.campaign_delay_ms;
  out["campaign_retry_ms"] = (Json::Int64)pol.campaign_retry_ms;
  out["campaign_retry_max_ms"] = (Json::Int64)pol.campaign_retry_max_ms;
  out["campaign_retry_backoff_factor"] = (Json::Int64)pol.campaign_retry_backoff_factor;
  out["leader_heartbeat_ms"] = (Json::Int64)pol.leader_heartbeat_ms;
  out["leader_lease_ms"] = (Json::Int64)pol.leader_lease_ms;
  out["lease_expiry_recampaign_delay_ms"] = (Json::Int64)pol.lease_expiry_recampaign_delay_ms;
  out["stale_runtime_recovery_grace_ms"] = (Json::Int64)pol.stale_runtime_recovery_grace_ms;
  Json::Value members(Json::arrayValue);
  for (const auto& member : pol.member_node_ids) members.append(member);
  out["member_node_ids"] = members;
  return out;
}

Json::Value edge_consensus_trust_epochs_to_json(
  uint64_t trust_roots_epoch,
  uint64_t revocations_epoch,
  uint64_t cert_roots_epoch
) {
  Json::Value out(Json::objectValue);
  out["trust_roots_epoch"] = Json::UInt64(trust_roots_epoch);
  out["revocations_epoch"] = Json::UInt64(revocations_epoch);
  out["cert_roots_epoch"] = Json::UInt64(cert_roots_epoch);
  return out;
}

Json::Value edge_consensus_runtime_to_json(const EdgeConsensusRuntime& st) {
  Json::Value out(Json::objectValue);
  out["schema"] = "edge_node_consensus_runtime_v1";
  out["runtime_kind"] = st.runtime_kind;
  out["status_source"] = st.status_source.empty() ? "memory" : st.status_source;
  out["node_id"] = st.node_id;
  out["cluster_id"] = st.cluster_id;
  out["manifest_sha256"] = st.manifest_sha256;
  if (!st.decision_sha256.empty()) out["decision_sha256"] = st.decision_sha256;
  Json::Value peers(Json::arrayValue);
  for (const auto& peer : st.peer_node_ids) peers.append(peer);
  out["peer_node_ids"] = peers;
  Json::Value members(Json::arrayValue);
  for (const auto& member : st.member_node_ids) members.append(member);
  out["member_node_ids"] = members;
  out["daemon_url"] = st.daemon_url;
  out["tool_path"] = st.tool_path;
  if (!st.model.empty()) out["model"] = st.model;
  if (!st.fw_git_sha.empty()) out["fw_git_sha"] = st.fw_git_sha;
  if (!st.stderr_log_path.empty()) out["stderr_log_path"] = st.stderr_log_path;
  out["started_unix_ms"] = (Json::Int64)st.started_unix_ms;
  if (st.ended_unix_ms > 0) out["ended_unix_ms"] = (Json::Int64)st.ended_unix_ms;
  out["campaign_delay_ms"] = (Json::Int64)st.campaign_delay_ms;
  out["campaign_retry_ms"] = (Json::Int64)st.campaign_retry_ms;
  out["campaign_retry_max_ms"] = (Json::Int64)st.campaign_retry_max_ms;
  out["campaign_retry_backoff_factor"] = (Json::Int64)st.campaign_retry_backoff_factor;
  out["leader_heartbeat_ms"] = (Json::Int64)st.leader_heartbeat_ms;
  out["leader_lease_ms"] = (Json::Int64)st.leader_lease_ms;
  out["lease_expiry_recampaign_delay_ms"] = (Json::Int64)st.lease_expiry_recampaign_delay_ms;
  out["stale_runtime_recovery_grace_ms"] = (Json::Int64)st.stale_runtime_recovery_grace_ms;
  out["poll_interval_ms"] = (Json::Int64)st.poll_interval_ms;
  out["deadline_ms"] = (Json::Int64)st.deadline_ms;
  out["cluster_size"] = Json::UInt64(st.cluster_size);
  out["outbox_limit"] = Json::UInt64(st.outbox_limit);
  out["trust_epochs"] = edge_consensus_trust_epochs_to_json(
    st.trust_roots_epoch, st.revocations_epoch, st.cert_roots_epoch);
  out["membership_epoch"] = Json::UInt64(st.membership_epoch);
  out["running"] = st.running;
  if (st.running) out["pid"] = (Json::Int64)st.pid;
  else out["exit_code"] = st.exit_code;
  if (st.exit_signal != 0) out["exit_signal"] = st.exit_signal;
  if (!st.last_error.empty()) out["last_error"] = st.last_error;
  if (!st.last_stdout_line.empty()) out["last_stdout_line"] = st.last_stdout_line;
  if (!st.last_stdout_json.isNull()) {
    out["last_stdout"] = st.last_stdout_json;
    out["result"] = st.last_stdout_json;
  }
  if (st.running && st.live_status_json.isObject()) out["live_status"] = st.live_status_json;
  return out;
}

Json::Value edge_consensus_runtime_cluster_policy_drift_json(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
) {
  const auto pol_it = cfg.edge_consensus_clusters.find(st.cluster_id);
  if (pol_it == cfg.edge_consensus_clusters.end()) return Json::Value(Json::nullValue);

  const EdgeConsensusClusterPolicy& pol = pol_it->second;
  std::vector<std::string> policy_member_node_ids =
    dedupe_safe_edge_ids_model(pol.member_node_ids);
  std::vector<std::string> runtime_member_node_ids =
    dedupe_safe_edge_ids_model(st.member_node_ids);
  std::sort(policy_member_node_ids.begin(), policy_member_node_ids.end());
  std::sort(runtime_member_node_ids.begin(), runtime_member_node_ids.end());

  Json::Value changed_fields(Json::arrayValue);
  auto append_changed = [&changed_fields](const char* field) { changed_fields.append(field); };

  if ((uint64_t)std::max<int64_t>(0, pol.membership_epoch) != st.membership_epoch) {
    append_changed("membership_epoch");
  }
  if (policy_member_node_ids != runtime_member_node_ids) append_changed("member_node_ids");
  if (pol.campaign_delay_ms != st.campaign_delay_ms) append_changed("campaign_delay_ms");
  if (pol.campaign_retry_ms != st.campaign_retry_ms) append_changed("campaign_retry_ms");
  if (pol.campaign_retry_max_ms != st.campaign_retry_max_ms) append_changed("campaign_retry_max_ms");
  if (pol.campaign_retry_backoff_factor != st.campaign_retry_backoff_factor) {
    append_changed("campaign_retry_backoff_factor");
  }
  if (pol.leader_heartbeat_ms != st.leader_heartbeat_ms) append_changed("leader_heartbeat_ms");
  if (pol.leader_lease_ms != st.leader_lease_ms) append_changed("leader_lease_ms");
  if (pol.lease_expiry_recampaign_delay_ms != st.lease_expiry_recampaign_delay_ms) {
    append_changed("lease_expiry_recampaign_delay_ms");
  }
  if (pol.stale_runtime_recovery_grace_ms != st.stale_runtime_recovery_grace_ms) {
    append_changed("stale_runtime_recovery_grace_ms");
  }

  if (changed_fields.empty()) return Json::Value(Json::nullValue);

  Json::Value out(Json::objectValue);
  out["cluster_id"] = st.cluster_id;
  out["changed_fields"] = changed_fields;
  out["current_policy"] = edge_consensus_cluster_policy_to_json(pol_it->first, pol);
  return out;
}

Json::Value edge_consensus_runtime_trust_epoch_drift_json(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
) {
  const uint64_t current_trust_roots_epoch =
    safe_config_epoch_u64_model(cfg.edge_auth_trust_roots_epoch);
  const uint64_t current_revocations_epoch =
    safe_config_epoch_u64_model(cfg.edge_auth_revocations_epoch);
  const uint64_t current_cert_roots_epoch =
    safe_config_epoch_u64_model(cfg.edge_auth_cert_roots_epoch);

  Json::Value changed_fields(Json::arrayValue);
  auto append_changed = [&changed_fields](const char* field) { changed_fields.append(field); };
  if (current_trust_roots_epoch != st.trust_roots_epoch) append_changed("trust_roots_epoch");
  if (current_revocations_epoch != st.revocations_epoch) append_changed("revocations_epoch");
  if (current_cert_roots_epoch != st.cert_roots_epoch) append_changed("cert_roots_epoch");

  if (changed_fields.empty()) return Json::Value(Json::nullValue);

  Json::Value out(Json::objectValue);
  out["changed_fields"] = changed_fields;
  out["current_trust_epochs"] = edge_consensus_trust_epochs_to_json(
    current_trust_roots_epoch, current_revocations_epoch, current_cert_roots_epoch);
  return out;
}

Json::Value edge_consensus_runtime_response_json(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
) {
  Json::Value out = edge_consensus_runtime_to_json(st);
  const Json::Value drift = edge_consensus_runtime_cluster_policy_drift_json(cfg, st);
  if (!drift.isNull()) out["cluster_policy_drift"] = drift;
  const Json::Value trust_drift = edge_consensus_runtime_trust_epoch_drift_json(cfg, st);
  if (!trust_drift.isNull()) out["trust_epoch_drift"] = trust_drift;
  return out;
}

int64_t edge_consensus_runtime_effective_stale_recovery_grace_ms(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
) {
  const auto pol_it = cfg.edge_consensus_clusters.find(st.cluster_id);
  const int64_t grace_ms = pol_it == cfg.edge_consensus_clusters.end()
    ? st.stale_runtime_recovery_grace_ms
    : pol_it->second.stale_runtime_recovery_grace_ms;
  return std::max<int64_t>(0, std::min<int64_t>(grace_ms, 86400000));
}

bool edge_consensus_runtime_stale_record_within_recovery_grace(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st,
  int64_t now_unix_ms,
  int64_t* out_age_ms
) {
  if (out_age_ms) *out_age_ms = 0;
  const int64_t grace_ms = edge_consensus_runtime_effective_stale_recovery_grace_ms(cfg, st);
  if (grace_ms <= 0 || st.started_unix_ms <= 0 || now_unix_ms <= 0) return false;
  const int64_t age_ms = std::max<int64_t>(0, now_unix_ms - st.started_unix_ms);
  if (out_age_ms) *out_age_ms = age_ms;
  return age_ms <= grace_ms;
}

bool edge_consensus_runtime_same_effective_config(
  const EdgeConsensusRuntime& a,
  const EdgeConsensusRuntime& b
) {
  const bool builtin =
    trim_copy(a.runtime_kind) == "builtin" && trim_copy(b.runtime_kind) == "builtin";
  return
    trim_copy(a.runtime_kind) == trim_copy(b.runtime_kind) &&
    trim_copy(a.node_id) == trim_copy(b.node_id) &&
    trim_copy(a.cluster_id) == trim_copy(b.cluster_id) &&
    trim_copy(a.manifest_sha256) == trim_copy(b.manifest_sha256) &&
    trim_copy(a.decision_sha256) == trim_copy(b.decision_sha256) &&
    a.peer_node_ids == b.peer_node_ids &&
    a.member_node_ids == b.member_node_ids &&
    (builtin || trim_copy(a.daemon_url) == trim_copy(b.daemon_url)) &&
    trim_copy(a.tool_path) == trim_copy(b.tool_path) &&
    trim_copy(a.model) == trim_copy(b.model) &&
    trim_copy(a.fw_git_sha) == trim_copy(b.fw_git_sha) &&
    a.campaign_delay_ms == b.campaign_delay_ms &&
    a.campaign_retry_ms == b.campaign_retry_ms &&
    a.campaign_retry_max_ms == b.campaign_retry_max_ms &&
    a.campaign_retry_backoff_factor == b.campaign_retry_backoff_factor &&
    a.leader_heartbeat_ms == b.leader_heartbeat_ms &&
    a.leader_lease_ms == b.leader_lease_ms &&
    a.lease_expiry_recampaign_delay_ms == b.lease_expiry_recampaign_delay_ms &&
    a.stale_runtime_recovery_grace_ms == b.stale_runtime_recovery_grace_ms &&
    a.poll_interval_ms == b.poll_interval_ms &&
    a.deadline_ms == b.deadline_ms &&
    a.cluster_size == b.cluster_size &&
    a.outbox_limit == b.outbox_limit &&
    a.trust_roots_epoch == b.trust_roots_epoch &&
    a.revocations_epoch == b.revocations_epoch &&
    a.cert_roots_epoch == b.cert_roots_epoch &&
    a.membership_epoch == b.membership_epoch;
}

bool edge_consensus_runtime_build_config(
  const DaemonConfig& cfg,
  const Json::Value& body,
  EdgeConsensusRuntimeConfig* out_cfg,
  EdgeConsensusRuntime* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_cfg || !out_state) {
    if (out_err) *out_err = "internal error";
    return false;
  }

  const std::string node_id =
    body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
  const std::string cluster_id =
    body.isMember("cluster_id") && body["cluster_id"].isString() ? trim_copy(body["cluster_id"].asString()) : "";
  const std::string manifest_sha256 =
    body.isMember("manifest_sha256") && body["manifest_sha256"].isString()
      ? trim_copy(body["manifest_sha256"].asString())
      : "";
  const std::string decision_sha256 =
    body.isMember("decision_sha256") && body["decision_sha256"].isString()
      ? trim_copy(body["decision_sha256"].asString())
      : "";
  const std::string daemon_url =
    body.isMember("daemon_url") && body["daemon_url"].isString()
      ? trim_copy(body["daemon_url"].asString())
      : default_local_daemon_url_model(cfg);
  const std::string auth_token =
    body.isMember("auth_token") && body["auth_token"].isString()
      ? trim_copy(body["auth_token"].asString())
      : cfg.auth_token;
  const std::string model =
    body.isMember("model") && body["model"].isString()
      ? trim_copy(body["model"].asString())
      : std::string("edge_consensus_node");
  const std::string fw_git_sha =
    body.isMember("fw_git_sha") && body["fw_git_sha"].isString()
      ? trim_copy(body["fw_git_sha"].asString())
      : std::string("agentd_managed_runtime");
  const auto pol_it = cfg.edge_consensus_clusters.find(cluster_id);
  const EdgeConsensusClusterPolicy* cluster_policy =
    pol_it == cfg.edge_consensus_clusters.end() ? nullptr : &pol_it->second;

  if (!edge_id_is_safe(node_id) || !edge_id_is_safe(cluster_id) ||
      !edge_sha256_token_is_safe(manifest_sha256)) {
    if (out_err) *out_err = "invalid node runtime identity";
    return false;
  }
  if (!decision_sha256.empty() && !edge_sha256_token_is_safe(decision_sha256)) {
    if (out_err) *out_err = "invalid decision_sha256";
    return false;
  }
  if (!is_safe_printable_field_model(daemon_url, 2048)) {
    if (out_err) *out_err = "invalid daemon_url";
    return false;
  }
  if (!auth_token.empty() && !is_safe_printable_field_model(auth_token, 2048)) {
    if (out_err) *out_err = "invalid auth_token";
    return false;
  }
  if (!is_safe_printable_field_model(model, 256) ||
      !is_safe_printable_field_model(fw_git_sha, 256)) {
    if (out_err) *out_err = "invalid model or fw_git_sha";
    return false;
  }

  std::vector<std::string> peer_node_ids;
  std::vector<std::string> member_node_ids;
  if (body.isMember("peer_node_ids")) {
    if (!body["peer_node_ids"].isArray()) {
      if (out_err) *out_err = "peer_node_ids must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < body["peer_node_ids"].size(); i++) {
      if (!body["peer_node_ids"][i].isString()) {
        if (out_err) *out_err = "peer_node_ids must contain only strings";
        return false;
      }
      const std::string peer = trim_copy(body["peer_node_ids"][i].asString());
      if (!edge_id_is_safe(peer)) {
        if (out_err) *out_err = "invalid peer_node_id";
        return false;
      }
      if (peer == node_id) continue;
      if (std::find(peer_node_ids.begin(), peer_node_ids.end(), peer) == peer_node_ids.end()) {
        peer_node_ids.push_back(peer);
      }
    }
  }
  if (body.isMember("member_node_ids")) {
    if (!body["member_node_ids"].isArray()) {
      if (out_err) *out_err = "member_node_ids must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < body["member_node_ids"].size(); i++) {
      if (!body["member_node_ids"][i].isString()) {
        if (out_err) *out_err = "member_node_ids must contain only strings";
        return false;
      }
      const std::string member = trim_copy(body["member_node_ids"][i].asString());
      if (!edge_id_is_safe(member)) {
        if (out_err) *out_err = "invalid member_node_id";
        return false;
      }
      if (std::find(member_node_ids.begin(), member_node_ids.end(), member) == member_node_ids.end()) {
        member_node_ids.push_back(member);
      }
    }
  }
  if (member_node_ids.empty() && cluster_policy) {
    member_node_ids = dedupe_safe_edge_ids_model(cluster_policy->member_node_ids);
  }
  if (std::find(member_node_ids.begin(), member_node_ids.end(), node_id) == member_node_ids.end()) {
    member_node_ids.push_back(node_id);
  }
  member_node_ids = dedupe_safe_edge_ids_model(member_node_ids);
  if (peer_node_ids.empty() && !member_node_ids.empty()) {
    for (const auto& member : member_node_ids) {
      if (member != node_id) peer_node_ids.push_back(member);
    }
  }
  peer_node_ids = dedupe_safe_edge_ids_model(peer_node_ids);
  if (member_node_ids.empty()) member_node_ids = peer_node_ids;

  uint64_t cluster_size = body.isMember("cluster_size") ? json_to_u64_model(body["cluster_size"], 0) : 0;
  if (cluster_size == 0) {
    cluster_size = !member_node_ids.empty()
      ? (uint64_t)member_node_ids.size()
      : (uint64_t)peer_node_ids.size() + 1;
  }
  cluster_size = std::max<uint64_t>(1, std::min<uint64_t>(cluster_size, 128));

  uint64_t outbox_limit = body.isMember("outbox_limit")
    ? json_to_u64_model(body["outbox_limit"], 128)
    : 128;
  outbox_limit = std::max<uint64_t>(1, std::min<uint64_t>(outbox_limit, 2048));

  EdgeConsensusClusterPolicy timing_pol;
  timing_pol.campaign_delay_ms = body.isMember("campaign_delay_ms")
    ? json_to_i64_model(body["campaign_delay_ms"], 0)
    : (cluster_policy ? cluster_policy->campaign_delay_ms : 0);
  timing_pol.campaign_retry_ms = body.isMember("campaign_retry_ms")
    ? json_to_i64_model(body["campaign_retry_ms"], decision_sha256.empty() ? 0 : 1500)
    : (cluster_policy ? cluster_policy->campaign_retry_ms : (decision_sha256.empty() ? 0 : 1500));
  timing_pol.campaign_retry_max_ms = body.isMember("campaign_retry_max_ms")
    ? json_to_i64_model(body["campaign_retry_max_ms"], timing_pol.campaign_retry_ms)
    : (cluster_policy ? cluster_policy->campaign_retry_max_ms : timing_pol.campaign_retry_ms);
  timing_pol.campaign_retry_backoff_factor = body.isMember("campaign_retry_backoff_factor")
    ? json_to_i64_model(body["campaign_retry_backoff_factor"], 1)
    : (cluster_policy ? cluster_policy->campaign_retry_backoff_factor : 1);
  timing_pol.leader_heartbeat_ms = body.isMember("leader_heartbeat_ms")
    ? json_to_i64_model(body["leader_heartbeat_ms"], 1000)
    : (cluster_policy ? cluster_policy->leader_heartbeat_ms : 1000);
  timing_pol.leader_lease_ms = body.isMember("leader_lease_ms")
    ? json_to_i64_model(body["leader_lease_ms"], 5000)
    : (cluster_policy ? cluster_policy->leader_lease_ms : 5000);
  timing_pol.lease_expiry_recampaign_delay_ms = body.isMember("lease_expiry_recampaign_delay_ms")
    ? json_to_i64_model(body["lease_expiry_recampaign_delay_ms"], 0)
    : (cluster_policy ? cluster_policy->lease_expiry_recampaign_delay_ms : 0);
  timing_pol.stale_runtime_recovery_grace_ms = body.isMember("stale_runtime_recovery_grace_ms")
    ? json_to_i64_model(body["stale_runtime_recovery_grace_ms"], 0)
    : (cluster_policy ? cluster_policy->stale_runtime_recovery_grace_ms : 0);
  edge_consensus_normalize_policy_timing(&timing_pol);
  const int64_t campaign_delay_ms = timing_pol.campaign_delay_ms;
  const int64_t campaign_retry_ms = timing_pol.campaign_retry_ms;
  const int64_t campaign_retry_max_ms = timing_pol.campaign_retry_max_ms;
  const int64_t campaign_retry_backoff_factor = timing_pol.campaign_retry_backoff_factor;
  const int64_t leader_heartbeat_ms = timing_pol.leader_heartbeat_ms;
  const int64_t leader_lease_ms = timing_pol.leader_lease_ms;
  const int64_t lease_expiry_recampaign_delay_ms = timing_pol.lease_expiry_recampaign_delay_ms;
  const int64_t stale_runtime_recovery_grace_ms = timing_pol.stale_runtime_recovery_grace_ms;
  int64_t poll_interval_ms = body.isMember("poll_interval_ms")
    ? json_to_i64_model(body["poll_interval_ms"], 100)
    : 100;
  poll_interval_ms = std::max<int64_t>(25, std::min<int64_t>(poll_interval_ms, 5000));
  int64_t deadline_ms = body.isMember("deadline_ms")
    ? json_to_i64_model(body["deadline_ms"], 10000)
    : 10000;
  deadline_ms = std::max<int64_t>(1000, std::min<int64_t>(deadline_ms, 300000));

  const uint64_t default_trust_roots_epoch =
    safe_config_epoch_u64_model(cfg.edge_auth_trust_roots_epoch);
  const uint64_t default_revocations_epoch =
    safe_config_epoch_u64_model(cfg.edge_auth_revocations_epoch);
  const uint64_t default_cert_roots_epoch =
    safe_config_epoch_u64_model(cfg.edge_auth_cert_roots_epoch);
  uint64_t trust_roots_epoch = body.isMember("trust_roots_epoch")
    ? json_to_u64_model(body["trust_roots_epoch"], default_trust_roots_epoch)
    : default_trust_roots_epoch;
  uint64_t revocations_epoch = body.isMember("revocations_epoch")
    ? json_to_u64_model(body["revocations_epoch"], default_revocations_epoch)
    : default_revocations_epoch;
  uint64_t cert_roots_epoch = body.isMember("cert_roots_epoch")
    ? json_to_u64_model(body["cert_roots_epoch"], default_cert_roots_epoch)
    : default_cert_roots_epoch;
  uint64_t membership_epoch = body.isMember("membership_epoch")
    ? json_to_u64_model(body["membership_epoch"], 0)
    : (cluster_policy && cluster_policy->membership_epoch >= 0
         ? (uint64_t)cluster_policy->membership_epoch
         : 0);

  out_cfg->daemon_url = daemon_url;
  out_cfg->auth_token = auth_token;
  out_cfg->node_id = node_id;
  out_cfg->cluster_id = cluster_id;
  out_cfg->manifest_sha256 = manifest_sha256;
  out_cfg->model = model;
  out_cfg->fw_git_sha = fw_git_sha;
  out_cfg->decision_sha256 = decision_sha256;
  out_cfg->peer_node_ids = peer_node_ids;
  out_cfg->member_node_ids = member_node_ids;
  out_cfg->cluster_size = (size_t)cluster_size;
  out_cfg->outbox_limit = (size_t)outbox_limit;
  out_cfg->campaign_delay_ms = campaign_delay_ms;
  out_cfg->campaign_retry_ms = campaign_retry_ms;
  out_cfg->campaign_retry_max_ms = campaign_retry_max_ms;
  out_cfg->campaign_retry_backoff_factor = campaign_retry_backoff_factor;
  out_cfg->leader_heartbeat_ms = leader_heartbeat_ms;
  out_cfg->leader_lease_ms = leader_lease_ms;
  out_cfg->lease_expiry_recampaign_delay_ms = lease_expiry_recampaign_delay_ms;
  out_cfg->stale_runtime_recovery_grace_ms = stale_runtime_recovery_grace_ms;
  out_cfg->poll_interval_ms = poll_interval_ms;
  out_cfg->deadline_ms = deadline_ms;
  out_cfg->trust_roots_epoch = trust_roots_epoch;
  out_cfg->revocations_epoch = revocations_epoch;
  out_cfg->cert_roots_epoch = cert_roots_epoch;
  out_cfg->membership_epoch = membership_epoch;

  *out_state = EdgeConsensusRuntime{};
  out_state->node_id = node_id;
  out_state->cluster_id = cluster_id;
  out_state->manifest_sha256 = manifest_sha256;
  out_state->decision_sha256 = decision_sha256;
  out_state->peer_node_ids = peer_node_ids;
  out_state->member_node_ids = member_node_ids;
  out_state->daemon_url = daemon_url;
  out_state->model = model;
  out_state->fw_git_sha = fw_git_sha;
  out_state->started_unix_ms = now_unix_ms_model();
  out_state->campaign_delay_ms = campaign_delay_ms;
  out_state->campaign_retry_ms = campaign_retry_ms;
  out_state->campaign_retry_max_ms = campaign_retry_max_ms;
  out_state->campaign_retry_backoff_factor = campaign_retry_backoff_factor;
  out_state->leader_heartbeat_ms = leader_heartbeat_ms;
  out_state->leader_lease_ms = leader_lease_ms;
  out_state->lease_expiry_recampaign_delay_ms = lease_expiry_recampaign_delay_ms;
  out_state->stale_runtime_recovery_grace_ms = stale_runtime_recovery_grace_ms;
  out_state->poll_interval_ms = poll_interval_ms;
  out_state->deadline_ms = deadline_ms;
  out_state->cluster_size = cluster_size;
  out_state->outbox_limit = outbox_limit;
  out_state->trust_roots_epoch = trust_roots_epoch;
  out_state->revocations_epoch = revocations_epoch;
  out_state->cert_roots_epoch = cert_roots_epoch;
  out_state->membership_epoch = membership_epoch;
  out_state->running = true;
  return true;
}

std::string edge_consensus_default_local_daemon_url(const DaemonConfig& cfg) {
  return default_local_daemon_url_model(cfg);
}

}  // namespace agentd
