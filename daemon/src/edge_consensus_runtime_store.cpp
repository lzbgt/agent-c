#include "edge_consensus_runtime_store.h"

#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace agentd {
namespace {

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static uint64_t json_to_u64(const Json::Value& v, uint64_t fallback) {
  if (v.isUInt64()) return v.asUInt64();
  if (v.isInt64() && v.asInt64() >= 0) return (uint64_t)v.asInt64();
  if (v.isUInt()) return (uint64_t)v.asUInt();
  if (v.isInt() && v.asInt() >= 0) return (uint64_t)v.asInt();
  return fallback;
}

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

static std::string edge_consensus_runtime_meta_key(const std::string& node_id) {
  return "edge.consensus_runtime." + node_id;
}

static std::filesystem::path edge_consensus_runtime_dir(
  const DaemonConfig& cfg,
  const std::string& node_id
) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");
  return base / "edge_consensus_runtimes" / node_id;
}

}  // namespace

bool remove_edge_consensus_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& node_id,
  bool* out_deleted,
  std::string* out_err
) {
  if (out_deleted) *out_deleted = false;
  if (out_err) out_err->clear();
  std::error_code ec;
  const std::filesystem::path run_dir = edge_consensus_runtime_dir(cfg, node_id);
  if (!std::filesystem::exists(run_dir, ec)) return true;
  const auto removed = std::filesystem::remove_all(run_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to remove edge consensus runtime artifacts";
    return false;
  }
  if (out_deleted) *out_deleted = removed > 0;
  return true;
}

