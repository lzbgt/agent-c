#include "avm_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "mount_allowlist.h"
#include "string_util.h"

#include "base64.h"

#include <json/json.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

#if defined(_WIN32)

namespace agentd {
namespace {

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

static void respond_avm_not_supported(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (resp) resp->status = 501;
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["error"] = "avm endpoints are not supported on Windows";
  o["error_kind"] = "unavailable";
  respond_json(resp, o);
}

}  // namespace

void handle_avm_job_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  respond_avm_not_supported(cfg, cors_cfg, req, resp);
}

void handle_avm_policy_scan_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  respond_avm_not_supported(cfg, cors_cfg, req, resp);
}

void handle_avm_inspect_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  respond_avm_not_supported(cfg, cors_cfg, req, resp);
}

void handle_avm_verify_strict_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  respond_avm_not_supported(cfg, cors_cfg, req, resp);
}

void handle_avm_trace_hash_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  respond_avm_not_supported(cfg, cors_cfg, req, resp);
}

void handle_avm_capsule_run_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  respond_avm_not_supported(cfg, cors_cfg, req, resp);
}

bool avm_capsule_run_to_json(
  const DaemonConfig&,
  const Json::Value&,
  Json::Value* out,
  std::string* out_error
) {
  if (out_error) *out_error = "avm capsule execution not supported on Windows";
  if (out) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "avm endpoints are not supported on Windows";
    o["error_kind"] = "unavailable";
    *out = o;
  }
  return false;
}

}  // namespace agentd

#else

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

struct AvmOutputEvidence {
  std::string raw_text;
  std::string json_text;
  std::string residual_text;
  std::optional<std::string> result_hash;
  std::optional<std::string> trace_hash;
  std::optional<std::string> state_hash;
};

struct AvmValidatedMount {
  std::string resolved_host_path;
  std::string resolved_container_path;
  std::string matched_root;
  bool readonly = true;
  bool is_main = true;
};

struct AvmHostEffectsPolicy {
  bool fs = false;
  bool proc = false;
  bool net = false;
};

static AvmOutputEvidence build_avm_output_evidence(const std::string& output);
static Json::Value avm_output_evidence_to_json(const AvmOutputEvidence& ev);
static Json::Value avm_host_effects_to_json(const AvmHostEffectsPolicy& policy);

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
      resp->body = json_error_body("avm endpoints require yolo_default=true");
    }
    return std::nullopt;
  }

  const std::string avm_bin = getenv_string("AGENTD_AVM_BIN");
  if (avm_bin.empty()) {
    if (resp) {
      resp->status = 503;
      resp->body = json_error_body("AGENTD_AVM_BIN is not set");
    }
    return std::nullopt;
  }
  return AvmReady{avm_bin};
}

static bool parse_obc_from_args(const Json::Value& args, std::string* out_obc_bytes, std::string* out_error);

static std::string mount_reason_to_error_kind(const std::string& reason) {
  if (
    reason == "allowlist_missing" || reason == "host_path_outside_roots" || reason == "blocked_pattern"
  ) {
    return "forbidden";
  }
  return "bad_request";
}

static std::string format_mount_error(
  size_t index,
  const MountAllowlistDecision& decision,
  const MountAllowlistStatus& status
) {
  std::string err = "mounts[" + std::to_string((unsigned long long)index) + "] rejected: " + decision.reason;
  if (!decision.blocked_pattern.empty()) err += " (" + decision.blocked_pattern + ")";
  if (decision.reason == "allowlist_missing" && !status.error.empty()) err += ": " + status.error;
  return err;
}

static Json::Value avm_mounts_to_json(const std::vector<AvmValidatedMount>& mounts) {
  Json::Value arr(Json::arrayValue);
  for (const auto& mount : mounts) {
    Json::Value row(Json::objectValue);
    row["host_path"] = mount.resolved_host_path;
    row["container_path"] = mount.resolved_container_path;
    row["readonly"] = mount.readonly;
    row["is_main"] = mount.is_main;
    if (!mount.matched_root.empty()) row["matched_root"] = mount.matched_root;
    arr.append(row);
  }
  return arr;
}

