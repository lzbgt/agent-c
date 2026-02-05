#include "tool_servers.h"

#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define AGENTD_HAVE_TOOL_SERVERS 1
#else
#define AGENTD_HAVE_TOOL_SERVERS 0
#endif

namespace agentd {
namespace {

struct ToolDef {
  std::string name;
  std::string description;
  std::string parameters_json;  // JSON Schema string
};

static std::string json_stringify_compact_local(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool parse_manifest_tools(
  const std::string& server_cmd,
  const Json::Value& root,
  std::vector<ToolDef>* out_tools,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out_tools) return false;
  out_tools->clear();

  Json::Value tools;
  if (root.isArray()) {
    tools = root;
  } else if (root.isObject() && root.isMember("tools") && root["tools"].isArray()) {
    tools = root["tools"];
  } else if (root.isObject() && root.isMember("ok") && root["ok"].isBool() && root["ok"].asBool() == false) {
    if (out_err) {
      *out_err = "tool server manifest error";
      if (root.isMember("error") && root["error"].isString()) *out_err += ": " + root["error"].asString();
    }
    return false;
  } else {
    if (out_err) *out_err = "tool server manifest must be {tools:[...]} or an array";
    return false;
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";

  for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
    const auto& t = tools[i];
    if (!t.isObject()) continue;

    const std::string name = t.isMember("name") && t["name"].isString() ? t["name"].asString() : "";
    if (name.empty()) continue;
    const std::string desc = t.isMember("description") && t["description"].isString() ? t["description"].asString() : "";

    std::string params_json;
    if (t.isMember("parameters_json") && t["parameters_json"].isString()) {
      params_json = t["parameters_json"].asString();
    } else if (t.isMember("parameters") && (t["parameters"].isObject() || t["parameters"].isArray())) {
      params_json = Json::writeString(wb, t["parameters"]);
    } else {
      if (out_err) *out_err = "tool missing parameters_json/parameters: " + name;
      return false;
    }

    ToolDef td;
    td.name = name;
    td.description = desc;
    td.parameters_json = params_json;
    out_tools->push_back(std::move(td));
  }

  if (out_tools->empty()) {
    if (out_err) *out_err = "tool server provided no tools (cmd=" + server_cmd + ")";
    return false;
  }
  return true;
}

#if AGENTD_HAVE_TOOL_SERVERS

struct ChildProc {
  pid_t pid = -1;
  int fd_in = -1;   // write to child stdin
  int fd_out = -1;  // read from child stdout
  std::string read_buf;
  uint64_t next_id = 1;
};

static void child_close_all(int* fds, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (fds[i] >= 0) close(fds[i]);
    fds[i] = -1;
  }
}

static bool spawn_shell_cmd(const std::string& cmd, ChildProc* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = ChildProc{};

  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  if (pipe(in_pipe) != 0) {
    if (out_err) *out_err = std::string("pipe(in) failed: ") + std::strerror(errno);
    return false;
  }
  if (pipe(out_pipe) != 0) {
    if (out_err) *out_err = std::string("pipe(out) failed: ") + std::strerror(errno);
    child_close_all(in_pipe, 2);
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    if (out_err) *out_err = std::string("fork failed: ") + std::strerror(errno);
    child_close_all(in_pipe, 2);
    child_close_all(out_pipe, 2);
    return false;
  }

  if (pid == 0) {
    // Child.
    (void)dup2(in_pipe[0], STDIN_FILENO);
    (void)dup2(out_pipe[1], STDOUT_FILENO);
    // Keep stderr separate from the JSON-lines protocol to avoid corrupting responses.

    child_close_all(in_pipe, 2);
    child_close_all(out_pipe, 2);

    // Exec via sh for simple operator ergonomics.
    execl("/bin/sh", "sh", "-lc", cmd.c_str(), (char*)nullptr);
    _exit(127);
  }

  // Parent.
  close(in_pipe[0]);
  close(out_pipe[1]);
  out->pid = pid;
  out->fd_in = in_pipe[1];
  out->fd_out = out_pipe[0];
  out->read_buf.clear();
  out->next_id = 1;
  return true;
}

static void kill_child_best_effort(ChildProc* p) {
  if (!p) return;
  if (p->fd_in >= 0) {
    close(p->fd_in);
    p->fd_in = -1;
  }
  if (p->fd_out >= 0) {
    close(p->fd_out);
    p->fd_out = -1;
  }
  if (p->pid > 0) {
    kill(p->pid, SIGTERM);
    int st = 0;
    (void)waitpid(p->pid, &st, 0);
    p->pid = -1;
  }
}

static bool write_all(int fd, const std::string& s, std::string* out_err) {
  if (out_err) out_err->clear();
  const char* p = s.data();
  size_t n = s.size();
  while (n > 0) {
    const ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (out_err) *out_err = std::string("write failed: ") + std::strerror(errno);
      return false;
    }
    p += (size_t)w;
    n -= (size_t)w;
  }
  return true;
}

