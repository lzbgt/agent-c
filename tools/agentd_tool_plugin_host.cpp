#include "tool_plugins.h"

#include "json_util.h"

#include "agent/agent.h"
#include "agent/tools.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using agentd::json_parse_any;
using agentd::json_stringify;
using agentd::ToolExtension;
using agentd::ToolPluginChain;
using agentd::ToolPluginSpec;

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#define AGENTD_HOST_HAVE_RLIMIT 1
#else
#define AGENTD_HOST_HAVE_RLIMIT 0
#endif

namespace {

void usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_tool_plugin_host")
    << " --plugin <path> [--plugin-config <json>] [--plugin <path> ...]\n"
    << "  Optional limits:\n"
    << "    --limit-cpu-ms <n>    CPU time limit (milliseconds)\n"
    << "    --limit-wall-ms <n>   Wall-clock limit for the host process (milliseconds)\n"
    << "    --limit-as-mb <n>     Address-space limit (MB)\n";
}

struct HostLimits {
  int64_t cpu_ms = 0;
  int64_t wall_ms = 0;
  int64_t as_mb = 0;
};

static void apply_min_limit(int64_t value, int64_t* target) {
  if (!target || value <= 0) return;
  if (*target <= 0) {
    *target = value;
  } else {
    *target = std::min(*target, value);
  }
}

static bool extract_limit(const Json::Value& obj, const char* key, int64_t* out, std::string* err) {
  if (!out || !key) return false;
  if (!obj.isMember(key)) return true;
  const Json::Value& v = obj[key];
  if (!(v.isInt64() || v.isInt() || v.isUInt64() || v.isUInt() || v.isDouble())) {
    if (err) *err = std::string("limit ") + key + " must be a number";
    return false;
  }
  int64_t n = 0;
  if (v.isDouble()) {
    n = (int64_t)v.asDouble();
  } else {
    n = v.asInt64();
  }
  if (n <= 0) {
    return true;
  }
  apply_min_limit(n, out);
  return true;
}

static bool merge_limits_from_config(const std::string& cfg_json, HostLimits* limits, std::string* err) {
  if (err) err->clear();
  if (!limits) return false;
  if (cfg_json.empty()) return true;

  Json::Value root;
  std::string perr;
  if (!json_parse_any(cfg_json, &root, &perr)) {
    if (err) *err = "invalid config json: " + perr;
    return false;
  }

  const Json::Value* policy = &root;
  if (root.isObject() && root.isMember("policy") && root["policy"].isObject()) {
    policy = &root["policy"];
  }
  const Json::Value* lim = policy;
  if (policy->isObject() && policy->isMember("limits") && (*policy)["limits"].isObject()) {
    lim = &(*policy)["limits"];
  }
  if (!lim->isObject()) return true;

  if (!extract_limit(*lim, "cpu_ms", &limits->cpu_ms, err)) return false;
  if (!extract_limit(*lim, "wall_ms", &limits->wall_ms, err)) return false;
  if (!extract_limit(*lim, "as_mb", &limits->as_mb, err)) return false;
  return true;
}

bool apply_limits(const HostLimits& lim, std::string* out_err) {
  if (out_err) out_err->clear();
  if (lim.cpu_ms <= 0 && lim.wall_ms <= 0 && lim.as_mb <= 0) return true;

#if !AGENTD_HOST_HAVE_RLIMIT
  if (out_err) *out_err = "resource limits are not supported on this platform";
  return false;
#else
  if (lim.cpu_ms > 0) {
    const int64_t seconds = (lim.cpu_ms + 999) / 1000;
    struct rlimit rl;
    rl.rlim_cur = (rlim_t)seconds;
    rl.rlim_max = (rlim_t)seconds;
    if (setrlimit(RLIMIT_CPU, &rl) != 0) {
      if (out_err) *out_err = "failed to set RLIMIT_CPU";
      return false;
    }
  }

  if (lim.as_mb > 0) {
    const int64_t bytes = lim.as_mb * 1024LL * 1024LL;
    struct rlimit rl;
    rl.rlim_cur = (rlim_t)bytes;
    rl.rlim_max = (rlim_t)bytes;
    if (setrlimit(RLIMIT_AS, &rl) != 0) {
      if (out_err) *out_err = "failed to set RLIMIT_AS";
      return false;
    }
  }

  if (lim.wall_ms > 0) {
    std::thread([ms = lim.wall_ms]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      std::cerr << "tool plugin host wall-time limit reached\n";
      std::cerr.flush();
      std::_Exit(124);
    }).detach();
  }

  return true;
#endif
}

bool registry_from_plugins(const ToolPluginChain& chain, agent_tool_registry_t** out_registry, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_registry) return false;
  *out_registry = nullptr;

  agent_tool_registry_t* registry = nullptr;
  if (agent_tool_registry_create(&registry) != AGENT_OK || !registry) {
    if (out_err) *out_err = "failed to create tool registry";
    return false;
  }

  const ToolExtension ext = chain.as_tool_extension();
  if (!ext.register_tools || !ext.execute_tool) {
    agent_tool_registry_destroy(registry);
    if (out_err) *out_err = "plugin tool extension missing register/execute callbacks";
    return false;
  }

  const agent_status_t st = ext.register_tools(ext.ctx, registry);
  if (st != AGENT_OK) {
    agent_tool_registry_destroy(registry);
    if (out_err) *out_err = "plugin tool registration failed";
    return false;
  }

  *out_registry = registry;
  return true;
}