static Json::Value avm_host_effects_to_json(const AvmHostEffectsPolicy& policy) {
  Json::Value out(Json::objectValue);
  out["fs"] = policy.fs;
  out["proc"] = policy.proc;
  out["net"] = policy.net;
  return out;
}

static AvmHostEffectsPolicy avm_host_effects_allowed_from_env() {
  AvmHostEffectsPolicy allowed;
  allowed.fs = env_truthy(getenv_string("AGENTD_AVM_ALLOW_FS"));
  allowed.proc = env_truthy(getenv_string("AGENTD_AVM_ALLOW_PROC"));
  allowed.net = env_truthy(getenv_string("AGENTD_AVM_ALLOW_NET"));
  return allowed;
}

static bool parse_host_effect_bool_member(
  const Json::Value& obj,
  const char* key,
  bool* out_value,
  std::string* out_error
) {
  if (!out_value) return false;
  if (!obj.isMember(key)) return true;
  if (!obj[key].isBool()) {
    if (out_error) *out_error = std::string("host_effects.") + key + " must be boolean";
    return false;
  }
  *out_value = obj[key].asBool();
  return true;
}

static bool parse_capsule_mounts(
  const Json::Value& args,
  std::vector<AvmValidatedMount>* out_mounts,
  std::string* out_error_kind,
  std::string* out_error
) {
  if (out_mounts) out_mounts->clear();
  if (out_error_kind) out_error_kind->clear();
  if (out_error) out_error->clear();
  if (!args.isMember("mounts")) return true;
  if (!args["mounts"].isArray()) {
    if (out_error_kind) *out_error_kind = "bad_request";
    if (out_error) *out_error = "mounts must be an array";
    return false;
  }

  const Json::Value& mounts = args["mounts"];
  if (mounts.empty()) return true;
  if (mounts.size() > 16) {
    if (out_error_kind) *out_error_kind = "bad_request";
    if (out_error) *out_error = "mounts may contain at most 16 entries";
    return false;
  }

  const MountAllowlist* allow = mount_allowlist_or_null();
  const MountAllowlistStatus allow_status = mount_allowlist_status();
  std::unordered_set<std::string> seen_container_paths;
  std::vector<AvmValidatedMount> parsed;
  parsed.reserve(mounts.size());

  for (Json::ArrayIndex i = 0; i < mounts.size(); ++i) {
    const Json::Value& mount = mounts[i];
    if (!mount.isObject()) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) *out_error = "mounts[" + std::to_string((unsigned long long)i) + "] must be an object";
      return false;
    }
    if (mount.isMember("host_path") && !mount["host_path"].isString()) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) *out_error = "mounts[" + std::to_string((unsigned long long)i) + "].host_path must be string";
      return false;
    }
    if (mount.isMember("container_path") && !mount["container_path"].isString()) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) *out_error = "mounts[" + std::to_string((unsigned long long)i) + "].container_path must be string";
      return false;
    }
    if (mount.isMember("container_prefix") && !mount["container_prefix"].isString()) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) *out_error = "mounts[" + std::to_string((unsigned long long)i) + "].container_prefix must be string";
      return false;
    }

    const std::string host_path =
      mount.isMember("host_path") && mount["host_path"].isString() ? trim_copy(mount["host_path"].asString()) : "";
    const std::string container_path =
      mount.isMember("container_path") && mount["container_path"].isString() ? trim_copy(mount["container_path"].asString()) : "";
    const std::string container_prefix =
      mount.isMember("container_prefix") && mount["container_prefix"].isString() ? trim_copy(mount["container_prefix"].asString()) : "";
    bool is_main = true;
    if (mount.isMember("is_main")) {
      if (!mount["is_main"].isBool()) {
        if (out_error_kind) *out_error_kind = "bad_request";
        if (out_error) *out_error = "mounts[" + std::to_string((unsigned long long)i) + "].is_main must be boolean";
        return false;
      }
      is_main = mount["is_main"].asBool();
    }

    MountAllowlistInput input;
    input.host_path = host_path;
    input.container_path = container_path;
    input.container_prefix = container_prefix;
    input.is_main = is_main;
    const MountAllowlistDecision decision = mount_allowlist_validate(allow, input);
    if (!decision.allowed) {
      if (out_error_kind) *out_error_kind = mount_reason_to_error_kind(decision.reason);
      if (out_error) *out_error = format_mount_error((size_t)i, decision, allow_status);
      return false;
    }
    if (!seen_container_paths.insert(decision.resolved_container_path).second) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) {
        *out_error =
          "mounts contain duplicate container_path after normalization: " + decision.resolved_container_path;
      }
      return false;
    }

    AvmValidatedMount validated;
    validated.resolved_host_path = decision.resolved_host_path;
    validated.resolved_container_path = decision.resolved_container_path;
    validated.matched_root = decision.matched_root;
    validated.readonly = decision.readonly;
    validated.is_main = is_main;
    parsed.push_back(std::move(validated));
  }

  if (out_mounts) *out_mounts = std::move(parsed);
  return true;
}