bool edge_consensus_runtime_from_json(
  const Json::Value& v,
  EdgeConsensusRuntime* out,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out) {
    if (out_err) *out_err = "runtime output missing";
    return false;
  }
  if (!v.isObject()) {
    if (out_err) *out_err = "runtime record must be an object";
    return false;
  }

  EdgeConsensusRuntime st;
  if (v.isMember("runtime_kind") && v["runtime_kind"].isString()) {
    st.runtime_kind = trim_copy(v["runtime_kind"].asString());
  }
  if (st.runtime_kind != "builtin" && st.runtime_kind != "external") {
    if (out_err) *out_err = "runtime_kind must be builtin or external";
    return false;
  }
  if (v.isMember("status_source") && v["status_source"].isString()) {
    st.status_source = trim_copy(v["status_source"].asString());
  }
  if (v.isMember("node_id") && v["node_id"].isString()) {
    st.node_id = trim_copy(v["node_id"].asString());
  }
  if (v.isMember("cluster_id") && v["cluster_id"].isString()) {
    st.cluster_id = trim_copy(v["cluster_id"].asString());
  }
  if (v.isMember("manifest_sha256") && v["manifest_sha256"].isString()) {
    st.manifest_sha256 = trim_copy(v["manifest_sha256"].asString());
  }
  if (v.isMember("decision_sha256") && v["decision_sha256"].isString()) {
    st.decision_sha256 = trim_copy(v["decision_sha256"].asString());
  }
  if (!edge_id_is_safe(st.node_id) || !edge_id_is_safe(st.cluster_id) ||
      !edge_sha256_token_is_safe(st.manifest_sha256)) {
    if (out_err) *out_err = "invalid persisted runtime identity";
    return false;
  }
  if (!st.decision_sha256.empty() && !edge_sha256_token_is_safe(st.decision_sha256)) {
    if (out_err) *out_err = "invalid persisted decision_sha256";
    return false;
  }

  if (v.isMember("peer_node_ids")) {
    if (!v["peer_node_ids"].isArray()) {
      if (out_err) *out_err = "persisted peer_node_ids must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < v["peer_node_ids"].size(); i++) {
      if (!v["peer_node_ids"][i].isString()) {
        if (out_err) *out_err = "persisted peer_node_ids must contain strings";
        return false;
      }
      st.peer_node_ids.push_back(trim_copy(v["peer_node_ids"][i].asString()));
    }
    st.peer_node_ids = dedupe_safe_edge_ids(st.peer_node_ids);
  }

  if (v.isMember("member_node_ids")) {
    if (!v["member_node_ids"].isArray()) {
      if (out_err) *out_err = "persisted member_node_ids must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < v["member_node_ids"].size(); i++) {
      if (!v["member_node_ids"][i].isString()) {
        if (out_err) *out_err = "persisted member_node_ids must contain strings";
        return false;
      }
      st.member_node_ids.push_back(trim_copy(v["member_node_ids"][i].asString()));
    }
    st.member_node_ids = dedupe_safe_edge_ids(st.member_node_ids);
  }

  if (v.isMember("daemon_url") && v["daemon_url"].isString()) {
    st.daemon_url = trim_copy(v["daemon_url"].asString());
  }
  if (v.isMember("tool_path") && v["tool_path"].isString()) {
    st.tool_path = trim_copy(v["tool_path"].asString());
  }
  if (v.isMember("model") && v["model"].isString()) {
    st.model = trim_copy(v["model"].asString());
  }
  if (v.isMember("fw_git_sha") && v["fw_git_sha"].isString()) {
    st.fw_git_sha = trim_copy(v["fw_git_sha"].asString());
  }
  if (v.isMember("stderr_log_path") && v["stderr_log_path"].isString()) {
    st.stderr_log_path = trim_copy(v["stderr_log_path"].asString());
  }
  if (v.isMember("started_unix_ms") && (v["started_unix_ms"].isInt64() || v["started_unix_ms"].isUInt64())) {
    st.started_unix_ms = v["started_unix_ms"].asInt64();
  }
  if (v.isMember("ended_unix_ms") && (v["ended_unix_ms"].isInt64() || v["ended_unix_ms"].isUInt64())) {
    st.ended_unix_ms = v["ended_unix_ms"].asInt64();
  }
  if (v.isMember("campaign_delay_ms") &&
      (v["campaign_delay_ms"].isInt64() || v["campaign_delay_ms"].isUInt64())) {
    st.campaign_delay_ms = v["campaign_delay_ms"].asInt64();
  }
  if (v.isMember("campaign_retry_ms") &&
      (v["campaign_retry_ms"].isInt64() || v["campaign_retry_ms"].isUInt64())) {
    st.campaign_retry_ms = v["campaign_retry_ms"].asInt64();
  }
  if (v.isMember("campaign_retry_max_ms") &&
      (v["campaign_retry_max_ms"].isInt64() || v["campaign_retry_max_ms"].isUInt64())) {
    st.campaign_retry_max_ms = v["campaign_retry_max_ms"].asInt64();
  }
  if (v.isMember("campaign_retry_backoff_factor") &&
      (v["campaign_retry_backoff_factor"].isInt64() ||
       v["campaign_retry_backoff_factor"].isUInt64())) {
    st.campaign_retry_backoff_factor = v["campaign_retry_backoff_factor"].asInt64();
  }
  if (v.isMember("leader_heartbeat_ms") &&
      (v["leader_heartbeat_ms"].isInt64() || v["leader_heartbeat_ms"].isUInt64())) {
    st.leader_heartbeat_ms = v["leader_heartbeat_ms"].asInt64();
  }
  if (v.isMember("leader_lease_ms") &&
      (v["leader_lease_ms"].isInt64() || v["leader_lease_ms"].isUInt64())) {
    st.leader_lease_ms = v["leader_lease_ms"].asInt64();
  }
  if (v.isMember("lease_expiry_recampaign_delay_ms") &&
      (v["lease_expiry_recampaign_delay_ms"].isInt64() ||
       v["lease_expiry_recampaign_delay_ms"].isUInt64())) {
    st.lease_expiry_recampaign_delay_ms = v["lease_expiry_recampaign_delay_ms"].asInt64();
  }
  if (v.isMember("stale_runtime_recovery_grace_ms") &&
      (v["stale_runtime_recovery_grace_ms"].isInt64() ||
       v["stale_runtime_recovery_grace_ms"].isUInt64())) {
    st.stale_runtime_recovery_grace_ms = v["stale_runtime_recovery_grace_ms"].asInt64();
  }
  if (v.isMember("poll_interval_ms") &&
      (v["poll_interval_ms"].isInt64() || v["poll_interval_ms"].isUInt64())) {
    st.poll_interval_ms = v["poll_interval_ms"].asInt64();
  }
  if (v.isMember("deadline_ms") && (v["deadline_ms"].isInt64() || v["deadline_ms"].isUInt64())) {
    st.deadline_ms = v["deadline_ms"].asInt64();
  }
  if (v.isMember("cluster_size")) st.cluster_size = json_to_u64(v["cluster_size"], st.cluster_size);
  if (v.isMember("outbox_limit")) st.outbox_limit = json_to_u64(v["outbox_limit"], st.outbox_limit);
  if (v.isMember("trust_epochs") && v["trust_epochs"].isObject()) {
    const auto& epochs = v["trust_epochs"];
    if (epochs.isMember("trust_roots_epoch")) {
      st.trust_roots_epoch = json_to_u64(epochs["trust_roots_epoch"], st.trust_roots_epoch);
    }
    if (epochs.isMember("revocations_epoch")) {
      st.revocations_epoch = json_to_u64(epochs["revocations_epoch"], st.revocations_epoch);
    }
    if (epochs.isMember("cert_roots_epoch")) {
      st.cert_roots_epoch = json_to_u64(epochs["cert_roots_epoch"], st.cert_roots_epoch);
    }
  }
  if (v.isMember("membership_epoch")) {
    st.membership_epoch = json_to_u64(v["membership_epoch"], st.membership_epoch);
  }
  if (v.isMember("running") && v["running"].isBool()) st.running = v["running"].asBool();
  if (v.isMember("exit_code") && v["exit_code"].isInt()) st.exit_code = v["exit_code"].asInt();
  if (v.isMember("exit_signal") && v["exit_signal"].isInt()) st.exit_signal = v["exit_signal"].asInt();
  if (v.isMember("last_error") && v["last_error"].isString()) st.last_error = v["last_error"].asString();
  if (v.isMember("last_stdout_line") && v["last_stdout_line"].isString()) {
    st.last_stdout_line = v["last_stdout_line"].asString();
  }
  if (v.isMember("last_stdout") && v["last_stdout"].isObject()) {
    st.last_stdout_json = v["last_stdout"];
  }
  if (v.isMember("pid") && (v["pid"].isInt64() || v["pid"].isUInt64())) {
    st.pid = (decltype(st.pid))v["pid"].asInt64();
  }
  *out = std::move(st);
  return true;
}