static bool read_line_timeout(int fd, int timeout_ms, std::string* io_buf, std::string* out_line, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!io_buf || !out_line) return false;
  out_line->clear();

  auto try_extract = [&]() -> bool {
    const size_t pos = io_buf->find('\n');
    if (pos == std::string::npos) return false;
    *out_line = io_buf->substr(0, pos);
    io_buf->erase(0, pos + 1);
    return true;
  };

  if (try_extract()) return true;

  const int64_t deadline = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count() +
    std::max(1, timeout_ms);

  for (;;) {
    if (try_extract()) return true;

    const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const int remain = (int)std::max<int64_t>(1, deadline - now);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int pr = poll(&pfd, 1, remain);
    if (pr < 0) {
      if (errno == EINTR) continue;
      if (out_err) *out_err = std::string("poll failed: ") + std::strerror(errno);
      return false;
    }
    if (pr == 0) {
      if (out_err) *out_err = "timeout waiting for tool server response";
      return false;
    }
    if (!(pfd.revents & POLLIN)) {
      if (out_err) *out_err = "tool server pipe not readable";
      return false;
    }

    char buf[4096];
    const ssize_t r = read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;
      if (out_err) *out_err = std::string("read failed: ") + std::strerror(errno);
      return false;
    }
    if (r == 0) {
      if (out_err) *out_err = "tool server closed pipe";
      return false;
    }
    io_buf->append(buf, (size_t)r);
  }
}

static bool rpc_call_jsonl(
  ChildProc* p,
  const Json::Value& req,
  int timeout_ms,
  Json::Value* out_resp,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!p || p->fd_in < 0 || p->fd_out < 0 || !out_resp) return false;
  *out_resp = Json::Value(Json::nullValue);

  const std::string line = json_stringify_compact_local(req) + "\n";
  std::string werr;
  if (!write_all(p->fd_in, line, &werr)) {
    if (out_err) *out_err = werr;
    return false;
  }

  std::string raw;
  std::string rerr;
  if (!read_line_timeout(p->fd_out, timeout_ms, &p->read_buf, &raw, &rerr)) {
    if (out_err) *out_err = rerr;
    return false;
  }

  Json::Value resp;
  std::string perr;
  if (!json_parse_any(raw, &resp, &perr) || !resp.isObject()) {
    if (out_err) *out_err = "invalid tool server response json: " + perr;
    return false;
  }
  *out_resp = resp;
  return true;
}

#endif  // AGENTD_HAVE_TOOL_SERVERS

}  // namespace

struct ToolServerChain::Impl {
  std::vector<ToolServerSpec> specs;
  std::vector<std::vector<ToolDef>> tools_by_server;
  std::unordered_map<std::string, size_t> tool_to_server;
  std::unordered_set<std::string> all_tool_names;
  std::mutex mu;

#if AGENTD_HAVE_TOOL_SERVERS
  std::vector<ChildProc> procs;
  std::vector<std::unique_ptr<std::mutex>> proc_mu;
#endif

