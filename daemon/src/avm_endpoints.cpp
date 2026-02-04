#include "avm_endpoints.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "string_util.h"

#include "base64.h"

#include <json/json.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace agentd {
namespace {

struct ExecResult {
  int exit_code = -1;
  bool timed_out = false;
  bool truncated = false;
  std::string output;
};

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static void respond_json(HttpResponse* resp, const Json::Value& v) {
  if (!resp) return;
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->body = json_stringify_compact(v);
}

static bool env_truthy(const std::string& v) {
  if (v.empty()) return false;
  if (v == "0" || v == "false" || v == "FALSE" || v == "no" || v == "NO") return false;
  return true;
}

static std::optional<std::filesystem::path> write_temp_file_bytes(const std::string& bytes, std::string* out_error) {
  if (out_error) out_error->clear();
  std::string tmpl = (std::filesystem::temp_directory_path() / "agentd_avm_obc_XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const int fd = mkstemp(buf.data());
  if (fd < 0) {
    if (out_error) *out_error = std::string("mkstemp failed: ") + std::strerror(errno);
    return std::nullopt;
  }
  const char* p = bytes.data();
  size_t remaining = bytes.size();
  while (remaining > 0) {
    const ssize_t n = write(fd, p, remaining);
    if (n <= 0) {
      if (out_error) *out_error = std::string("write failed: ") + std::strerror(errno);
      close(fd);
      unlink(buf.data());
      return std::nullopt;
    }
    p += (size_t)n;
    remaining -= (size_t)n;
  }
  close(fd);
  return std::filesystem::path(std::string(buf.data()));
}

static std::vector<std::string> build_env_kv_strings_with_overrides(
  const std::vector<std::pair<std::string, std::string>>& overrides
) {
  std::unordered_map<std::string, std::string> m;
  for (char** e = environ; e && *e; e++) {
    const char* s = *e;
    const char* eq = std::strchr(s, '=');
    if (!eq) continue;
    const std::string k(s, (size_t)(eq - s));
    const std::string v(eq + 1);
    m[k] = v;
  }
  for (const auto& kv : overrides) {
    if (kv.first.empty()) continue;
    m[kv.first] = kv.second;
  }
  std::vector<std::string> out;
  out.reserve(m.size());
  for (const auto& kv : m) {
    out.push_back(kv.first + "=" + kv.second);
  }
  return out;
}

static ExecResult run_proc_capture_env(
  const std::vector<std::string>& argv,
  int timeout_ms,
  size_t max_output_bytes,
  const std::vector<std::pair<std::string, std::string>>& env_overrides
) {
  ExecResult r;
  if (argv.empty()) {
    r.output = "argv is empty";
    return r;
  }

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    r.output = "pipe failed";
    return r;
  }
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);

  std::vector<std::string> env_kv = build_env_kv_strings_with_overrides(env_overrides);
  std::vector<char*> cenv;
  cenv.reserve(env_kv.size() + 1);
  for (auto& kv : env_kv) cenv.push_back(const_cast<char*>(kv.c_str()));
  cenv.push_back(nullptr);

  pid_t pid = 0;
  int spawn_rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), cenv.data());
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);

  if (spawn_rc != 0) {
    close(pipefd[0]);
    r.output = std::string("posix_spawnp failed: ") + std::strerror(spawn_rc);
    return r;
  }

  const auto start = std::chrono::steady_clock::now();
  bool child_done = false;
  int status = 0;

  while (true) {
    char buf[4096];
    while (r.output.size() < max_output_bytes) {
      ssize_t n = read(pipefd[0], buf, sizeof(buf));
      if (n > 0) {
        size_t to_add = (size_t)n;
        if (r.output.size() + to_add > max_output_bytes) {
          to_add = max_output_bytes - r.output.size();
        }
        r.output.append(buf, buf + to_add);
        if (to_add < (size_t)n) {
          r.truncated = true;
          break;
        }
      } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        break;
      }
    }

    if (!child_done) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) child_done = true;
    }

    if (child_done) break;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (timeout_ms > 0 && elapsed > timeout_ms) {
      r.timed_out = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }

    struct pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    (void)poll(&pfd, 1, 50);
  }

  close(pipefd[0]);

  if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) r.exit_code = 128 + WTERMSIG(status);
  else r.exit_code = -1;

  return r;
}

