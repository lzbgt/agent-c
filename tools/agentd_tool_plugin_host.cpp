#include "tool_plugins.h"

#include "json_util.h"

#include "agent/agent.h"
#include "agent/tools.h"

#include <json/json.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using agentd::json_parse_any;
using agentd::json_stringify;
using agentd::ToolExtension;
using agentd::ToolPluginChain;
using agentd::ToolPluginSpec;

namespace {

void usage(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "agentd_tool_plugin_host")
    << " --plugin <path> [--plugin-config <json>] [--plugin <path> ...]\n";
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
