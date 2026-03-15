#include "edge_runtime_endpoints.h"

#include "daemon_auth.h"
#include "edge_consensus_http_runtime.h"
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

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentd {
namespace {

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

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

static uint64_t json_to_u64(const Json::Value& v, uint64_t fallback) {
  if (v.isUInt64()) return v.asUInt64();
  if (v.isInt64() && v.asInt64() >= 0) return (uint64_t)v.asInt64();
  if (v.isUInt()) return (uint64_t)v.asUInt();
  if (v.isInt() && v.asInt() >= 0) return (uint64_t)v.asInt();
  return fallback;
}

static int64_t json_to_i64(const Json::Value& v, int64_t fallback) {
  if (v.isInt64()) return v.asInt64();
  if (v.isUInt64() && v.asUInt64() <= (uint64_t)INT64_MAX) return (int64_t)v.asUInt64();
  if (v.isInt()) return (int64_t)v.asInt();
  if (v.isUInt()) return (int64_t)v.asUInt();
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

static Json::Value edge_consensus_cluster_policy_to_json(const std::string& cluster_id, const EdgeConsensusClusterPolicy& pol) {
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
  Json::Value members(Json::arrayValue);
  for (const auto& member : pol.member_node_ids) members.append(member);
  out["member_node_ids"] = members;
  return out;
}

struct EdgeConsensusRuntime {
  std::string runtime_kind = "builtin";
  std::string node_id;
  std::string cluster_id;
  std::string manifest_sha256;
  std::string decision_sha256;
  std::vector<std::string> peer_node_ids;
  std::vector<std::string> member_node_ids;
  std::string daemon_url;
  std::string tool_path;
  std::string model;
  std::string fw_git_sha;
  std::string stderr_log_path;
  int64_t started_unix_ms = 0;
  int64_t ended_unix_ms = 0;
  int64_t campaign_delay_ms = 0;
  int64_t campaign_retry_ms = 0;
  int64_t campaign_retry_max_ms = 0;
  int64_t campaign_retry_backoff_factor = 1;
  int64_t leader_heartbeat_ms = 1000;
  int64_t leader_lease_ms = 5000;
  int64_t poll_interval_ms = 100;
  int64_t deadline_ms = 10000;
  uint64_t cluster_size = 0;
  uint64_t outbox_limit = 128;
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
  uint64_t membership_epoch = 0;
  bool running = false;
  int exit_code = 0;
  int exit_signal = 0;
  std::string last_error;
  std::string last_stdout_line;
  Json::Value last_stdout_json = Json::Value(Json::nullValue);
  std::shared_ptr<std::atomic<bool>> stop_requested;
#if defined(_WIN32)
  intptr_t pid = 0;
#else
  pid_t pid = -1;
#endif
};

static std::mutex g_edge_consensus_runtime_mu;
static std::unordered_map<std::string, std::shared_ptr<EdgeConsensusRuntime>> g_edge_consensus_runtime_by_node;

static Json::Value edge_consensus_runtime_to_json(const EdgeConsensusRuntime& st) {
  Json::Value out(Json::objectValue);
  out["schema"] = "edge_node_consensus_runtime_v1";
  out["runtime_kind"] = st.runtime_kind;
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
  out["poll_interval_ms"] = (Json::Int64)st.poll_interval_ms;
  out["deadline_ms"] = (Json::Int64)st.deadline_ms;
  out["cluster_size"] = Json::UInt64(st.cluster_size);
  out["outbox_limit"] = Json::UInt64(st.outbox_limit);
  Json::Value epochs(Json::objectValue);
  epochs["trust_roots_epoch"] = Json::UInt64(st.trust_roots_epoch);
  epochs["revocations_epoch"] = Json::UInt64(st.revocations_epoch);
  epochs["cert_roots_epoch"] = Json::UInt64(st.cert_roots_epoch);
  out["trust_epochs"] = epochs;
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
  return out;
}

static std::shared_ptr<EdgeConsensusRuntime> edge_consensus_runtime_lookup_locked(const std::string& node_id) {
  const auto it = g_edge_consensus_runtime_by_node.find(node_id);
  return it == g_edge_consensus_runtime_by_node.end() ? nullptr : it->second;
}

static std::filesystem::path edge_consensus_runtime_dir(const DaemonConfig& cfg, const std::string& node_id) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");
  return base / "edge_consensus_runtimes" / node_id;
}

static std::string default_local_daemon_url(const DaemonConfig& cfg) {
  std::string host = trim_copy(cfg.listen_host);
  if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]") host = "127.0.0.1";
  return "http://" + host + ":" + std::to_string((int)cfg.listen_port);
}

static std::string edge_consensus_external_runtime_unavailable_reason(const DaemonConfig& cfg) {
  const std::string tool_path = trim_copy(cfg.edge_consensus_node_tool_path);
  if (tool_path.empty()) return "edge_consensus_node_tool_path not configured";
  if (!std::filesystem::exists(std::filesystem::path(tool_path))) return "edge_consensus_node_tool_path not found";
  if (!is_safe_shellish_token(tool_path, 512)) return "invalid edge_consensus_node_tool_path";
#if !defined(_WIN32)
  if (::access(tool_path.c_str(), X_OK) != 0) {
    return std::string("edge_consensus_node_tool_path not executable: ") + std::strerror(errno);
  }
#endif
  return "";
}

static std::string configured_default_edge_consensus_runtime_kind(const DaemonConfig& cfg) {
  const std::string kind = lower_copy(trim_copy(cfg.edge_consensus_default_runtime_kind));
  return (kind == "builtin" || kind == "external") ? kind : std::string();
}

static std::string default_edge_consensus_runtime_kind_source(const DaemonConfig& cfg) {
  if (configured_default_edge_consensus_runtime_kind(cfg).empty()) return "auto";
  return cfg.edge_consensus_default_runtime_kind_from_env ? "env" : "config";
}

static std::string default_edge_consensus_runtime_kind(const DaemonConfig& cfg) {
  const std::string configured = configured_default_edge_consensus_runtime_kind(cfg);
  return configured.empty() ? "builtin" : configured;
}

static bool edge_consensus_runtime_build_config(
  const DaemonConfig& cfg,
  const Json::Value& body,
  EdgeConsensusHttpRuntimeConfig* out_cfg,
  EdgeConsensusRuntime* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_cfg || !out_state) {
    if (out_err) *out_err = "internal error";
    return false;
  }

  const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
  const std::string cluster_id = body.isMember("cluster_id") && body["cluster_id"].isString() ? trim_copy(body["cluster_id"].asString()) : "";
  const std::string manifest_sha256 =
    body.isMember("manifest_sha256") && body["manifest_sha256"].isString() ? trim_copy(body["manifest_sha256"].asString()) : "";
  const std::string decision_sha256 =
    body.isMember("decision_sha256") && body["decision_sha256"].isString() ? trim_copy(body["decision_sha256"].asString()) : "";
  const std::string daemon_url =
    body.isMember("daemon_url") && body["daemon_url"].isString() ? trim_copy(body["daemon_url"].asString()) : default_local_daemon_url(cfg);
  const std::string auth_token =
    body.isMember("auth_token") && body["auth_token"].isString() ? trim_copy(body["auth_token"].asString()) : cfg.auth_token;
  const std::string model =
    body.isMember("model") && body["model"].isString() ? trim_copy(body["model"].asString()) : std::string("edge_consensus_node");
  const std::string fw_git_sha =
    body.isMember("fw_git_sha") && body["fw_git_sha"].isString() ? trim_copy(body["fw_git_sha"].asString()) : std::string("agentd_managed_runtime");
  const auto pol_it = cfg.edge_consensus_clusters.find(cluster_id);
  const EdgeConsensusClusterPolicy* cluster_policy = pol_it == cfg.edge_consensus_clusters.end() ? nullptr : &pol_it->second;

  if (!edge_id_is_safe(node_id) || !edge_id_is_safe(cluster_id) || !edge_sha256_token_is_safe(manifest_sha256)) {
    if (out_err) *out_err = "invalid node runtime identity";
    return false;
  }
  if (!decision_sha256.empty() && !edge_sha256_token_is_safe(decision_sha256)) {
    if (out_err) *out_err = "invalid decision_sha256";
    return false;
  }
  if (!is_safe_printable_field(daemon_url, 2048)) {
    if (out_err) *out_err = "invalid daemon_url";
    return false;
  }
  if (!auth_token.empty() && !is_safe_printable_field(auth_token, 2048)) {
    if (out_err) *out_err = "invalid auth_token";
    return false;
  }
  if (!is_safe_printable_field(model, 256) || !is_safe_printable_field(fw_git_sha, 256)) {
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
      if (std::find(peer_node_ids.begin(), peer_node_ids.end(), peer) == peer_node_ids.end()) peer_node_ids.push_back(peer);
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
      if (std::find(member_node_ids.begin(), member_node_ids.end(), member) == member_node_ids.end()) member_node_ids.push_back(member);
    }
  }
  if (member_node_ids.empty() && cluster_policy) member_node_ids = dedupe_safe_edge_ids(cluster_policy->member_node_ids);
  if (std::find(member_node_ids.begin(), member_node_ids.end(), node_id) == member_node_ids.end()) member_node_ids.push_back(node_id);
  member_node_ids = dedupe_safe_edge_ids(member_node_ids);
  if (peer_node_ids.empty() && !member_node_ids.empty()) {
    for (const auto& member : member_node_ids) if (member != node_id) peer_node_ids.push_back(member);
  }
  peer_node_ids = dedupe_safe_edge_ids(peer_node_ids);
  if (member_node_ids.empty()) member_node_ids = peer_node_ids;

  uint64_t cluster_size = body.isMember("cluster_size") ? json_to_u64(body["cluster_size"], 0) : 0;
  if (cluster_size == 0) cluster_size = !member_node_ids.empty() ? (uint64_t)member_node_ids.size() : (uint64_t)peer_node_ids.size() + 1;
  cluster_size = std::max<uint64_t>(1, std::min<uint64_t>(cluster_size, 128));

  uint64_t outbox_limit = body.isMember("outbox_limit") ? json_to_u64(body["outbox_limit"], 128) : 128;
  outbox_limit = std::max<uint64_t>(1, std::min<uint64_t>(outbox_limit, 2048));

  int64_t campaign_delay_ms = body.isMember("campaign_delay_ms")
    ? json_to_i64(body["campaign_delay_ms"], 0)
    : (cluster_policy ? cluster_policy->campaign_delay_ms : 0);
  campaign_delay_ms = std::max<int64_t>(0, std::min<int64_t>(campaign_delay_ms, 120000));
  int64_t campaign_retry_ms = body.isMember("campaign_retry_ms")
    ? json_to_i64(body["campaign_retry_ms"], decision_sha256.empty() ? 0 : 1500)
    : (cluster_policy ? cluster_policy->campaign_retry_ms : (decision_sha256.empty() ? 0 : 1500));
  campaign_retry_ms = std::max<int64_t>(0, std::min<int64_t>(campaign_retry_ms, 120000));
  int64_t campaign_retry_max_ms = body.isMember("campaign_retry_max_ms")
    ? json_to_i64(body["campaign_retry_max_ms"], campaign_retry_ms)
    : (cluster_policy ? cluster_policy->campaign_retry_max_ms : campaign_retry_ms);
  campaign_retry_max_ms = std::max<int64_t>(campaign_retry_ms, std::min<int64_t>(campaign_retry_max_ms, 300000));
  int64_t campaign_retry_backoff_factor = body.isMember("campaign_retry_backoff_factor")
    ? json_to_i64(body["campaign_retry_backoff_factor"], 1)
    : (cluster_policy ? cluster_policy->campaign_retry_backoff_factor : 1);
  campaign_retry_backoff_factor = std::max<int64_t>(1, std::min<int64_t>(campaign_retry_backoff_factor, 8));
  int64_t leader_heartbeat_ms = body.isMember("leader_heartbeat_ms")
    ? json_to_i64(body["leader_heartbeat_ms"], 1000)
    : (cluster_policy ? cluster_policy->leader_heartbeat_ms : 1000);
  leader_heartbeat_ms = std::max<int64_t>(0, std::min<int64_t>(leader_heartbeat_ms, 120000));
  int64_t leader_lease_ms = body.isMember("leader_lease_ms")
    ? json_to_i64(body["leader_lease_ms"], 5000)
    : (cluster_policy ? cluster_policy->leader_lease_ms : 5000);
  leader_lease_ms = std::max<int64_t>(leader_heartbeat_ms, std::min<int64_t>(leader_lease_ms, 300000));
  int64_t poll_interval_ms = body.isMember("poll_interval_ms") ? json_to_i64(body["poll_interval_ms"], 100) : 100;
  poll_interval_ms = std::max<int64_t>(25, std::min<int64_t>(poll_interval_ms, 5000));
  int64_t deadline_ms = body.isMember("deadline_ms") ? json_to_i64(body["deadline_ms"], 10000) : 10000;
  deadline_ms = std::max<int64_t>(1000, std::min<int64_t>(deadline_ms, 300000));

  uint64_t trust_roots_epoch = body.isMember("trust_roots_epoch") ? json_to_u64(body["trust_roots_epoch"], 0) : 0;
  uint64_t revocations_epoch = body.isMember("revocations_epoch") ? json_to_u64(body["revocations_epoch"], 0) : 0;
  uint64_t cert_roots_epoch = body.isMember("cert_roots_epoch") ? json_to_u64(body["cert_roots_epoch"], 0) : 0;
  uint64_t membership_epoch = body.isMember("membership_epoch")
    ? json_to_u64(body["membership_epoch"], 0)
    : (cluster_policy && cluster_policy->membership_epoch >= 0 ? (uint64_t)cluster_policy->membership_epoch : 0);

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
  out_state->started_unix_ms = now_unix_ms();
  out_state->campaign_delay_ms = campaign_delay_ms;
  out_state->campaign_retry_ms = campaign_retry_ms;
  out_state->campaign_retry_max_ms = campaign_retry_max_ms;
  out_state->campaign_retry_backoff_factor = campaign_retry_backoff_factor;
  out_state->leader_heartbeat_ms = leader_heartbeat_ms;
  out_state->leader_lease_ms = leader_lease_ms;
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