static ExecResult run_proc_capture(const std::vector<std::string>& argv, int timeout_ms, size_t max_output_bytes) {
  return run_proc_capture_env(argv, timeout_ms, max_output_bytes, {});
}

static std::string getenv_string(const char* k) {
  if (!k || !*k) return "";
  const char* v = getenv(k);
  return v ? std::string(v) : std::string();
}

struct AvmReady {
  std::string avm_bin;
};

static std::optional<AvmReady> avm_require_ready(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (resp) resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return std::nullopt;

  if (!cfg.yolo_default) {
    if (resp) {
      resp->status = 403;
      resp->body = "{\"ok\":false,\"error\":\"avm endpoints require yolo_default=true\"}";
    }
    return std::nullopt;
  }

  const std::string avm_bin = getenv_string("AGENTD_AVM_BIN");
  if (avm_bin.empty()) {
    if (resp) {
      resp->status = 503;
      resp->body = "{\"ok\":false,\"error\":\"AGENTD_AVM_BIN is not set\"}";
    }
    return std::nullopt;
  }
  return AvmReady{avm_bin};
}

static bool parse_obc_from_request_body(const std::string& body, std::string* out_obc_bytes, Json::Value* out_args, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_obc_bytes) out_obc_bytes->clear();
  if (out_args) *out_args = Json::Value(Json::objectValue);

  Json::Value args;
  std::string perr;
  if (!json_parse_object(body, &args, &perr)) {
    if (out_error) *out_error = std::string("invalid JSON: ") + perr;
    return false;
  }

  const std::string obc_b64 =
    args.isMember("obc_base64") && args["obc_base64"].isString() ? args["obc_base64"].asString() : "";
  if (obc_b64.empty()) {
    if (out_error) *out_error = "missing obc_base64";
    return false;
  }

  std::string obc_bytes;
  std::string berr;
  if (!base64_decode(obc_b64, &obc_bytes, &berr)) {
    if (out_error) *out_error = std::string("invalid obc_base64: ") + berr;
    return false;
  }
  if (obc_bytes.size() > (size_t)(16 * 1024 * 1024)) {
    if (out_error) *out_error = "obc too large (max 16MB)";
    return false;
  }

  if (out_args) *out_args = args;
  if (out_obc_bytes) *out_obc_bytes = std::move(obc_bytes);
  return true;
}

static Json::Value run_avm_json(const std::string& avm_bin, const std::string& avm_flag, const std::filesystem::path& obc_path) {
  const int timeout_ms = 5000;
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture({avm_bin, avm_flag, obc_path.string()}, timeout_ms, max_out);

  Json::Value out(Json::objectValue);
  out["ok"] = (r.exit_code == 0 && !r.timed_out);
  out["exit_code"] = r.exit_code;
  out["timed_out"] = r.timed_out;
  out["truncated"] = r.truncated;
  out["stdout"] = r.output;

  if (r.timed_out) {
    out["ok"] = false;
    out["error"] = "avm timed out";
    return out;
  }
  if (r.exit_code != 0) {
    out["ok"] = false;
    out["error"] = "avm exited non-zero";
    return out;
  }

  Json::Value v;
  std::string jerr;
  if (!json_parse_any(r.output, &v, &jerr)) {
    out["ok"] = false;
    out["error"] = "avm output was not valid JSON";
    out["parse_error"] = jerr;
    return out;
  }

  out["value"] = v;
  return out;
}

static std::optional<std::string> parse_trace_hash(const std::string& output) {
  // Real AVM prints: "TRACE_HASH <hex>" (rolling). Accept extra whitespace and extra lines.
  // Return just the <hex> token if found.
  const std::string s = output;
  size_t pos = s.find("TRACE_HASH");
  if (pos == std::string::npos) return std::nullopt;
  pos += std::string("TRACE_HASH").size();
  while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
  size_t end = pos;
  while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '\r' && s[end] != '\n') end++;
  if (end <= pos) return std::nullopt;
  return s.substr(pos, end - pos);
}

static std::optional<std::string> parse_token_line(const std::string& output, const char* key) {
  if (!key || !*key) return std::nullopt;
  const std::string s = output;
  size_t pos = s.find(key);
  if (pos == std::string::npos) return std::nullopt;
  pos += std::strlen(key);
  while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
  size_t end = pos;
  while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '\r' && s[end] != '\n') end++;
  if (end <= pos) return std::nullopt;
  return s.substr(pos, end - pos);
}