Json::Value build_manifest_tools(const agent_tool_registry_t* registry) {
  Json::Value tools(Json::arrayValue);
  if (!registry) return tools;

  const size_t n = agent_tool_registry_count(registry);
  for (size_t i = 0; i < n; i++) {
    agent_tool_def_view_t v{};
    if (agent_tool_registry_get(registry, i, &v) != AGENT_OK) continue;
    if (!v.name || !v.name[0]) continue;

    Json::Value t(Json::objectValue);
    t["name"] = v.name;
    if (v.description && v.description[0]) t["description"] = v.description;
    if (v.parameters_json && v.parameters_json[0]) {
      t["parameters_json"] = v.parameters_json;
    } else {
      t["parameters_json"] = "{}";
    }
    tools.append(std::move(t));
  }
  return tools;
}

std::string json_line(const Json::Value& v) {
  return json_stringify(v) + "\n";
}

bool extract_id(const Json::Value& req, Json::Value* out_id) {
  if (!out_id) return false;
  if (req.isMember("id")) {
    *out_id = req["id"];
    return true;
  }
  return false;
}

std::string arguments_to_json(const Json::Value& req) {
  if (req.isMember("arguments") && (req["arguments"].isObject() || req["arguments"].isArray())) {
    return json_stringify(req["arguments"]);
  }
  if (req.isMember("arguments_json") && req["arguments_json"].isString()) {
    return req["arguments_json"].asString();
  }
  if (req.isMember("arguments") && req["arguments"].isString()) {
    return req["arguments"].asString();
  }
  return "{}";
}

Json::Value tool_result_from_json(const std::string& raw) {
  Json::Value out;
  std::string perr;
  if (json_parse_any(raw, &out, &perr)) {
    return out;
  }
  Json::Value fallback(Json::stringValue);
  fallback = raw;
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<ToolPluginSpec> specs;
  HostLimits limits;
  std::string policy_err;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i] ? argv[i] : "";
    if (arg == "--plugin") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 2;
      }
      ToolPluginSpec s;
      s.path = argv[++i];
      specs.push_back(std::move(s));
    } else if (arg == "--plugin-config") {
      if (i + 1 >= argc || specs.empty()) {
        usage(argv[0]);
        return 2;
      }
      specs.back().config_json = argv[++i];
    } else if (arg == "--limit-cpu-ms") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 2;
      }
      try {
        limits.cpu_ms = std::stoll(argv[++i]);
      } catch (...) {
        std::cerr << "Invalid --limit-cpu-ms\n";
        return 2;
      }
    } else if (arg == "--limit-wall-ms") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 2;
      }
      try {
        limits.wall_ms = std::stoll(argv[++i]);
      } catch (...) {
        std::cerr << "Invalid --limit-wall-ms\n";
        return 2;
      }
    } else if (arg == "--limit-as-mb") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 2;
      }
      try {
        limits.as_mb = std::stoll(argv[++i]);
      } catch (...) {
        std::cerr << "Invalid --limit-as-mb\n";
        return 2;
      }
    } else if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown arg: " << arg << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  if (specs.empty()) {
    usage(argv[0]);
    return 2;
  }

  for (const auto& spec : specs) {
    if (!spec.config_json.empty()) {
      if (!merge_limits_from_config(spec.config_json, &limits, &policy_err)) {
        std::cerr << "Invalid plugin policy: " << (policy_err.empty() ? "unknown error" : policy_err) << "\n";
        return 2;
      }
    }
  }

  std::string limit_err;
  if (!apply_limits(limits, &limit_err)) {
    std::cerr << "Failed to apply limits: " << (limit_err.empty() ? "unknown error" : limit_err) << "\n";
    return 2;
  }

  ToolPluginChain chain;
  std::string err;
  if (!chain.load(specs, &err)) {
    std::cerr << "Failed to load plugins: " << (err.empty() ? "unknown error" : err) << "\n";
    return 2;
  }

  agent_tool_registry_t* registry = nullptr;
  if (!registry_from_plugins(chain, &registry, &err)) {
    std::cerr << "Failed to build plugin registry: " << (err.empty() ? "unknown error" : err) << "\n";
    return 2;
  }

  const ToolExtension ext = chain.as_tool_extension();
  const Json::Value manifest_tools = build_manifest_tools(registry);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    Json::Value req;
    std::string perr;
    if (!json_parse_any(line, &req, &perr)) {
      std::cerr << "Invalid JSON request: " << perr << "\n";
      continue;
    }

    Json::Value resp(Json::objectValue);
    extract_id(req, &resp["id"]);

    const std::string op = req.isMember("op") && req["op"].isString() ? req["op"].asString() : "";
    if (op == "manifest") {
      resp["ok"] = true;
      resp["tools"] = manifest_tools;
    } else if (op == "ping") {
      resp["ok"] = true;
      resp["pong"] = true;
    } else if (op == "execute") {
      const std::string tool_name = req.isMember("tool_name") && req["tool_name"].isString()
        ? req["tool_name"].asString()
        : "";
      if (tool_name.empty()) {
        resp["ok"] = false;
        resp["error"] = "missing tool_name";
      } else {
        const std::string args_json = arguments_to_json(req);
        agent_string_t out{};
        const agent_status_t st = ext.execute_tool(ext.ctx, tool_name.c_str(), args_json.c_str(), &out);
        if (st != AGENT_OK) {
          resp["ok"] = false;
          resp["error"] = "tool execution failed";
        } else {
          const std::string raw = out.data ? std::string(out.data, out.len) : std::string();
          agent_string_free(&out);
          resp["ok"] = true;
          resp["tool_result"] = tool_result_from_json(raw);
        }
      }
    } else {
      resp["ok"] = false;
      resp["error"] = "unknown op";
    }

    std::cout << json_line(resp);
    std::cout.flush();
  }

  agent_tool_registry_destroy(registry);
  return 0;
}
