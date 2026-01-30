#include "toolset_host_internal.h"

#include <chrono>
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

namespace {

using host_tools_internal::HostToolCtx;
using host_tools_internal::is_cancelled;
using host_tools_internal::set_result;

#if defined(AGENT_HAVE_JSONCPP)
using host_tools_internal::parse_json;
#endif

struct ExecResult {
  int exit_code = -1;
  bool timed_out = false;
  bool truncated = false;
  bool cancelled = false;
  std::string output;
};

static std::string trim_ws(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

static bool patch_path_is_safe_scoped(std::string p) {
  p = trim_ws(std::move(p));
  if (p.empty()) return true;
  if (p == "/dev/null") return true;
  if (p.rfind("a/", 0) == 0) p = p.substr(2);
  if (p.rfind("b/", 0) == 0) p = p.substr(2);

  std::filesystem::path fp(p);
  if (fp.is_absolute()) return false;
  for (const auto& comp : fp) {
    if (comp == "..") return false;
  }
  return true;
}

static bool patch_is_safe_scoped(const std::string& patch, std::string* out_error) {
  if (out_error) out_error->clear();
  size_t off = 0;
  while (off < patch.size()) {
    size_t end = patch.find('\n', off);
    if (end == std::string::npos) end = patch.size();
    std::string line = patch.substr(off, end - off);
    off = (end < patch.size()) ? (end + 1) : end;

    if (line.rfind("+++ ", 0) == 0 || line.rfind("--- ", 0) == 0) {
      const std::string p = line.substr(4);
      if (!patch_path_is_safe_scoped(p)) {
        if (out_error) *out_error = "patch contains unsafe path: " + trim_ws(p);
        return false;
      }
      continue;
    }
    if (line.rfind("diff --git ", 0) == 0) {
      std::string rest = trim_ws(line.substr(std::string("diff --git ").size()));
      const size_t sp = rest.find(' ');
      if (sp == std::string::npos) continue;
      const std::string a = rest.substr(0, sp);
      const std::string b = trim_ws(rest.substr(sp + 1));
      if (!patch_path_is_safe_scoped(a) || !patch_path_is_safe_scoped(b)) {
        if (out_error) *out_error = "patch contains unsafe path in diff header";
        return false;
      }
      continue;
    }
    if (line.rfind("rename from ", 0) == 0) {
      const std::string p = line.substr(std::string("rename from ").size());
      if (!patch_path_is_safe_scoped(p)) {
        if (out_error) *out_error = "patch contains unsafe path in rename from";
        return false;
      }
      continue;
    }
    if (line.rfind("rename to ", 0) == 0) {
      const std::string p = line.substr(std::string("rename to ").size());
      if (!patch_path_is_safe_scoped(p)) {
        if (out_error) *out_error = "patch contains unsafe path in rename to";
        return false;
      }
      continue;
    }
    if (line.rfind("copy from ", 0) == 0) {
      const std::string p = line.substr(std::string("copy from ").size());
      if (!patch_path_is_safe_scoped(p)) {
        if (out_error) *out_error = "patch contains unsafe path in copy from";
        return false;
      }
      continue;
    }
    if (line.rfind("copy to ", 0) == 0) {
      const std::string p = line.substr(std::string("copy to ").size());
      if (!patch_path_is_safe_scoped(p)) {
        if (out_error) *out_error = "patch contains unsafe path in copy to";
        return false;
      }
      continue;
    }
  }
  return true;
}

static std::optional<std::filesystem::path> write_temp_file(const std::string& content, std::string* out_error) {
  std::string tmpl = (std::filesystem::temp_directory_path() / "agent_patch_XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const int fd = mkstemp(buf.data());
  if (fd < 0) {
    if (out_error) *out_error = std::string("mkstemp failed: ") + std::strerror(errno);
    return std::nullopt;
  }

  const char* p = content.data();
  size_t remaining = content.size();
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

static ExecResult run_shell_exec(
  const std::string& cmd,
  int timeout_ms,
  size_t max_output_bytes,
  HostCancelCallback should_cancel,
  void* should_cancel_ctx
) {
  ExecResult r;
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    r.output = "pipe failed";
    return r;
  }
  // Non-blocking read end.
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  // Child: stdout/stderr -> pipe write end
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);

  pid_t pid = 0;
  const char* argv[] = {"/bin/sh", "-lc", cmd.c_str(), nullptr};
  int spawn_rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr, (char* const*)argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]); // parent closes write end