static std::optional<std::string> parse_result_hash(const std::string& output) {
  return parse_token_line(output, "RESULT_HASH");
}

static std::optional<std::string> parse_state_hash(const std::string& output) {
  return parse_token_line(output, "STATE_HASH");
}

static bool extract_first_json_object(const std::string& s, std::string* out_json) {
  if (out_json) out_json->clear();
  if (s.empty() || !out_json) return false;
  size_t start = s.find('{');
  if (start == std::string::npos) return false;

  bool in_str = false;
  bool escape = false;
  int depth = 0;
  for (size_t i = start; i < s.size(); i++) {
    const char c = s[i];
    if (in_str) {
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') {
        in_str = false;
      }
      continue;
    }

    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == '{') {
      depth++;
      continue;
    }
    if (c == '}') {
      depth--;
      if (depth == 0) {
        *out_json = s.substr(start, (i - start) + 1);
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void handle_avm_job_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  const auto ready = avm_require_ready(cfg, cors_cfg, req, resp);
  if (!ready) return;

  std::string obc_bytes;
  std::string rerr;
  if (!parse_obc_from_request_body(req.body, &obc_bytes, nullptr, &rerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = rerr;
    respond_json(resp, o);
    return;
  }

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to write temp obc";
    o["detail"] = terr;
    respond_json(resp, o);
    return;
  }

  Json::Value out = run_avm_json(ready->avm_bin, "--print-job-json", *tmp);
  std::error_code ec;
  std::filesystem::remove(*tmp, ec);
  const bool ok = out.isMember("ok") && out["ok"].asBool();
  if (!ok) {
    if (out.isMember("timed_out") && out["timed_out"].asBool()) resp->status = 504;
    else resp->status = 502;
    respond_json(resp, out);
    return;
  }
  out["job"] = out["value"];
  out.removeMember("value");
  respond_json(resp, out);
}

void handle_avm_policy_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  const auto ready = avm_require_ready(cfg, cors_cfg, req, resp);
  if (!ready) return;

  std::string obc_bytes;
  std::string rerr;
  if (!parse_obc_from_request_body(req.body, &obc_bytes, nullptr, &rerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = rerr;
    respond_json(resp, o);
    return;
  }

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to write temp obc";
    o["detail"] = terr;
    respond_json(resp, o);
    return;
  }

  Json::Value out = run_avm_json(ready->avm_bin, "--print-policy-json", *tmp);
  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  const bool ok = out.isMember("ok") && out["ok"].asBool();
  if (!ok) {
    if (out.isMember("timed_out") && out["timed_out"].asBool()) resp->status = 504;
    else resp->status = 502;
    respond_json(resp, out);
    return;
  }
  out["policy"] = out["value"];
  out.removeMember("value");
  respond_json(resp, out);
}

void handle_avm_inspect_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  const auto ready = avm_require_ready(cfg, cors_cfg, req, resp);
  if (!ready) return;

  std::string obc_bytes;
  std::string rerr;
  if (!parse_obc_from_request_body(req.body, &obc_bytes, nullptr, &rerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = rerr;
    respond_json(resp, o);
    return;
  }

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to write temp obc";
    o["detail"] = terr;
    respond_json(resp, o);
    return;
  }

  Json::Value out = run_avm_json(ready->avm_bin, "--inspect-json", *tmp);
  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  const bool ok = out.isMember("ok") && out["ok"].asBool();
  if (!ok) {
    if (out.isMember("timed_out") && out["timed_out"].asBool()) resp->status = 504;
    else resp->status = 502;
    respond_json(resp, out);
    return;
  }
  out["inspect"] = out["value"];
  out.removeMember("value");
  respond_json(resp, out);
}