static bool parse_capsule_host_effects(
  const Json::Value& args,
  const std::vector<AvmValidatedMount>& mounts,
  const std::string& allow_domains,
  AvmHostEffectsPolicy* out_policy,
  std::string* out_error_kind,
  std::string* out_error
) {
  if (out_policy) *out_policy = AvmHostEffectsPolicy{};
  if (out_error_kind) out_error_kind->clear();
  if (out_error) out_error->clear();

  AvmHostEffectsPolicy requested;
  if (args.isMember("host_effects")) {
    if (!args["host_effects"].isObject()) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) *out_error = "host_effects must be an object";
      return false;
    }
    std::string parse_error;
    if (!parse_host_effect_bool_member(args["host_effects"], "fs", &requested.fs, &parse_error) ||
        !parse_host_effect_bool_member(args["host_effects"], "proc", &requested.proc, &parse_error) ||
        !parse_host_effect_bool_member(args["host_effects"], "net", &requested.net, &parse_error)) {
      if (out_error_kind) *out_error_kind = "bad_request";
      if (out_error) *out_error = parse_error;
      return false;
    }
  }

  if (!mounts.empty() && !requested.fs) {
    if (out_error_kind) *out_error_kind = "bad_request";
    if (out_error) *out_error = "mounts require host_effects.fs=true";
    return false;
  }
  if (!allow_domains.empty() && !requested.net) {
    if (out_error_kind) *out_error_kind = "bad_request";
    if (out_error) *out_error = "allow_domains requires host_effects.net=true";
    return false;
  }

  const AvmHostEffectsPolicy allowed = avm_host_effects_allowed_from_env();
  if (requested.fs && !allowed.fs) {
    if (out_error_kind) *out_error_kind = "forbidden";
    if (out_error) *out_error = "host_effects.fs requested but AGENTD_AVM_ALLOW_FS is not enabled";
    return false;
  }
  if (requested.proc && !allowed.proc) {
    if (out_error_kind) *out_error_kind = "forbidden";
    if (out_error) *out_error = "host_effects.proc requested but AGENTD_AVM_ALLOW_PROC is not enabled";
    return false;
  }
  if (requested.net && !allowed.net) {
    if (out_error_kind) *out_error_kind = "forbidden";
    if (out_error) *out_error = "host_effects.net requested but AGENTD_AVM_ALLOW_NET is not enabled";
    return false;
  }

  if (out_policy) *out_policy = requested;
  return true;
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

  // Delegate to the args-based parser for validation/limits.
  std::string err2;
  std::string obc_bytes;
  if (!parse_obc_from_args(args, &obc_bytes, &err2)) {
    if (out_error) *out_error = err2;
    return false;
  }

  if (out_args) *out_args = args;
  if (out_obc_bytes) *out_obc_bytes = std::move(obc_bytes);
  return true;
}

static bool parse_obc_from_args(const Json::Value& args, std::string* out_obc_bytes, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_obc_bytes) out_obc_bytes->clear();
  if (!args.isObject()) {
    if (out_error) *out_error = "invalid JSON (expected object)";
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

  if (out_obc_bytes) *out_obc_bytes = std::move(obc_bytes);
  return true;
}