  static agent_status_t register_tools_cb(void* vctx, agent_tool_registry_t* registry) {
    if (!vctx || !registry) return AGENT_ERR_INVALID_ARGUMENT;
    auto* self = static_cast<Impl*>(vctx);
    std::lock_guard<std::mutex> lock(self->mu);

    // Capture existing tool names so servers can't clobber base tools or plugins.
    std::unordered_set<std::string> existing;
    const size_t before = agent_tool_registry_count(registry);
    existing.reserve(before + self->all_tool_names.size());
    for (size_t i = 0; i < before; i++) {
      agent_tool_def_view_t v{};
      if (agent_tool_registry_get(registry, i, &v) != AGENT_OK) continue;
      if (!v.name || !v.name[0]) continue;
      existing.insert(v.name);
    }

    for (size_t si = 0; si < self->tools_by_server.size(); si++) {
      for (const auto& t : self->tools_by_server[si]) {
        if (t.name.empty()) return AGENT_ERR_INVALID_ARGUMENT;
        if (existing.find(t.name) != existing.end()) {
          return AGENT_ERR_INVALID_ARGUMENT;
        }
        if (agent_tool_registry_add(registry, t.name.c_str(), t.description.c_str(), t.parameters_json.c_str()) != AGENT_OK) {
          return AGENT_ERR_INTERNAL;
        }
        existing.insert(t.name);
      }
    }
    return AGENT_OK;
  }