void handle_avm_verify_strict_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  const auto ready = avm_require_ready(cfg, cors_cfg, req, resp);
  if (!ready) return;

  std::string obc_bytes;
  std::string rerr;
  if (!parse_obc_from_request_body(req.body, &obc_bytes, nullptr, &rerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = rerr;
    respond_json(resp, o);
    return;
  }

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to write temp obc";
    o["detail"] = terr;
    respond_json(resp, o);
    return;
  }

  const int timeout_ms = 5000;
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture({ready->avm_bin, "--verify-strict", tmp->string()}, timeout_ms, max_out);

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  Json::Value out(Json::objectValue);
  out["ok"] = (r.exit_code == 0 && !r.timed_out);
  out["exit_code"] = r.exit_code;
  out["timed_out"] = r.timed_out;
  out["truncated"] = r.truncated;
  out["stdout"] = r.output;

  if (r.timed_out) {
    resp->status = 504;
    out["ok"] = false;
    out["error"] = "avm timed out";
    respond_json(resp, out);
    return;
  }
  if (r.exit_code != 0) {
    resp->status = 422;
    out["ok"] = false;
    out["error"] = "verify-strict failed";
    respond_json(resp, out);
    return;
  }
  respond_json(resp, out);
}

void handle_avm_trace_hash_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  const auto ready = avm_require_ready(cfg, cors_cfg, req, resp);
  if (!ready) return;

  std::string obc_bytes;
  std::string rerr;
  if (!parse_obc_from_request_body(req.body, &obc_bytes, nullptr, &rerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = rerr;
    respond_json(resp, o);
    return;
  }

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to write temp obc";
    o["detail"] = terr;
    respond_json(resp, o);
    return;
  }

  const int timeout_ms = 5000;
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture({ready->avm_bin, "--print-trace-hash", tmp->string()}, timeout_ms, max_out);

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  Json::Value out(Json::objectValue);
  out["ok"] = (r.exit_code == 0 && !r.timed_out);
  out["exit_code"] = r.exit_code;
  out["timed_out"] = r.timed_out;
  out["truncated"] = r.truncated;
  out["stdout"] = r.output;

  if (r.timed_out) {
    resp->status = 504;
    out["ok"] = false;
    out["error"] = "avm timed out";
    respond_json(resp, out);
    return;
  }
  if (r.exit_code != 0) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "avm exited non-zero";
    respond_json(resp, out);
    return;
  }

  const auto th = parse_trace_hash(r.output);
  if (!th) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "TRACE_HASH not found in avm output";
    respond_json(resp, out);
    return;
  }
  out["trace_hash"] = *th;
  respond_json(resp, out);
}