static Json::Value run_avm_json(const std::string& avm_bin, const std::string& avm_flag, const std::filesystem::path& obc_path) {
  const int timeout_ms = 5000;
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture({avm_bin, avm_flag, obc_path.string()}, timeout_ms, max_out);
  const AvmOutputEvidence ev = build_avm_output_evidence(r.output);

  Json::Value out(Json::objectValue);
  out["ok"] = (r.exit_code == 0 && !r.timed_out);
  out["exit_code"] = r.exit_code;
  out["timed_out"] = r.timed_out;
  out["truncated"] = r.truncated;
  out["stdout"] = r.output;
  out["output"] = avm_output_evidence_to_json(ev);

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
  const std::string& json_input = !ev.json_text.empty() ? ev.json_text : r.output;
  if (!json_parse_any(json_input, &v, &jerr)) {
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

static bool extract_first_json_object_range(
  const std::string& s,
  size_t* out_start,
  size_t* out_len,
  std::string* out_json
) {
  if (out_start) *out_start = 0;
  if (out_len) *out_len = 0;
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
        if (out_start) *out_start = start;
        if (out_len) *out_len = (i - start) + 1;
        *out_json = s.substr(start, (i - start) + 1);
        return true;
      }
    }
  }
  return false;
}

static AvmOutputEvidence build_avm_output_evidence(const std::string& output) {
  AvmOutputEvidence ev;
  ev.raw_text = output;
  ev.result_hash = parse_result_hash(output);
  ev.trace_hash = parse_trace_hash(output);
  ev.state_hash = parse_state_hash(output);

  size_t json_start = 0;
  size_t json_len = 0;
  std::string json_text;
  if (extract_first_json_object_range(output, &json_start, &json_len, &json_text)) {
    ev.json_text = json_text;
    std::string residual = output.substr(0, json_start);
    residual.append(output.substr(json_start + json_len));
    ev.residual_text = trim_copy(residual);
  } else {
    ev.residual_text = trim_copy(output);
  }
  return ev;
}

static Json::Value avm_output_evidence_to_json(const AvmOutputEvidence& ev) {
  Json::Value o(Json::objectValue);
  o["raw_text"] = ev.raw_text;
  if (!ev.json_text.empty()) o["json_text"] = ev.json_text;
  if (!ev.residual_text.empty()) o["residual_text"] = ev.residual_text;
  Json::Value hashes(Json::objectValue);
  if (ev.result_hash) hashes["result_hash"] = *ev.result_hash;
  if (ev.trace_hash) hashes["trace_hash"] = *ev.trace_hash;
  if (ev.state_hash) hashes["state_hash"] = *ev.state_hash;
  if (!hashes.empty()) o["hashes"] = hashes;
  return o;
}

}  // namespace

