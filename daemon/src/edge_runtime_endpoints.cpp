#include "edge_runtime_endpoints.h"

#include "daemon_auth.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_model.h"
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

#if !defined(_WIN32)
static bool pid_is_running(pid_t pid) {
  if (pid <= 0) return false;
  if (::kill(pid, 0) == 0) return true;
  return errno == EPERM;
}
#endif

static std::mutex g_edge_consensus_runtime_mu;
static std::unordered_map<std::string, std::shared_ptr<EdgeConsensusRuntime>> g_edge_consensus_runtime_by_node;

static bool edge_consensus_runtime_stdout_event_is_startup_ready(const Json::Value& parsed) {
  return
    parsed.isObject() &&
    parsed.isMember("schema") &&
    parsed["schema"].isString() &&
    trim_copy(parsed["schema"].asString()) == "edge_node_consensus_runtime_event_v1" &&
    parsed.isMember("event") &&
    parsed["event"].isString() &&
    trim_copy(parsed["event"].asString()) == "startup_ready";
}

static bool edge_consensus_runtime_stdout_event_live_status(
  const Json::Value& parsed,
  Json::Value* out_status
) {
  if (out_status) *out_status = Json::Value(Json::nullValue);
  if (!parsed.isObject()) return false;
  if (!parsed.isMember("schema") || !parsed["schema"].isString()) return false;
  if (trim_copy(parsed["schema"].asString()) != "edge_node_consensus_live_status_v1") return false;
  if (!parsed.isMember("status") || !parsed["status"].isObject()) return false;
  if (out_status) *out_status = parsed["status"];
  return true;
}

static void refresh_edge_consensus_runtime_state(EdgeConsensusRuntime* st) {
  if (!st) return;
#if !defined(_WIN32)
  if (st->runtime_kind == "external" && st->running && !pid_is_running(st->pid)) {
    st->running = false;
    if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  }
#endif
}

static void finalize_recovered_edge_consensus_stop(EdgeConsensusRuntime* st, int signal_used) {
  if (!st || signal_used <= 0) return;
  if (st->status_source != "persisted" || st->running) return;
  if (st->exit_signal != 0) return;
  if (st->ended_unix_ms <= 0) st->ended_unix_ms = now_unix_ms();
  st->exit_signal = signal_used;
}

static std::shared_ptr<EdgeConsensusRuntime> edge_consensus_runtime_lookup_locked(const std::string& node_id) {
  const auto it = g_edge_consensus_runtime_by_node.find(node_id);
  return it == g_edge_consensus_runtime_by_node.end() ? nullptr : it->second;
}

static std::string edge_consensus_runtime_meta_key(const std::string& node_id) {
  return "edge.consensus_runtime." + node_id;
}

static std::filesystem::path edge_consensus_runtime_dir(const DaemonConfig& cfg, const std::string& node_id) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");
  return base / "edge_consensus_runtimes" / node_id;
}