  if (spawn_rc != 0) {
    close(pipefd[0]);
    r.output = std::string("posix_spawn failed: ") + std::strerror(spawn_rc);
    return r;
  }

  const auto start = std::chrono::steady_clock::now();
  bool child_done = false;
  int status = 0;

  while (true) {
    if (should_cancel && should_cancel(should_cancel_ctx)) {
      r.cancelled = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }
    // Drain output.
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
          // Truncated.
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
      if (w == pid) {
        child_done = true;
      }
    }

    if (child_done) {
      break;
    }

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

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exit_code = 128 + WTERMSIG(status);
  } else {
    r.exit_code = -1;
  }

  return r;
}

static ExecResult run_proc_exec(
  const std::vector<std::string>& argv,
  int timeout_ms,
  size_t max_output_bytes,
  HostCancelCallback should_cancel,
  void* should_cancel_ctx
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
  for (const auto& s : argv) {
    cargv.push_back(const_cast<char*>(s.c_str()));
  }
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
    if (should_cancel && should_cancel(should_cancel_ctx)) {
      r.cancelled = true;
      kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
      break;
    }
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
      if (w == pid) {
        child_done = true;
      }
    }

    if (child_done) {
      break;
    }

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

  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.exit_code = 128 + WTERMSIG(status);
  } else {
    r.exit_code = -1;
  }

  return r;
}

} // namespace

namespace host_tools_internal {

agent_status_t tool_shell_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"shell_exec requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args["cmd"].isString()) {
    return write_envelope(false, "missing string field 'cmd'", Json::Value(Json::objectValue));
  }
  const std::string cmd = args["cmd"].asString();
  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 30000;
  const int max_output = args.isMember("max_output_bytes") && args["max_output_bytes"].isInt() ? args["max_output_bytes"].asInt() : 65536;

  ExecResult r = run_shell_exec(cmd, timeout_ms, (size_t)max_output, ctx ? ctx->should_cancel : nullptr, ctx ? ctx->should_cancel_ctx : nullptr);

  Json::Value data(Json::objectValue);
  data["exit_code"] = r.exit_code;
  data["timed_out"] = r.timed_out;
  data["truncated"] = r.truncated;
  data["cancelled"] = r.cancelled;
  data["output"] = r.output;
  // Note: ok is a hint only; the model should still judge based on output for cases like pip warnings.
  const bool ok_hint = (!r.timed_out && !r.cancelled && r.exit_code == 0);
  return write_envelope(ok_hint, r.cancelled ? "cancelled" : "", data);
#endif
}

agent_status_t tool_proc_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"proc_exec requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args["argv"].isArray()) {
    return write_envelope(false, "missing array field 'argv'", Json::Value(Json::objectValue));
  }
  std::vector<std::string> argv;
  for (const auto& v : args["argv"]) {
    if (!v.isString()) {
      continue;
    }
    argv.push_back(v.asString());
  }
  if (argv.empty()) {
    return write_envelope(false, "argv is empty", Json::Value(Json::objectValue));
  }
  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 30000;
  const int max_output = args.isMember("max_output_bytes") && args["max_output_bytes"].isInt() ? args["max_output_bytes"].asInt() : 65536;

  ExecResult r = run_proc_exec(argv, timeout_ms, (size_t)max_output, ctx ? ctx->should_cancel : nullptr, ctx ? ctx->should_cancel_ctx : nullptr);
  Json::Value data(Json::objectValue);
  data["argv"] = Json::Value(Json::arrayValue);
  for (const auto& s : argv) {
    data["argv"].append(s);
  }
  data["exit_code"] = r.exit_code;
  data["timed_out"] = r.timed_out;
  data["truncated"] = r.truncated;
  data["cancelled"] = r.cancelled;
  data["output"] = r.output;
  const bool ok_hint = (!r.timed_out && !r.cancelled && r.exit_code == 0);
  return write_envelope(ok_hint, r.cancelled ? "cancelled" : "", data);