bool avm_capsule_run_to_json(
  const DaemonConfig& cfg,
  const Json::Value& args,
  Json::Value* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) {
    if (out_error) *out_error = "missing out";
    return false;
  }
  *out = Json::Value(Json::objectValue);

  Json::Value o(Json::objectValue);

  if (!cfg.yolo_default) {
    const std::string err = "avm capsule_run requires yolo_default=true";
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = "forbidden";
    o["error"] = err;
    *out = o;
    return true;
  }
  const std::string avm_bin = getenv_string("AGENTD_AVM_BIN");
  if (avm_bin.empty()) {
    const std::string err = "AGENTD_AVM_BIN is not set";
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = "unavailable";
    o["error"] = err;
    *out = o;
    return true;
  }
  if (!env_truthy(getenv_string("AGENTD_AVM_EXEC"))) {
    const std::string err = "avm execution is disabled (set AGENTD_AVM_EXEC=1)";
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = "forbidden";
    o["error"] = err;
    *out = o;
    return true;
  }

  std::string obc_bytes;
  std::string perr;
  if (!parse_obc_from_args(args, &obc_bytes, &perr)) {
    const std::string err = perr.empty() ? "invalid args" : perr;
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = "bad_request";
    o["error"] = err;
    *out = o;
    return true;
  }

  std::vector<AvmValidatedMount> mounts;
  std::string mount_error_kind;
  std::string mount_error;
  if (!parse_capsule_mounts(args, &mounts, &mount_error_kind, &mount_error)) {
    const std::string err = mount_error.empty() ? "invalid mounts" : mount_error;
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = mount_error_kind.empty() ? "bad_request" : mount_error_kind;
    o["error"] = err;
    *out = o;
    return true;
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

  AvmHostEffectsPolicy host_effects;
  std::string host_effects_error_kind;
  std::string host_effects_error;
  if (!parse_capsule_host_effects(args, mounts, allow_domains, &host_effects, &host_effects_error_kind, &host_effects_error)) {
    const std::string err = host_effects_error.empty() ? "invalid host_effects" : host_effects_error;
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = host_effects_error_kind.empty() ? "bad_request" : host_effects_error_kind;
    o["error"] = err;
    *out = o;
    return true;
  }

  const bool have_rng_seed =
    args.isMember("rng_seed") && (args["rng_seed"].isInt64() || args["rng_seed"].isUInt64() || args["rng_seed"].isInt());
  const int64_t rng_seed = have_rng_seed ? args["rng_seed"].asInt64() : 0;

  const bool have_time_start_ns =
    args.isMember("time_start_ns") && (args["time_start_ns"].isInt64() || args["time_start_ns"].isUInt64() || args["time_start_ns"].isInt());
  const int64_t time_start_ns = have_time_start_ns ? args["time_start_ns"].asInt64() : 0;

  std::string terr;
  const auto tmp = write_temp_file_bytes(obc_bytes, &terr);
  if (!tmp) {
    const std::string err = terr.empty() ? "failed to write temp obc" : terr;
    if (out_error) *out_error = err;
    o["ok"] = false;
    o["error_kind"] = "internal";
    o["error"] = err;
    *out = o;
    return true;
  }

  std::vector<std::string> avm_argv;
  avm_argv.reserve(16);
  avm_argv.push_back(avm_bin);
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
  env_overrides.reserve(10 + mounts.size() * 5);
  env_overrides.push_back({"AVM_GAS", std::to_string((long long)gas)});
  env_overrides.push_back({"AVM_MEM_BYTES", std::to_string((long long)mem_bytes)});
  env_overrides.push_back({"AVM_IO_BYTES", std::to_string((long long)io_bytes)});
  env_overrides.push_back({"AVM_LOG_BYTES", std::to_string((long long)log_bytes)});
  env_overrides.push_back({"AVM_DETERMINISTIC", deterministic ? "1" : "0"});
  env_overrides.push_back({"AGENTD_AVM_HOST_EFFECT_FS", host_effects.fs ? "1" : "0"});
  env_overrides.push_back({"AGENTD_AVM_HOST_EFFECT_PROC", host_effects.proc ? "1" : "0"});
  env_overrides.push_back({"AGENTD_AVM_HOST_EFFECT_NET", host_effects.net ? "1" : "0"});
  if (have_rng_seed) env_overrides.push_back({"AVM_RNG_SEED", std::to_string((long long)rng_seed)});
  if (have_time_start_ns) env_overrides.push_back({"AVM_TIME_START_NS", std::to_string((long long)time_start_ns)});
  if (!mounts.empty()) {
    const Json::Value mounts_json = avm_mounts_to_json(mounts);
    env_overrides.push_back({"AGENTD_AVM_MOUNT_COUNT", std::to_string((unsigned long long)mounts.size())});
    env_overrides.push_back({"AGENTD_AVM_MOUNTS_JSON", json_stringify_compact(mounts_json)});
    for (size_t i = 0; i < mounts.size(); ++i) {
      const auto& mount = mounts[i];
      const std::string pfx = "AGENTD_AVM_MOUNT_" + std::to_string((unsigned long long)i) + "_";
      env_overrides.push_back({pfx + "HOST_PATH", mount.resolved_host_path});
      env_overrides.push_back({pfx + "CONTAINER_PATH", mount.resolved_container_path});
      env_overrides.push_back({pfx + "READONLY", mount.readonly ? "1" : "0"});
      env_overrides.push_back({pfx + "IS_MAIN", mount.is_main ? "1" : "0"});
      env_overrides.push_back({pfx + "MATCHED_ROOT", mount.matched_root});
    }
  }

  const int outer_timeout_ms = (int)clamp_i64(timeout_ms + 1000, 1, 65000);
  const size_t max_out = 1024 * 1024;
  ExecResult r = run_proc_capture_env(avm_argv, outer_timeout_ms, max_out, env_overrides);
  const AvmOutputEvidence ev = build_avm_output_evidence(r.output);

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  o["exit_code"] = r.exit_code;
  o["timed_out"] = r.timed_out;
  o["truncated"] = r.truncated;
  o["stdout"] = r.output;
  o["output"] = avm_output_evidence_to_json(ev);
  o["host_effects"] = avm_host_effects_to_json(host_effects);
  if (!mounts.empty()) o["mounts"] = avm_mounts_to_json(mounts);

  if (r.timed_out) {
    o["ok"] = false;
    o["error_kind"] = "timeout";
    o["error"] = "avm timed out";
    *out = o;
    return true;
  }

  if (ev.json_text.empty()) {
    o["ok"] = false;
    o["error_kind"] = "bad_gateway";
    o["error"] = "missing run JSON in avm output";
    *out = o;
    return true;
  }
  Json::Value run;
  std::string jerr;
  if (!json_parse_any(ev.json_text, &run, &jerr)) {
    o["ok"] = false;
    o["error_kind"] = "bad_gateway";
    o["error"] = "failed to parse run JSON from avm output";
    o["parse_error"] = jerr;
    *out = o;
    return true;
  }

  o["run"] = run;
  o["run_json_raw"] = ev.json_text;
  if (ev.result_hash) o["result_hash"] = *ev.result_hash;
  if (ev.trace_hash) o["trace_hash"] = *ev.trace_hash;
  if (ev.state_hash) o["state_hash"] = *ev.state_hash;
  o["ok"] = (r.exit_code == 0);
  if (!o["ok"].asBool()) {
    o["error_kind"] = "bad_gateway";
    o["error"] = "avm exited non-zero";
  }
  *out = o;
  return true;
}

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
  const AvmOutputEvidence ev = build_avm_output_evidence(r.output);
  out["output"] = avm_output_evidence_to_json(ev);

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
  const AvmOutputEvidence ev = build_avm_output_evidence(r.output);

  std::error_code ec;
  std::filesystem::remove(*tmp, ec);

  Json::Value out(Json::objectValue);
  out["ok"] = (r.exit_code == 0 && !r.timed_out);
  out["exit_code"] = r.exit_code;
  out["timed_out"] = r.timed_out;
  out["truncated"] = r.truncated;
  out["stdout"] = r.output;
  out["output"] = avm_output_evidence_to_json(ev);

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

  if (!ev.trace_hash) {
    resp->status = 502;
    out["ok"] = false;
    out["error"] = "TRACE_HASH not found in avm output";
    respond_json(resp, out);
    return;
  }
  out["trace_hash"] = *ev.trace_hash;
  respond_json(resp, out);
}

void handle_avm_capsule_run_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    respond_json(resp, o);
    return;
  }

  Json::Value out;
  std::string err;
  (void)avm_capsule_run_to_json(cfg, args, &out, &err);
  const std::string ek =
    out.isObject() && out.isMember("error_kind") && out["error_kind"].isString() ? out["error_kind"].asString() : "";
  if (ek == "bad_request") resp->status = 400;
  else if (ek == "forbidden") resp->status = 403;
  else if (ek == "unavailable") resp->status = 503;
  else if (ek == "timeout" || (out.isObject() && out.isMember("timed_out") && out["timed_out"].isBool() && out["timed_out"].asBool())) {
    resp->status = 504;
  } else if (out.isObject() && out.isMember("ok") && out["ok"].isBool() && !out["ok"].asBool()) {
    resp->status = 502;
  }
  respond_json(resp, out);
}

}  // namespace agentd

#endif  // defined(_WIN32)