  static agent_status_t execute_tool_cb(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
    if (!vctx || !tool_name || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
    auto* self = static_cast<Impl*>(vctx);
    size_t idx = (size_t)-1;
    {
      std::lock_guard<std::mutex> lock(self->mu);
      const auto it = self->tool_to_server.find(tool_name ? tool_name : "");
      if (it == self->tool_to_server.end()) {
        const char* err = "{\"ok\":false,\"error\":\"tool not owned by any tool server\"}";
        return agent_string_set_copy(out_result, err, std::strlen(err));
      }
      idx = it->second;
    }

#if !AGENTD_HAVE_TOOL_SERVERS
    const char* err = "{\"ok\":false,\"error\":\"tool servers not supported on this platform\"}";
    return agent_string_set_copy(out_result, err, std::strlen(err));
#else
    if (idx >= self->procs.size()) {
      const char* err = "{\"ok\":false,\"error\":\"invalid tool server index\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    if (idx >= self->proc_mu.size() || !self->proc_mu[idx]) {
      const char* err = "{\"ok\":false,\"error\":\"tool server mutex missing\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }

    std::lock_guard<std::mutex> lock(*self->proc_mu[idx]);
    ChildProc& p = self->procs[idx];
    if (p.pid <= 0 || p.fd_in < 0 || p.fd_out < 0) {
      const char* err = "{\"ok\":false,\"error\":\"tool server not running\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }

    Json::Value args(Json::nullValue);
    std::string perr;
    const std::string args_s = (arguments_json && arguments_json[0]) ? std::string(arguments_json) : "{}";
    if (!json_parse_any(args_s, &args, &perr) || (!args.isObject() && !args.isNull())) {
      const char* err = "{\"ok\":false,\"error\":\"invalid tool arguments json\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    if (args.isNull()) args = Json::Value(Json::objectValue);

    Json::Value req(Json::objectValue);
    const uint64_t id = p.next_id++;
    req["id"] = (Json::UInt64)id;
    req["op"] = "execute";
    req["tool_name"] = std::string(tool_name);
    req["arguments"] = args;

    Json::Value resp;
    std::string rerr;
    if (!rpc_call_jsonl(&p, req, /*timeout_ms=*/30000, &resp, &rerr)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "tool server rpc failed";
      if (!rerr.empty()) o["detail"] = rerr;
      const std::string msg = json_stringify_compact_local(o);
      return agent_string_set_copy(out_result, msg.c_str(), msg.size());
    }

    // Expect response includes matching id, but do not fail hard on mismatch (best-effort).
    Json::Value tool_result = resp.isMember("tool_result") ? resp["tool_result"] : resp;
    std::string out_s;
    if (tool_result.isString()) {
      out_s = tool_result.asString();
      if (out_s.empty()) out_s = "{}";
    } else {
      out_s = json_stringify_compact_local(tool_result);
    }
    return agent_string_set_copy(out_result, out_s.c_str(), out_s.size());
#endif
  }
};

ToolServerChain::ToolServerChain() : impl_(new Impl()) {}

ToolServerChain::~ToolServerChain() {
  if (!impl_) return;
#if AGENTD_HAVE_TOOL_SERVERS
  for (auto& p : impl_->procs) {
    kill_child_best_effort(&p);
  }
#endif
  delete impl_;
  impl_ = nullptr;
}

bool ToolServerChain::load(const std::vector<ToolServerSpec>& specs, std::string* out_error) {
  if (out_error) out_error->clear();
  loaded_ = false;
  if (!impl_) {
    if (out_error) *out_error = "ToolServerChain missing impl";
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->specs.clear();
  impl_->tools_by_server.clear();
  impl_->tool_to_server.clear();
  impl_->all_tool_names.clear();
#if AGENTD_HAVE_TOOL_SERVERS
  for (auto& p : impl_->procs) kill_child_best_effort(&p);
  impl_->procs.clear();
  impl_->proc_mu.clear();
#endif

  if (specs.empty()) {
    loaded_ = true;
    return true;
  }

#if !AGENTD_HAVE_TOOL_SERVERS
  if (out_error) *out_error = "tool servers not supported on this platform";
  return false;
#else
  impl_->specs = specs;
  impl_->tools_by_server.resize(specs.size());
  impl_->procs.resize(specs.size());
  impl_->proc_mu.resize(specs.size());
  for (size_t i = 0; i < impl_->proc_mu.size(); i++) impl_->proc_mu[i] = std::make_unique<std::mutex>();

  for (size_t i = 0; i < specs.size(); i++) {
    const auto& s = specs[i];
    if (s.cmd.empty()) continue;

    std::string serr;
    if (!spawn_shell_cmd(s.cmd, &impl_->procs[i], &serr)) {
      if (out_error) *out_error = "failed to spawn tool server: " + serr;
      return false;
    }

    // Fetch manifest.
    Json::Value req(Json::objectValue);
    const uint64_t id = impl_->procs[i].next_id++;
    req["id"] = (Json::UInt64)id;
    req["op"] = "manifest";
    Json::Value resp;
    std::string rerr;
    if (!rpc_call_jsonl(&impl_->procs[i], req, /*timeout_ms=*/30000, &resp, &rerr)) {
      if (out_error) *out_error = "tool server manifest rpc failed: " + rerr + " (cmd=" + s.cmd + ")";
      return false;
    }

    Json::Value root = resp;
    if (resp.isObject() && resp.isMember("tools")) {
      // already ok
    } else if (resp.isObject() && resp.isMember("tool_result")) {
      root = resp["tool_result"];
    }

    std::string perr;
    if (!parse_manifest_tools(s.cmd, root, &impl_->tools_by_server[i], &perr)) {
      if (out_error) *out_error = perr;
      return false;
    }

    for (const auto& td : impl_->tools_by_server[i]) {
      if (td.name.empty()) continue;
      if (!impl_->all_tool_names.insert(td.name).second) {
        if (out_error) *out_error = "duplicate tool name across tool servers: " + td.name;
        return false;
      }
      impl_->tool_to_server[td.name] = i;
    }
  }

  if (impl_->all_tool_names.empty()) {
    if (out_error) *out_error = "no tool server tools loaded";
    return false;
  }

  loaded_ = true;
  return true;
#endif
}

ToolExtension ToolServerChain::as_tool_extension() const {
  ToolExtension ext;
  if (!impl_ || !loaded_) return ext;
  ext.ctx = (void*)impl_;
  ext.register_tools = &Impl::register_tools_cb;
  ext.execute_tool = &Impl::execute_tool_cb;
  return ext;
}

std::vector<std::string> ToolServerChain::tool_names() const {
  std::vector<std::string> out;
  if (!impl_) return out;
  std::lock_guard<std::mutex> lock(impl_->mu);
  out.reserve(impl_->all_tool_names.size());
  for (const auto& n : impl_->all_tool_names) out.push_back(n);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace agentd