bool persist_edge_consensus_runtime_record(
  AgentDb* db,
  const EdgeConsensusRuntime& st,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  Json::Value record = edge_consensus_runtime_to_json(st);
  record["persisted_utc_ms"] = (Json::Int64)now_unix_ms();
  return db->meta_set(edge_consensus_runtime_meta_key(st.node_id), json_stringify(record), out_err);
}

bool clear_edge_consensus_runtime_record(
  AgentDb* db,
  const std::string& node_id,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  return db->meta_set(edge_consensus_runtime_meta_key(node_id), "", out_err);
}

bool load_edge_consensus_runtime_record(
  AgentDb* db,
  const std::string& node_id,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  bool* out_self_healed,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (out_self_healed) *out_self_healed = false;
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }

  std::string raw;
  if (!db->meta_get(edge_consensus_runtime_meta_key(node_id), &raw, out_err)) return false;
  if (trim_copy(raw).empty()) return true;

  Json::Value parsed(Json::nullValue);
  std::string jerr;
  if (!json_parse_any(raw, &parsed, &jerr) || !parsed.isObject()) {
    std::string cerr;
    if (!clear_edge_consensus_runtime_record(db, node_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? (jerr.empty() ? "persisted edge consensus runtime record corrupt" : jerr)
          : ("failed to clear corrupt persisted edge consensus runtime record: " + cerr);
      }
      return false;
    }
    if (out_self_healed) *out_self_healed = true;
    return true;
  }

  auto st = std::make_shared<EdgeConsensusRuntime>();
  if (!edge_consensus_runtime_from_json(parsed, st.get(), out_err)) {
    const std::string original_err = out_err
      ? *out_err
      : std::string("persisted edge consensus runtime record corrupt");
    std::string cerr;
    if (!clear_edge_consensus_runtime_record(db, node_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty()
          ? original_err
          : ("failed to clear corrupt persisted edge consensus runtime record: " + cerr);
      }
      return false;
    }
    if (out_self_healed) *out_self_healed = true;
    if (out_state) out_state->reset();
    if (out_err) out_err->clear();
    return true;
  }

  if (st->status_source != "persisted_recovered") st->status_source = "persisted";
  if (out_state) *out_state = std::move(st);
  return true;
}