void handle_avm_capsule_run_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  const auto ready = avm_require_ready(cfg, cors_cfg, req, resp);
  if (!ready) return;

  if (!env_truthy(getenv_string("AGENTD_AVM_EXEC"))) {
    resp->status = 403;
    resp->body = "{\"ok\":false,\"error\":\"avm execution is disabled (set AGENTD_AVM_EXEC=1)\"}";
    return;
  }

  Json::Value args;
  std::string obc_bytes;
  std::string rerr;
  if (!parse_obc_from_request_body(req.body, &obc_bytes, &args, &rerr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = rerr;
    respond_json(resp, o);
    return;
  }

  auto clamp_i64 = [](int64_t v, int64_t lo, int64_t hi) -> int64_t {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  };

  int64_t timeout_ms = 2000;
  if (args.isMember("timeout_ms") && (args["timeout_ms"].isInt64() || args["timeout_ms"].isUInt64() || args["timeout_ms"].isInt())) {
    timeout_ms = args["timeout_ms"].asInt64();
  }
  timeout_ms = clamp_i64(timeout_ms, 1, 60000);

  int64_t gas = 200000;
  if (args.isMember("gas") && (args["gas"].isInt64() || args["gas"].isUInt64() || args["gas"].isInt())) {
    gas = args["gas"].asInt64();
  }
  gas = clamp_i64(gas, 1, 50ll * 1000 * 1000);

  int64_t mem_bytes = 8 * 1000 * 1000;
  if (args.isMember("mem_bytes") && (args["mem_bytes"].isInt64() || args["mem_bytes"].isUInt64() || args["mem_bytes"].isInt())) {
    mem_bytes = args["mem_bytes"].asInt64();
  }
  mem_bytes = clamp_i64(mem_bytes, 64 * 1000, 512ll * 1000 * 1000);

  int64_t io_bytes = 0;
  if (args.isMember("io_bytes") && (args["io_bytes"].isInt64() || args["io_bytes"].isUInt64() || args["io_bytes"].isInt())) {
    io_bytes = args["io_bytes"].asInt64();
  }
  io_bytes = clamp_i64(io_bytes, 0, 512ll * 1000 * 1000);

  int64_t log_bytes = 0;
  if (args.isMember("log_bytes") && (args["log_bytes"].isInt64() || args["log_bytes"].isUInt64() || args["log_bytes"].isInt())) {
    log_bytes = args["log_bytes"].asInt64();
  }
  log_bytes = clamp_i64(log_bytes, 0, 512ll * 1000 * 1000);

  const bool deterministic =
    !args.isMember("deterministic") || (args["deterministic"].isBool() && args["deterministic"].asBool());

  const std::string allow_domains =
    args.isMember("allow_domains") && args["allow_domains"].isString() ? trim_copy(args["allow_domains"].asString()) : "";

  const bool have_rng_seed =
    args.isMember("rng_seed") && (args["rng_seed"].isInt64() || args["rng_seed"].isUInt64() || args["rng_seed"].isInt());
  const int64_t rng_seed = have_rng_seed ? args["rng_seed"].asInt64() : 0;

  const bool have_time_start_ns =
    args.isMember("time_start_ns") && (args["time_start_ns"].isInt64() || args["time_start_ns"].isUInt64() || args["time_start_ns"].isInt());
  const int64_t time_start_ns = have_time_start_ns ? args["time_start_ns"].asInt64() : 0;

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to write temp obc";
    o["detail"] = terr;
    respond_json(resp, o);
    return;
  }

  std::vector<std::string> avm_argv;
  avm_argv.reserve(16);
  avm_argv.push_back(ready->avm_bin);
  avm_argv.push_back("--capsule");
  avm_argv.push_back("--print-run-json");
  avm_argv.push_back("--print-result-hash");
  avm_argv.push_back("--print-trace-hash");
  avm_argv.push_back("--print-state-hash");
  avm_argv.push_back("--timeout-ms");
  avm_argv.push_back(std::to_string((long long)timeout_ms));
  if (!allow_domains.empty()) {
    avm_argv.push_back("--allow-domains");
    avm_argv.push_back(allow_domains);
  }
  avm_argv.push_back(tmp->string());

  std::vector<std::pair<std::string, std::string>> env_overrides;
  env_overrides.reserve(8);
  env_overrides.push_back({"AVM_GAS", std::to_string((long long)gas)});
  env_overrides.push_back({"AVM_MEM_BYTES", std::to_string((long long)mem_bytes)});
  env_overrides.push_back({"AVM_IO_BYTES", std::to_string((long long)io_bytes)});
  env_overrides.push_back({"AVM_LOG_BYTES", std::to_string((long long)log_bytes)});
  env_overrides.push_back({"AVM_DETERMINISTIC", deterministic ? "1" : "0"});
  if (have_rng_seed) env_overrides.push_back({"AVM_RNG_SEED", std::to_string((long long)rng_seed)});
  if (have_time_start_ns) env_overrides.push_back({"AVM_TIME_START_NS", std::to_string((long long)time_start_ns)});

  const int outer_timeout_ms = (int)clamp_i64(timeout_ms + 1000, 1, 65000);
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture_env(avm_argv, outer_timeout_ms, max_out, env_overrides);

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  Json::Value out(Json::objectValue);
  out["exit_code"] = r.exit_code;
  out["timed_out"] = r.timed_out;
  out["truncated"] = r.truncated;
  out["stdout"] = r.output;

  if (r.timed_out) {
    resp->status = 504;
    out["ok"] = false;
    out["error"] = "avm timed out";
    respond_json(resp, out);
    return;
  }

  std::string run_json_s;
  if (!extract_first_json_object(r.output, &run_json_s)) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "missing run JSON in avm output";
    respond_json(resp, out);
    return;
  }
  Json::Value run;
  std::string jerr;
  if (!json_parse_any(run_json_s, &run, &jerr)) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "failed to parse run JSON from avm output";
    out["parse_error"] = jerr;
    respond_json(resp, out);
    return;
  }

  out["run"] = run;
  if (const auto h = parse_result_hash(r.output)) out["result_hash"] = *h;
  if (const auto h = parse_trace_hash(r.output)) out["trace_hash"] = *h;
  if (const auto h = parse_state_hash(r.output)) out["state_hash"] = *h;

  out["ok"] = (r.exit_code == 0);
  respond_json(resp, out);
}

}  // namespace agentd