#if !defined(_WIN32)
static bool edge_consensus_runtime_spawn_process(
  const DaemonConfig& cfg,
  const Json::Value& body,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_state) return false;
  out_state->reset();

  EdgeConsensusHttpRuntimeConfig run_cfg;
  EdgeConsensusRuntime runtime_state;
  if (!edge_consensus_runtime_build_config(cfg, body, &run_cfg, &runtime_state, out_err)) return false;
  const std::string tool_path = trim_copy(cfg.edge_consensus_node_tool_path);

  const std::string unavailable_reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  if (!unavailable_reason.empty()) {
    if (out_err) *out_err = unavailable_reason;
    return false;
  }

  std::error_code ec;
  const std::filesystem::path run_dir = edge_consensus_runtime_dir(cfg, runtime_state.node_id);
  std::filesystem::create_directories(run_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to create edge consensus runtime dir";
    return false;
  }
  const std::filesystem::path stderr_log = run_dir / "stderr.log";

  int out_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0) {
    if (out_err) *out_err = std::string("pipe failed: ") + std::strerror(errno);
    return false;
  }
  const int stderr_fd = ::open(stderr_log.string().c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (stderr_fd < 0) {
    if (out_err) *out_err = std::string("open stderr log failed: ") + std::strerror(errno);
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    if (out_err) *out_err = std::string("fork failed: ") + std::strerror(errno);
    close(stderr_fd);
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    (void)dup2(out_pipe[1], STDOUT_FILENO);
    (void)dup2(stderr_fd, STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(stderr_fd);

    std::vector<std::string> args;
    args.push_back(tool_path);
    args.push_back("--daemon-url");
    args.push_back(run_cfg.daemon_url);
    args.push_back("--node-id");
    args.push_back(runtime_state.node_id);
    args.push_back("--cluster-id");
    args.push_back(runtime_state.cluster_id);
    args.push_back("--manifest-sha256");
    args.push_back(runtime_state.manifest_sha256);
    args.push_back("--cluster-size");
    args.push_back(std::to_string((unsigned long long)runtime_state.cluster_size));
    args.push_back("--outbox-limit");
    args.push_back(std::to_string((unsigned long long)runtime_state.outbox_limit));
    args.push_back("--campaign-delay-ms");
    args.push_back(std::to_string((long long)runtime_state.campaign_delay_ms));
    args.push_back("--campaign-retry-ms");
    args.push_back(std::to_string((long long)runtime_state.campaign_retry_ms));
    args.push_back("--campaign-retry-max-ms");
    args.push_back(std::to_string((long long)runtime_state.campaign_retry_max_ms));
    args.push_back("--campaign-retry-backoff-factor");
    args.push_back(std::to_string((long long)runtime_state.campaign_retry_backoff_factor));
    args.push_back("--leader-heartbeat-ms");
    args.push_back(std::to_string((long long)runtime_state.leader_heartbeat_ms));
    args.push_back("--leader-lease-ms");
    args.push_back(std::to_string((long long)runtime_state.leader_lease_ms));
    args.push_back("--poll-interval-ms");
    args.push_back(std::to_string((long long)runtime_state.poll_interval_ms));
    args.push_back("--deadline-ms");
    args.push_back(std::to_string((long long)runtime_state.deadline_ms));
    args.push_back("--trust-roots-epoch");
    args.push_back(std::to_string((unsigned long long)runtime_state.trust_roots_epoch));
    args.push_back("--revocations-epoch");
    args.push_back(std::to_string((unsigned long long)runtime_state.revocations_epoch));
    args.push_back("--cert-roots-epoch");
    args.push_back(std::to_string((unsigned long long)runtime_state.cert_roots_epoch));
    args.push_back("--membership-epoch");
    args.push_back(std::to_string((unsigned long long)runtime_state.membership_epoch));
    args.push_back("--model");
    args.push_back(runtime_state.model);
    args.push_back("--fw-git-sha");
    args.push_back(runtime_state.fw_git_sha);
    if (!runtime_state.decision_sha256.empty()) {
      args.push_back("--decision-sha256");
      args.push_back(runtime_state.decision_sha256);
    }
    if (!run_cfg.auth_token.empty()) {
      args.push_back("--auth-token");
      args.push_back(run_cfg.auth_token);
    }
    for (const auto& peer : runtime_state.peer_node_ids) {
      args.push_back("--peer-node-id");
      args.push_back(peer);
    }
    for (const auto& member : runtime_state.member_node_ids) {
      args.push_back("--member-node-id");
      args.push_back(member);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(tool_path.c_str(), argv.data());
    _exit(127);
  }

  close(out_pipe[1]);
  close(stderr_fd);

  auto st = std::make_shared<EdgeConsensusRuntime>(std::move(runtime_state));
  st->runtime_kind = "external";
  st->tool_path = tool_path;
  st->stderr_log_path = stderr_log.string();
  st->pid = pid;

  std::thread([st, fd = out_pipe[0]]() {
    std::string buffer;
    char chunk[4096];
    for (;;) {
      const ssize_t n = ::read(fd, chunk, sizeof(chunk));
      if (n < 0) {
        if (errno == EINTR) continue;
        std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
        st->last_error = std::string("read failed: ") + std::strerror(errno);
        break;
      }
      if (n == 0) break;
      buffer.append(chunk, (size_t)n);
      for (;;) {
        const size_t nl = buffer.find('\n');
        if (nl == std::string::npos) break;
        std::string line = trim_copy(buffer.substr(0, nl));
        buffer.erase(0, nl + 1);
        if (line.empty()) continue;
        Json::Value parsed(Json::nullValue);
        std::string jerr;
        if (json_parse_any(line, &parsed, &jerr) && parsed.isObject()) {
          std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
          st->last_stdout_line = line;
          st->last_stdout_json = parsed;
          if (parsed.isMember("error") && parsed["error"].isString()) st->last_error = parsed["error"].asString();
        } else {
          std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
          st->last_stdout_line = line;
        }
      }
    }
    close(fd);
    int status = 0;
    (void)waitpid(st->pid, &status, 0);
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    st->running = false;
    st->ended_unix_ms = now_unix_ms();
    if (WIFEXITED(status)) st->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) st->exit_signal = WTERMSIG(status);
  }).detach();

  *out_state = std::move(st);
  return true;
}

static bool edge_consensus_runtime_start_builtin(
  const DaemonConfig& cfg,
  const Json::Value& body,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_state) return false;
  out_state->reset();

  EdgeConsensusHttpRuntimeConfig run_cfg;
  EdgeConsensusRuntime runtime_state;
  if (!edge_consensus_runtime_build_config(cfg, body, &run_cfg, &runtime_state, out_err)) return false;

  auto st = std::make_shared<EdgeConsensusRuntime>(std::move(runtime_state));
  st->runtime_kind = "builtin";
  st->tool_path = "@builtin";
  st->stop_requested = std::make_shared<std::atomic<bool>>(false);

  std::thread([st, run_cfg]() mutable {
    EdgeConsensusHttpRuntimeHooks hooks;
    hooks.stop_requested = st->stop_requested.get();
    hooks.log_line = [st](const std::string& line) {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      st->last_stdout_line = line;
    };
    Json::Value result(Json::nullValue);
    std::string err;
    const bool ok = run_edge_consensus_http_runtime(run_cfg, hooks, &result, &err);
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    st->running = false;
    st->ended_unix_ms = now_unix_ms();
    st->last_stdout_json = result;
    if (!result.isNull()) st->last_stdout_line = json_stringify(result);
    if (!ok) {
      st->exit_code = 1;
      st->last_error = err;
    } else {
      st->exit_code = result.isObject() && result.isMember("ok") && result["ok"].asBool() ? 0 : 1;
      if (result.isObject() && result.isMember("error") && result["error"].isString()) {
        st->last_error = result["error"].asString();
      }
    }
  }).detach();

  *out_state = std::move(st);
  return true;
}