bool recover_or_clear_edge_consensus_stale_builtin_record(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  int64_t now_ms,
  bool* out_recovered,
  Json::Value* out_cleanup,
  std::string* out_err
) {
  if (out_recovered) *out_recovered = false;
  if (out_cleanup) *out_cleanup = Json::Value(Json::objectValue);
  if (out_err) out_err->clear();
  if (!st || !st->running) return true;

  int64_t stale_age_ms = 0;
  const int64_t recovery_grace_ms =
    edge_consensus_runtime_effective_stale_recovery_grace_ms(cfg, *st);
  const bool recover_stale =
    edge_consensus_runtime_stale_record_within_recovery_grace(cfg, *st, now_ms, &stale_age_ms);

  Json::Value cleanup(Json::objectValue);
  cleanup["stale_runtime_recovery_grace_ms"] = (Json::Int64)recovery_grace_ms;
  cleanup["stale_runtime_age_ms"] = (Json::Int64)stale_age_ms;
  bool artifacts_deleted = false;
  std::string aerr;
  if (remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr)) {
    cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
  } else if (!aerr.empty()) {
    cleanup["runtime_artifacts_delete_error"] = aerr;
  }
  if (recover_stale) {
    st->running = false;
    st->ended_unix_ms = now_ms;
    st->status_source = "persisted_recovered";
    st->last_error = "stale_builtin_runtime_recovered_after_restart";
    cleanup["persisted_record_recovered"] = persist_edge_consensus_runtime_record(db, *st, nullptr);
    cleanup["persisted_record_cleared"] = false;
    if (out_recovered) *out_recovered = true;
    if (out_cleanup) *out_cleanup = cleanup;
    return true;
  }
  cleanup["persisted_record_cleared"] = clear_edge_consensus_runtime_record(db, node_id, nullptr);
  if (out_cleanup) *out_cleanup = cleanup;
  return true;
}

bool recover_edge_consensus_runtime_record(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  Json::Value* out_updates,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_state) out_state->reset();
  if (out_updates) *out_updates = Json::Value(Json::objectValue);

  bool record_self_healed = false;
  std::shared_ptr<EdgeConsensusRuntime> st;
  if (!load_edge_consensus_runtime_record(db, node_id, &st, &record_self_healed, out_err)) {
    return false;
  }

  if (record_self_healed) {
    Json::Value cleanup(Json::objectValue);
    cleanup["persisted_record_cleared"] = true;
    bool artifacts_deleted = false;
    std::string aerr;
    if (remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr)) {
      cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
    } else if (!aerr.empty()) {
      cleanup["runtime_artifacts_delete_error"] = aerr;
    }
    if (out_updates) (*out_updates)["cleanup_on_corrupt_record"] = cleanup;
    return true;
  }

  if (st && st->status_source == "persisted" && st->running &&
      trim_copy(st->runtime_kind) != "external") {
    const int64_t now_ms = now_unix_ms();
    bool recovered = false;
    Json::Value cleanup(Json::objectValue);
    if (!recover_or_clear_edge_consensus_stale_builtin_record(
          cfg, db, node_id, st, now_ms, &recovered, &cleanup, out_err)) {
      return false;
    }
    if (recovered) {
      if (out_updates) (*out_updates)["cleanup_on_stale_record"] = cleanup;
      if (out_state) *out_state = st;
      return true;
    }
    if (out_updates) (*out_updates)["cleanup_on_stale_record"] = cleanup;
    st.reset();
  }

  if (out_state) *out_state = std::move(st);
  return true;
}

}  // namespace agentd