#endif
}

agent_status_t tool_file_apply_patch(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"file_apply_patch requires jsoncpp\"}");
#else
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }
  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args["patch"].isString()) {
    return write_envelope(false, "missing string field 'patch'", Json::Value(Json::objectValue));
  }

  const std::string patch = args["patch"].asString();
  if (patch.empty()) {
    return write_envelope(false, "patch is empty", Json::Value(Json::objectValue));
  }

  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 30000;
  const int max_output = args.isMember("max_output_bytes") && args["max_output_bytes"].isInt() ? args["max_output_bytes"].asInt() : 65536;
  // Never allow --unsafe-paths in scoped mode: it can enable patch path traversal.
  const bool unsafe_paths = ctx->unrestricted &&
    (args.isMember("unsafe_paths") && args["unsafe_paths"].isBool() ? args["unsafe_paths"].asBool() : true);

  if (!ctx->unrestricted) {
    std::string unsafe_err;
    if (!patch_is_safe_scoped(patch, &unsafe_err)) {
      Json::Value data(Json::objectValue);
      data["tool"] = "git apply";
      data["root_dir"] = ctx->root.string();
      data["unsafe_paths"] = false;
      data["patch"] = patch;
      data["rejected"] = true;
      data["reject_reason"] = unsafe_err.empty() ? "unsafe patch path" : unsafe_err;
      return write_envelope(false, data["reject_reason"].asString(), data);
    }
  }

  std::string tmp_err;
  const std::optional<std::filesystem::path> tmp = write_temp_file(patch, &tmp_err);
  if (!tmp) {
    return write_envelope(false, tmp_err.empty() ? "failed to write temp patch" : tmp_err, Json::Value(Json::objectValue));
  }

  auto run_git_apply = [&](const std::vector<std::string>& extra_args) -> ExecResult {
    std::vector<std::string> argv;
    argv.push_back("git");
    if (!ctx->unrestricted) {
      argv.push_back("-C");
      argv.push_back(ctx->root.string());
    }
    argv.push_back("apply");
    if (unsafe_paths) {
      argv.push_back("--unsafe-paths");
    }
    for (const auto& a : extra_args) {
      argv.push_back(a);
    }
    argv.push_back(tmp->string());
    return run_proc_exec(argv, timeout_ms, (size_t)max_output, ctx ? ctx->should_cancel : nullptr, ctx ? ctx->should_cancel_ctx : nullptr);
  };

  ExecResult check = run_git_apply({"--check"});
  ExecResult apply;
  bool ok = (!check.timed_out && check.exit_code == 0);
  if (ok) {
    apply = run_git_apply({});
    ok = (!apply.timed_out && apply.exit_code == 0);
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "git apply";
  data["root_dir"] = ctx->unrestricted ? Json::Value("") : Json::Value(ctx->root.string());
  data["unsafe_paths"] = unsafe_paths;
  data["patch"] = patch;
  data["check"] = Json::Value(Json::objectValue);
  data["check"]["exit_code"] = check.exit_code;
  data["check"]["timed_out"] = check.timed_out;
  data["check"]["truncated"] = check.truncated;
  data["check"]["output"] = check.output;
  if (ok) {
    data["apply"] = Json::Value(Json::objectValue);
    data["apply"]["exit_code"] = apply.exit_code;
    data["apply"]["timed_out"] = apply.timed_out;
    data["apply"]["truncated"] = apply.truncated;
    data["apply"]["output"] = apply.output;
  }

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);
  return write_envelope(ok, ok ? "" : "git apply failed", data);
#endif
}

} // namespace host_tools_internal
