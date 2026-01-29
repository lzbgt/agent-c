#include "toolset_host.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

#if defined(AGENT_HAVE_JSONCPP)
static bool parse_json(const char* arguments_json, Json::Value* out, std::string* out_err) {
  if (out_err) {
    out_err->clear();
  }
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(arguments_json ? arguments_json : "");
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) {
      *out_err = "invalid JSON: " + errs;
    }
    return false;
  }
  *out = v;
  return true;
}
#endif

static agent_status_t set_result(agent_string_t* out, const std::string& s) {
  return agent_string_set_copy(out, s.c_str(), s.size());
}

struct HostToolCtx {
  std::filesystem::path root;
  bool unrestricted = false;
};

#if 0
static agent_status_t tool_fs_read(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)arguments_json;
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_read requires jsoncpp\"}");
#else
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
  if (!args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }
  const std::string path = args["path"].asString();
  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::ifstream in(*resolved, std::ios::binary);
  if (!in.is_open()) {
    return write_envelope(false, "failed to open", Json::Value(Json::objectValue));
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  Json::Value data(Json::objectValue);
  const std::string content = oss.str();
  data["path"] = path;
  data["content"] = content;
  data["bytes"] = (Json::UInt64)content.size();
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_write(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_write requires jsoncpp\"}");
#else
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
  if (!args["path"].isString() || !args["content"].isString()) {
    return write_envelope(false, "expected {path: string, content: string, ...}", Json::Value(Json::objectValue));
  }
  const std::string path = args["path"].asString();
  const std::string content = args["content"].asString();
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? args["mode"].asString() : "overwrite";
  const bool create_dirs = args.isMember("create_dirs") && args["create_dirs"].isBool() ? args["create_dirs"].asBool() : true;

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  if (create_dirs) {
    std::filesystem::create_directories(resolved->parent_path(), ec);
  }
  std::ios::openmode om = std::ios::binary;
  if (mode == "append") {
    om |= std::ios::app;
  } else {
    om |= std::ios::trunc;
  }
  std::ofstream out(*resolved, om);
  if (!out.is_open()) {
    return write_envelope(false, "failed to open for write", Json::Value(Json::objectValue));
  }
  out.write(content.data(), (std::streamsize)content.size());
  out.flush();
  if (!out.good()) {
    return write_envelope(false, "write failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["path"] = path;
  data["bytes_written"] = (Json::UInt64)content.size();
  data["mode"] = mode;
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_mkdir(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_mkdir requires jsoncpp\"}");
#else
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
  if (!args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }
  const bool parents = args.isMember("parents") && args["parents"].isBool() ? args["parents"].asBool() : true;
  const auto resolved = resolve_under_root(ctx->root, args["path"].asString(), ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  bool created = false;
  if (parents) {
    created = std::filesystem::create_directories(*resolved, ec);
  } else {
    created = std::filesystem::create_directory(*resolved, ec);
  }
  if (ec) {
    return write_envelope(false, "mkdir failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["path"] = args["path"].asString();
  data["created"] = created;
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_remove(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_remove requires jsoncpp\"}");
#else
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
  if (!args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }
  const bool recursive = args.isMember("recursive") && args["recursive"].isBool() ? args["recursive"].asBool() : false;
  const auto p = std::filesystem::path(args["path"].asString());
  if (!ctx->unrestricted && p.is_absolute()) {
    return write_envelope(false, "absolute paths not allowed in restricted mode", Json::Value(Json::objectValue));
  }
  const auto resolved = resolve_under_root(ctx->root, args["path"].asString(), ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  uintmax_t removed = 0;
  if (recursive) {
    removed = std::filesystem::remove_all(*resolved, ec);
  } else {
    removed = std::filesystem::remove(*resolved, ec) ? 1 : 0;
  }
  if (ec) {
    return write_envelope(false, "remove failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["path"] = args["path"].asString();
  data["removed"] = (Json::UInt64)removed;
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_move(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_move requires jsoncpp\"}");
#else
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
  if (!args["from"].isString() || !args["to"].isString()) {
    return write_envelope(false, "expected {from: string, to: string}", Json::Value(Json::objectValue));
  }
  const auto from_p = std::filesystem::path(args["from"].asString());
  const auto to_p = std::filesystem::path(args["to"].asString());
  if (!ctx->unrestricted && (from_p.is_absolute() || to_p.is_absolute())) {
    return write_envelope(false, "absolute paths not allowed in restricted mode", Json::Value(Json::objectValue));
  }
  const auto from = resolve_under_root(ctx->root, args["from"].asString(), ctx->unrestricted);
  const auto to = resolve_under_root(ctx->root, args["to"].asString(), ctx->unrestricted);
  if (!from || !to) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  std::filesystem::create_directories(to->parent_path(), ec);
  ec.clear();
  std::filesystem::rename(*from, *to, ec);
  if (ec) {
    return write_envelope(false, "rename failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["from"] = args["from"].asString();
  data["to"] = args["to"].asString();
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_copy(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_copy requires jsoncpp\"}");
#else
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
  if (!args["from"].isString() || !args["to"].isString()) {
    return write_envelope(false, "expected {from: string, to: string}", Json::Value(Json::objectValue));
  }
  const bool recursive = args.isMember("recursive") && args["recursive"].isBool() ? args["recursive"].asBool() : false;
  const bool overwrite = args.isMember("overwrite") && args["overwrite"].isBool() ? args["overwrite"].asBool() : true;

  const auto from_p = std::filesystem::path(args["from"].asString());
  const auto to_p = std::filesystem::path(args["to"].asString());
  if (!ctx->unrestricted && (from_p.is_absolute() || to_p.is_absolute())) {
    return write_envelope(false, "absolute paths not allowed in restricted mode", Json::Value(Json::objectValue));
  }
  const auto from = resolve_under_root(ctx->root, args["from"].asString(), ctx->unrestricted);
  const auto to = resolve_under_root(ctx->root, args["to"].asString(), ctx->unrestricted);
  if (!from || !to) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }

  std::error_code ec;
  std::filesystem::create_directories(to->parent_path(), ec);
  ec.clear();
  auto opts = std::filesystem::copy_options::none;
  if (overwrite) {
    opts |= std::filesystem::copy_options::overwrite_existing;
  }
  if (recursive) {
    opts |= std::filesystem::copy_options::recursive;
  }
  std::filesystem::copy(*from, *to, opts, ec);
  if (ec) {
    return write_envelope(false, "copy failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["from"] = args["from"].asString();
  data["to"] = args["to"].asString();
  data["recursive"] = recursive;
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_stat(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_stat requires jsoncpp\"}");
#else
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
  if (!args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }
  const auto resolved = resolve_under_root(ctx->root, args["path"].asString(), ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  auto st = std::filesystem::status(*resolved, ec);
  if (ec) {
    return write_envelope(false, "stat failed", Json::Value(Json::objectValue));
  }
  Json::Value data(Json::objectValue);
  data["exists"] = std::filesystem::exists(st);
  data["is_file"] = std::filesystem::is_regular_file(st);
  data["is_dir"] = std::filesystem::is_directory(st);
  if (data["exists"].asBool() && data["is_file"].asBool()) {
    data["size"] = (Json::UInt64)std::filesystem::file_size(*resolved, ec);
  }
  return write_envelope(true, "", data);
#endif
}

static agent_status_t tool_fs_list(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"fs_list requires jsoncpp\"}");
#else
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
  const std::string path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : ".";
  const bool recursive = args.isMember("recursive") && args["recursive"].isBool() ? args["recursive"].asBool() : false;
  const int max_entries = args.isMember("max_entries") && args["max_entries"].isInt() ? args["max_entries"].asInt() : 200;

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_directory(*resolved, ec)) {
    return write_envelope(false, "not a directory", Json::Value(Json::objectValue));
  }

  Json::Value arr(Json::arrayValue);
  int count = 0;
  if (recursive) {
    for (auto it = std::filesystem::recursive_directory_iterator(*resolved, ec); !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (count >= max_entries) {
        break;
      }
      Json::Value e(Json::objectValue);
      e["path"] = it->path().lexically_relative(ctx->root).generic_string();
      e["type"] = it->is_directory() ? "dir" : (it->is_regular_file() ? "file" : "other");
      arr.append(e);
      count++;
    }
  } else {
    for (auto it = std::filesystem::directory_iterator(*resolved, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
      if (count >= max_entries) {
        break;
      }
      Json::Value e(Json::objectValue);
      e["path"] = it->path().lexically_relative(ctx->root).generic_string();
      e["type"] = it->is_directory() ? "dir" : (it->is_regular_file() ? "file" : "other");
      arr.append(e);
      count++;
    }
  }
  Json::Value data(Json::objectValue);
  data["path"] = path;
  data["entries"] = arr;
  data["truncated"] = (count >= max_entries);
  return write_envelope(true, "", data);
#endif
}
#endif

struct ExecResult {
  int exit_code = -1;
  bool timed_out = false;
  bool truncated = false;
  std::string output;
};

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

static ExecResult run_shell_exec(const std::string& cmd, int timeout_ms, size_t max_output_bytes) {
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

static agent_status_t tool_shell_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  if (!out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"shell_exec requires jsoncpp\"}");
#else
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

  ExecResult r = run_shell_exec(cmd, timeout_ms, (size_t)max_output);

  Json::Value data(Json::objectValue);
  data["exit_code"] = r.exit_code;
  data["timed_out"] = r.timed_out;
  data["truncated"] = r.truncated;
  data["output"] = r.output;
  // Note: ok is a hint only; the model should still judge based on output for cases like pip warnings.
  const bool ok_hint = (!r.timed_out && r.exit_code == 0);
  return write_envelope(ok_hint, "", data);
#endif
}

static ExecResult run_proc_exec(const std::vector<std::string>& argv, int timeout_ms, size_t max_output_bytes) {
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

static agent_status_t tool_proc_exec(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  if (!out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"proc_exec requires jsoncpp\"}");
#else
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

  ExecResult r = run_proc_exec(argv, timeout_ms, (size_t)max_output);
  Json::Value data(Json::objectValue);
  data["argv"] = Json::Value(Json::arrayValue);
  for (const auto& s : argv) {
    data["argv"].append(s);
  }
  data["exit_code"] = r.exit_code;
  data["timed_out"] = r.timed_out;
  data["truncated"] = r.truncated;
  data["output"] = r.output;
  const bool ok_hint = (!r.timed_out && r.exit_code == 0);
  return write_envelope(ok_hint, "", data);
#endif
}

static agent_status_t tool_file_apply_patch(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
#if !defined(AGENT_HAVE_JSONCPP)
  return set_result(out_result, "{\"ok\":false,\"error\":\"file_apply_patch requires jsoncpp\"}");
#else
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
  const bool unsafe_paths = args.isMember("unsafe_paths") && args["unsafe_paths"].isBool() ? args["unsafe_paths"].asBool() : ctx->unrestricted;

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
    return run_proc_exec(argv, timeout_ms, (size_t)max_output);
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

static agent_status_t host_tools_execute(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  if (!vctx || !tool_name || !arguments_json || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  HostToolCtx* ctx = (HostToolCtx*)vctx;
  const std::string name(tool_name);
  if (name == "shell_exec") {
    return tool_shell_exec(ctx, arguments_json, out_result);
  }
  if (name == "proc_exec") {
    return tool_proc_exec(ctx, arguments_json, out_result);
  }
  if (name == "file_apply_patch") {
    return tool_file_apply_patch(ctx, arguments_json, out_result);
  }
  // Keep the response machine-readable so the LLM can reason about failures.
  return set_result(out_result, "{\"ok\":false,\"error\":\"unknown tool\",\"data\":{}}");
}

static agent_status_t add_tool(agent_tool_registry_t* r, const char* name, const char* desc, const char* params_json) {
  return agent_tool_registry_add(r, name, desc, params_json);
}

} // namespace

agent_status_t toolset_host_create(const HostToolsetConfig& cfg, agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor) {
  if (!out_registry || !out_executor) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_registry = nullptr;
  out_executor->ctx = nullptr;
  out_executor->execute = nullptr;

  agent_tool_registry_t* r = nullptr;
  HostToolCtx* ctx = nullptr;
  agent_status_t st = agent_tool_registry_create(&r);
  if (st != AGENT_OK) {
    return st;
  }

  // Process exec (primary mechanism for host-side tooling).
  st = add_tool(
    r,
    "shell_exec",
    "Execute /bin/sh -lc <cmd>. Returns JSON envelope: {ok, error?, data:{exit_code, timed_out, output}}. ok is a hint; judge success from output.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"cmd\":{\"type\":\"string\"},"
    "  \"timeout_ms\":{\"type\":\"integer\"},"
    "  \"max_output_bytes\":{\"type\":\"integer\"}"
    "},"
    "\"required\":[\"cmd\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "proc_exec",
    "Execute a process without a shell (posix_spawnp). Returns JSON envelope: {ok, error?, data:{argv, exit_code, timed_out, truncated, output}}.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "  \"timeout_ms\":{\"type\":\"integer\"},"
    "  \"max_output_bytes\":{\"type\":\"integer\"}"
    "},"
    "\"required\":[\"argv\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "file_apply_patch",
    "Apply a unified-diff patch (uses `git apply`). Returns JSON envelope: {ok, error?, data:{patch, check, apply?}}.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"patch\":{\"type\":\"string\"},"
    "  \"timeout_ms\":{\"type\":\"integer\"},"
    "  \"max_output_bytes\":{\"type\":\"integer\"},"
    "  \"unsafe_paths\":{\"type\":\"boolean\"}"
    "},"
    "\"required\":[\"patch\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  // Executor context (owned by host; for CLI we just heap-allocate).
  ctx = new (std::nothrow) HostToolCtx();
  if (!ctx) {
    st = AGENT_ERR_OOM;
    goto fail;
  }
  ctx->unrestricted = cfg.root_dir.empty();
  ctx->root = cfg.root_dir.empty() ? std::filesystem::current_path()
                                   : std::filesystem::path(cfg.root_dir);
  {
    // Normalize to reduce symlink/canonical mismatch (e.g. /var vs /private/var on macOS).
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(ctx->root, ec);
    if (!ec) {
      ctx->root = canon;
    } else {
      ctx->root = ctx->root.lexically_normal();
    }
  }

  out_executor->ctx = ctx;
  out_executor->execute = host_tools_execute;
  *out_registry = r;
  return AGENT_OK;

fail:
  if (ctx) {
    delete ctx;
  }
  agent_tool_registry_destroy(r);
  return st;
}

void toolset_host_destroy(agent_tool_executor_t* executor) {
  if (!executor) {
    return;
  }
  if (executor->ctx) {
    HostToolCtx* ctx = (HostToolCtx*)executor->ctx;
    delete ctx;
  }
  executor->ctx = nullptr;
  executor->execute = nullptr;
}
