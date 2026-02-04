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

static ExecResult run_proc_capture(const std::vector<std::string>& argv, int timeout_ms, size_t max_output_bytes) {
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

  pid_t pid = 0;
  int spawn_rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
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

static std::string getenv_string(const char* k) {
  if (!k || !*k) return "";
  const char* v = getenv(k);
  return v ? std::string(v) : std::string();
}

}  // namespace

void handle_avm_job_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!cfg.yolo_default) {
    resp->status = 403;
    resp->body = "{\"ok\":false,\"error\":\"avm endpoints require yolo_default=true\"}";
    return;
  }

  const std::string avm_bin = getenv_string("AGENTD_AVM_BIN");
  if (avm_bin.empty()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"AGENTD_AVM_BIN is not set\"}";
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  const std::string obc_b64 =
    args.isMember("obc_base64") && args["obc_base64"].isString() ? args["obc_base64"].asString() : "";
  if (obc_b64.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing obc_base64\"}";
    return;
  }

  std::string obc_bytes;
  std::string berr;
  if (!base64_decode(obc_b64, &obc_bytes, &berr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "invalid obc_base64";
    o["detail"] = berr;
    resp->body = json_stringify_compact(o);
    return;
  }
  if (obc_bytes.size() > (size_t)(16 * 1024 * 1024)) {
    resp->status = 413;
    resp->body = "{\"ok\":false,\"error\":\"obc too large (max 16MB)\"}";
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
    resp->body = json_stringify_compact(o);
    return;
  }

  const int timeout_ms = 5000;
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture({avm_bin, "--print-job-json", tmp->string()}, timeout_ms, max_out);

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
    resp->body = json_stringify_compact(out);
    return;
  }
  if (r.exit_code != 0) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "avm exited non-zero";
    resp->body = json_stringify_compact(out);
    return;
  }

  Json::Value job;
  std::string jerr;
  if (!json_parse_any(r.output, &job, &jerr)) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "avm output was not valid JSON";
    out["parse_error"] = jerr;
    resp->body = json_stringify_compact(out);
    return;
  }
  out["job"] = job;
  resp->body = json_stringify_compact(out);
}

}  // namespace agentd