static bool remove_edge_consensus_runtime_artifacts(
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

static bool edge_consensus_runtime_from_json(const Json::Value& v, EdgeConsensusRuntime* out, std::string* out_err) {
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
  if (v.isMember("runtime_kind") && v["runtime_kind"].isString()) st.runtime_kind = trim_copy(v["runtime_kind"].asString());
  if (st.runtime_kind != "builtin" && st.runtime_kind != "external") {
    if (out_err) *out_err = "runtime_kind must be builtin or external";
    return false;
  }
  if (v.isMember("status_source") && v["status_source"].isString()) st.status_source = trim_copy(v["status_source"].asString());
  if (v.isMember("node_id") && v["node_id"].isString()) st.node_id = trim_copy(v["node_id"].asString());
  if (v.isMember("cluster_id") && v["cluster_id"].isString()) st.cluster_id = trim_copy(v["cluster_id"].asString());
  if (v.isMember("manifest_sha256") && v["manifest_sha256"].isString()) st.manifest_sha256 = trim_copy(v["manifest_sha256"].asString());
  if (v.isMember("decision_sha256") && v["decision_sha256"].isString()) st.decision_sha256 = trim_copy(v["decision_sha256"].asString());
  if (!edge_id_is_safe(st.node_id) || !edge_id_is_safe(st.cluster_id) || !edge_sha256_token_is_safe(st.manifest_sha256)) {
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
  if (v.isMember("daemon_url") && v["daemon_url"].isString()) st.daemon_url = trim_copy(v["daemon_url"].asString());
  if (v.isMember("tool_path") && v["tool_path"].isString()) st.tool_path = trim_copy(v["tool_path"].asString());
  if (v.isMember("model") && v["model"].isString()) st.model = trim_copy(v["model"].asString());
  if (v.isMember("fw_git_sha") && v["fw_git_sha"].isString()) st.fw_git_sha = trim_copy(v["fw_git_sha"].asString());
  if (v.isMember("stderr_log_path") && v["stderr_log_path"].isString()) st.stderr_log_path = trim_copy(v["stderr_log_path"].asString());
  if (v.isMember("started_unix_ms") && (v["started_unix_ms"].isInt64() || v["started_unix_ms"].isUInt64())) st.started_unix_ms = v["started_unix_ms"].asInt64();
  if (v.isMember("ended_unix_ms") && (v["ended_unix_ms"].isInt64() || v["ended_unix_ms"].isUInt64())) st.ended_unix_ms = v["ended_unix_ms"].asInt64();
  if (v.isMember("campaign_delay_ms") && (v["campaign_delay_ms"].isInt64() || v["campaign_delay_ms"].isUInt64())) st.campaign_delay_ms = v["campaign_delay_ms"].asInt64();
  if (v.isMember("campaign_retry_ms") && (v["campaign_retry_ms"].isInt64() || v["campaign_retry_ms"].isUInt64())) st.campaign_retry_ms = v["campaign_retry_ms"].asInt64();
  if (v.isMember("campaign_retry_max_ms") && (v["campaign_retry_max_ms"].isInt64() || v["campaign_retry_max_ms"].isUInt64())) st.campaign_retry_max_ms = v["campaign_retry_max_ms"].asInt64();
  if (v.isMember("campaign_retry_backoff_factor") && (v["campaign_retry_backoff_factor"].isInt64() || v["campaign_retry_backoff_factor"].isUInt64())) st.campaign_retry_backoff_factor = v["campaign_retry_backoff_factor"].asInt64();
  if (v.isMember("leader_heartbeat_ms") && (v["leader_heartbeat_ms"].isInt64() || v["leader_heartbeat_ms"].isUInt64())) st.leader_heartbeat_ms = v["leader_heartbeat_ms"].asInt64();
  if (v.isMember("leader_lease_ms") && (v["leader_lease_ms"].isInt64() || v["leader_lease_ms"].isUInt64())) st.leader_lease_ms = v["leader_lease_ms"].asInt64();
  if (v.isMember("lease_expiry_recampaign_delay_ms") &&
      (v["lease_expiry_recampaign_delay_ms"].isInt64() || v["lease_expiry_recampaign_delay_ms"].isUInt64())) {
    st.lease_expiry_recampaign_delay_ms = v["lease_expiry_recampaign_delay_ms"].asInt64();
  }
  if (v.isMember("poll_interval_ms") && (v["poll_interval_ms"].isInt64() || v["poll_interval_ms"].isUInt64())) st.poll_interval_ms = v["poll_interval_ms"].asInt64();
  if (v.isMember("deadline_ms") && (v["deadline_ms"].isInt64() || v["deadline_ms"].isUInt64())) st.deadline_ms = v["deadline_ms"].asInt64();
  if (v.isMember("cluster_size")) st.cluster_size = json_to_u64(v["cluster_size"], st.cluster_size);
  if (v.isMember("outbox_limit")) st.outbox_limit = json_to_u64(v["outbox_limit"], st.outbox_limit);
  if (v.isMember("trust_epochs") && v["trust_epochs"].isObject()) {
    const auto& epochs = v["trust_epochs"];
    if (epochs.isMember("trust_roots_epoch")) st.trust_roots_epoch = json_to_u64(epochs["trust_roots_epoch"], st.trust_roots_epoch);
    if (epochs.isMember("revocations_epoch")) st.revocations_epoch = json_to_u64(epochs["revocations_epoch"], st.revocations_epoch);
    if (epochs.isMember("cert_roots_epoch")) st.cert_roots_epoch = json_to_u64(epochs["cert_roots_epoch"], st.cert_roots_epoch);
  }
  if (v.isMember("membership_epoch")) st.membership_epoch = json_to_u64(v["membership_epoch"], st.membership_epoch);
  if (v.isMember("running") && v["running"].isBool()) st.running = v["running"].asBool();
  if (v.isMember("exit_code") && v["exit_code"].isInt()) st.exit_code = v["exit_code"].asInt();
  if (v.isMember("exit_signal") && v["exit_signal"].isInt()) st.exit_signal = v["exit_signal"].asInt();
  if (v.isMember("last_error") && v["last_error"].isString()) st.last_error = v["last_error"].asString();
  if (v.isMember("last_stdout_line") && v["last_stdout_line"].isString()) st.last_stdout_line = v["last_stdout_line"].asString();
  if (v.isMember("last_stdout") && v["last_stdout"].isObject()) st.last_stdout_json = v["last_stdout"];
  if (v.isMember("pid") && (v["pid"].isInt64() || v["pid"].isUInt64())) st.pid = (decltype(st.pid))v["pid"].asInt64();
  *out = std::move(st);
  return true;
}

static bool persist_edge_consensus_runtime_record(AgentDb* db, const EdgeConsensusRuntime& st, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  Json::Value record = edge_consensus_runtime_to_json(st);
  record["persisted_utc_ms"] = (Json::Int64)now_unix_ms();
  return db->meta_set(edge_consensus_runtime_meta_key(st.node_id), json_stringify(record), out_err);
}

static bool clear_edge_consensus_runtime_record(AgentDb* db, const std::string& node_id, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!db) {
    if (out_err) *out_err = "db unavailable";
    return false;
  }
  return db->meta_set(edge_consensus_runtime_meta_key(node_id), "", out_err);
}

static bool load_edge_consensus_runtime_record(
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
    const std::string original_err = out_err ? *out_err : std::string("persisted edge consensus runtime record corrupt");
    std::string cerr;
    if (!clear_edge_consensus_runtime_record(db, node_id, &cerr)) {
      if (out_err) {
        *out_err = cerr.empty() ? original_err : ("failed to clear corrupt persisted edge consensus runtime record: " + cerr);
      }
      return false;
    }
    if (out_self_healed) *out_self_healed = true;
    if (out_state) out_state->reset();
    if (out_err) out_err->clear();
    return true;
  }
  st->status_source = "persisted";
  if (out_state) *out_state = std::move(st);
  return true;
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

#if !defined(_WIN32)
static bool edge_consensus_runtime_spawn_process(
  const DaemonConfig& cfg,
  AgentDb* db,
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
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 4096;
    for (int fd = 3; fd < max_fd; ++fd) close(fd);

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
    args.push_back("--lease-expiry-recampaign-delay-ms");
    args.push_back(std::to_string((long long)runtime_state.lease_expiry_recampaign_delay_ms));
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
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);

  std::thread([db, st, fd = out_pipe[0]]() {
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
          Json::Value live_status(Json::nullValue);
          std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
          if (edge_consensus_runtime_stdout_event_is_startup_ready(parsed)) {
            if (st->startup_ready) st->startup_ready->store(true);
            continue;
          }
          if (edge_consensus_runtime_stdout_event_live_status(parsed, &live_status)) {
            st->live_status_json = live_status;
            continue;
          }
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
    st->live_status_json = Json::Value(Json::nullValue);
    if (WIFEXITED(status)) st->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) st->exit_signal = WTERMSIG(status);
    std::string perr;
    (void)persist_edge_consensus_runtime_record(db, *st, &perr);
  }).detach();

  *out_state = std::move(st);
  return true;
}