static bool edge_consensus_runtime_kill_best_effort(std::shared_ptr<EdgeConsensusRuntime> st, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!st) return false;
  if (!st->running) return true;
  if (st->runtime_kind == "builtin") {
    if (st->stop_requested) st->stop_requested->store(true);
    const int64_t deadline = now_unix_ms() + 4000;
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
        if (!st->running) return true;
      }
      if (now_unix_ms() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (out_err) *out_err = "builtin runtime stop timeout";
    return false;
  }
  if (::kill(st->pid, SIGTERM) != 0) {
    if (errno == ESRCH) return true;
    if (out_err) *out_err = std::string("kill(SIGTERM) failed: ") + std::strerror(errno);
    return false;
  }
  const int64_t deadline = now_unix_ms() + 1500;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      if (!st->running) return true;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (::kill(st->pid, SIGKILL) != 0 && errno != ESRCH) {
    if (out_err) *out_err = std::string("kill(SIGKILL) failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

static bool edge_consensus_runtime_confirm_startup(
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  Json::Value* out_runtime,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (out_runtime) *out_runtime = Json::Value(Json::nullValue);
  if (!st) {
    if (out_err) *out_err = "missing runtime";
    return false;
  }

  const int64_t deadline = now_unix_ms() + 400;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      if (!st->running) {
        const bool completed_ok =
          st->exit_code == 0 ||
          (st->last_stdout_json.isObject() &&
           st->last_stdout_json.isMember("ok") &&
           st->last_stdout_json["ok"].isBool() &&
           st->last_stdout_json["ok"].asBool());
        if (out_runtime) *out_runtime = edge_consensus_runtime_to_json(*st);
        if (completed_ok) return true;
        if (out_err) {
          *out_err = !st->last_error.empty() ? st->last_error : "consensus runtime exited before startup confirmation";
        }
        return false;
      }
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  if (out_runtime) {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    *out_runtime = edge_consensus_runtime_to_json(*st);
  }
  return true;
}
#endif

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

Json::Value edge_consensus_runtime_backend_metadata_json(const DaemonConfig& cfg) {
  Json::Value out(Json::objectValue);
  out["builtin_available"] = true;
  const std::string default_runtime_kind = default_edge_consensus_runtime_kind(cfg);
  out["default_runtime_kind"] = default_runtime_kind;
  out["default_runtime_kind_source"] = default_edge_consensus_runtime_kind_source(cfg);
  out["tool_configured"] = !trim_copy(cfg.edge_consensus_node_tool_path).empty();
  if (!cfg.edge_consensus_node_tool_path.empty()) out["tool_path"] = cfg.edge_consensus_node_tool_path;
  out["default_daemon_url"] = default_local_daemon_url(cfg);
  const std::string external_reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  out["external_available"] = external_reason.empty();
  const std::string default_reason = default_runtime_kind == "external" ? external_reason : std::string();
  out["default_runtime_kind_available"] = default_reason.empty();
  if (!external_reason.empty()) out["external_unavailable_reason"] = external_reason;
  if (!default_reason.empty()) out["default_runtime_kind_unavailable_reason"] = default_reason;
  return out;
}

Json::Value edge_consensus_runtime_status_json_for_node(const std::string& node_id) {
  std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
  auto st = edge_consensus_runtime_lookup_locked(node_id);
  return st ? edge_consensus_runtime_to_json(*st) : Json::Value(Json::nullValue);
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
#if defined(_WIN32)
    out["error"] = "consensus_runtime stop unsupported on Windows";
    out["runtime"] = edge_consensus_runtime_to_json(*st);
    resp->status = 501;
    resp->body = json_stringify(out);
    return;
#else
    std::string serr;
    if (!edge_consensus_runtime_kill_best_effort(st, &serr)) {
      out["error"] = serr.empty() ? "failed to stop consensus runtime" : serr;
      {
        std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
        out["runtime"] = edge_consensus_runtime_to_json(*st);
      }
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      out["runtime"] = edge_consensus_runtime_to_json(*st);
    }
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
    if (st && st->running) {
      out["ok"] = true;
      out["already_running"] = true;
      out["runtime"] = edge_consensus_runtime_to_json(*st);
      resp->status = 200;
      resp->body = json_stringify(out);
      return;
    }
    if (st && !st->running) g_edge_consensus_runtime_by_node.erase(node_id);
  }

#if defined(_WIN32)
  out["error"] = "consensus_runtime start unsupported on Windows";
  resp->status = 501;
  resp->body = json_stringify(out);
  return;
#else
  std::shared_ptr<EdgeConsensusRuntime> spawned;
  std::string serr;
  const bool started = runtime_kind == "external"
    ? edge_consensus_runtime_spawn_process(cfg, body, &spawned, &serr)
    : edge_consensus_runtime_start_builtin(cfg, body, &spawned, &serr);
  if (!started) {
    out["error"] = serr.empty() ? "failed to start consensus runtime" : serr;
    out["startup_confirmed"] = false;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  Json::Value startup_runtime(Json::nullValue);
  if (!edge_consensus_runtime_confirm_startup(spawned, &startup_runtime, &serr)) {
    out["error"] = serr.empty() ? "failed to start consensus runtime" : serr;
    out["startup_confirmed"] = false;
    out["runtime"] = startup_runtime;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }
  out["startup_confirmed"] = true;
  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    g_edge_consensus_runtime_by_node[node_id] = spawned;
    out["runtime"] = edge_consensus_runtime_to_json(*spawned);
  }
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

  std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
  auto st = edge_consensus_runtime_lookup_locked(*nid);
  out["runtime"] = st ? edge_consensus_runtime_to_json(*st) : Json::Value(Json::nullValue);
  if (st) {
    const auto pol_it = cfg.edge_consensus_clusters.find(st->cluster_id);
    if (pol_it != cfg.edge_consensus_clusters.end()) out["cluster_policy"] = edge_consensus_cluster_policy_to_json(pol_it->first, pol_it->second);
  }
  out["running"] = st ? st->running : false;
  resp->status = 200;
  resp->body = json_stringify(out);
}

}  // namespace agentd
