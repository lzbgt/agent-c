#include "edge_runtime_endpoints.h"

#include "daemon_auth.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_model.h"
#include "edge_consensus_runtime_process_plan.h"
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

  const std::string unavailable_reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  if (!unavailable_reason.empty()) {
    if (out_err) *out_err = unavailable_reason;
    return false;
  }

  const EdgeConsensusExternalProcessPlan process_plan =
    make_edge_consensus_external_process_plan(cfg, run_cfg, runtime_state);

  std::error_code ec;
  const std::filesystem::path run_dir = process_plan.artifacts.runtime_dir;
  std::filesystem::create_directories(run_dir, ec);
  if (ec) {
    if (out_err) *out_err = "failed to create edge consensus runtime dir";
    return false;
  }
  const std::filesystem::path stderr_log = process_plan.artifacts.stderr_log_path;

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

    std::vector<char*> argv;
    argv.reserve(process_plan.argv.size() + 1);
    for (const auto& arg : process_plan.argv) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execvp(process_plan.tool_path.c_str(), argv.data());
    _exit(127);
  }

  close(out_pipe[1]);
  close(stderr_fd);

  auto st = std::make_shared<EdgeConsensusRuntime>(std::move(runtime_state));
  st->runtime_kind = "external";
  st->tool_path = process_plan.tool_path;
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