static bool edge_consensus_runtime_start_builtin(
  const DaemonConfig& cfg,
  AgentDb* db,
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
  st->daemon_url = "@local";
  st->stop_requested = std::make_shared<std::atomic<bool>>(false);
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);

  std::thread([db, st, run_cfg]() mutable {
    EdgeConsensusHttpRuntimeHooks hooks;
    hooks.stop_requested = st->stop_requested.get();
    hooks.log_line = [st](const std::string& line) {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      st->last_stdout_line = line;
    };
    hooks.status_update = [st](const Json::Value& status) {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      st->live_status_json = status;
    };
    hooks.startup_ready = [st]() {
      if (st->startup_ready) st->startup_ready->store(true);
    };
    Json::Value result(Json::nullValue);
    std::string err;
    const bool ok = run_edge_consensus_local_runtime(db, run_cfg, hooks, &result, &err);
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    st->running = false;
    st->ended_unix_ms = now_unix_ms();
    st->live_status_json = Json::Value(Json::nullValue);
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
    std::string perr;
    (void)persist_edge_consensus_runtime_record(db, *st, &perr);
  }).detach();

  *out_state = std::move(st);
  return true;
}

static bool edge_consensus_runtime_kill_best_effort(
  std::shared_ptr<EdgeConsensusRuntime> st,
  int* out_signal_used,
  std::string* out_err
) {
  if (out_signal_used) *out_signal_used = 0;
  if (out_err) out_err->clear();
  if (!st) return false;
  refresh_edge_consensus_runtime_state(st.get());
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
  if (out_signal_used) *out_signal_used = SIGTERM;
  const int64_t deadline = now_unix_ms() + 1500;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
      refresh_edge_consensus_runtime_state(st.get());
      if (!st->running) return true;
    }
    if (now_unix_ms() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (::kill(st->pid, SIGKILL) != 0 && errno != ESRCH) {
    if (out_err) *out_err = std::string("kill(SIGKILL) failed: ") + std::strerror(errno);
    return false;
  }
  if (out_signal_used) *out_signal_used = SIGKILL;
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
      if (st->startup_ready && st->startup_ready->load()) {
        if (out_runtime) *out_runtime = edge_consensus_runtime_to_json(*st);
        return true;
      }
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

  {
    std::lock_guard<std::mutex> lk(g_edge_consensus_runtime_mu);
    if (st->startup_ready && !st->startup_ready->load()) {
      if (out_runtime) *out_runtime = edge_consensus_runtime_to_json(*st);
      if (out_err) *out_err = "consensus runtime did not confirm startup";
      return false;
    }
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
  out["default_daemon_url"] = edge_consensus_default_local_daemon_url(cfg);
  const std::string external_reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  out["external_available"] = external_reason.empty();
  const std::string default_reason = default_runtime_kind == "external" ? external_reason : std::string();
  out["default_runtime_kind_available"] = default_reason.empty();
  if (!external_reason.empty()) out["external_unavailable_reason"] = external_reason;
  if (!default_reason.empty()) out["default_runtime_kind_unavailable_reason"] = default_reason;
  return out;
}

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
  bool record_self_healed = false;
  std::string err;
  if (!load_edge_consensus_runtime_record(db_or_null, node_id, &persisted, &record_self_healed, &err)) return Json::Value(Json::nullValue);
  if (record_self_healed) {
    bool artifacts_deleted = false;
    std::string aerr;
    (void)remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr);
    return Json::Value(Json::nullValue);
  }
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
  return persisted ? edge_consensus_runtime_response_json(cfg, *persisted) : Json::Value(Json::nullValue);
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
      bool record_self_healed = false;
      std::string lerr;
      if (!load_edge_consensus_runtime_record(db_or_null, node_id, &st, &record_self_healed, &lerr)) {
        out["error"] = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
        resp->status = 500;
        resp->body = json_stringify(out);
        return;
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
        out["cleanup_on_corrupt_record"] = cleanup;
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
    if (!edge_consensus_runtime_kill_best_effort(st, &signal_used, &serr)) {
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
    bool record_self_healed = false;
    std::string lerr;
    if (!load_edge_consensus_runtime_record(db_or_null, node_id, &persisted, &record_self_healed, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
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
      out["cleanup_on_corrupt_record"] = cleanup;
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
  const bool started = runtime_kind == "external"
    ? edge_consensus_runtime_spawn_process(cfg, db_or_null, body, &spawned, &serr)
    : edge_consensus_runtime_start_builtin(cfg, db_or_null, body, &spawned, &serr);
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
  bool record_self_healed = false;
  if (!st) {
    std::string lerr;
    if (!load_edge_consensus_runtime_record(db_or_null, *nid, &st, &record_self_healed, &lerr)) {
      out["error"] = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
  }
  if (record_self_healed) {
    Json::Value cleanup(Json::objectValue);
    cleanup["persisted_record_cleared"] = true;
    bool artifacts_deleted = false;
    std::string aerr;
    if (remove_edge_consensus_runtime_artifacts(cfg, *nid, &artifacts_deleted, &aerr)) {
      cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
    } else if (!aerr.empty()) {
      cleanup["runtime_artifacts_delete_error"] = aerr;
    }
    out["cleanup_on_corrupt_record"] = cleanup;
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
