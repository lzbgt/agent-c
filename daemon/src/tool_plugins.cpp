#include "tool_plugins.h"

#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#define AGENTD_HAVE_DLOPEN 1
#else
#define AGENTD_HAVE_DLOPEN 0
#endif

namespace agentd {

struct ToolDef {
  std::string name;
  std::string description;
  std::string parameters_json;  // JSON Schema object string
};

struct PluginEntry {
  std::string path;
  std::string config_json;

#if AGENTD_HAVE_DLOPEN
  void* handle = nullptr;
#endif

  const char* (*manifest_json_fn)() = nullptr;
  char* (*execute_json_fn)(const char* tool_name, const char* arguments_json) = nullptr;
  void (*free_fn)(char* p) = nullptr;

  std::vector<ToolDef> tools;
};

struct ToolPluginChain::Impl {
  std::vector<PluginEntry> plugins;
  std::unordered_map<std::string, size_t> tool_to_plugin;
  std::unordered_set<std::string> all_tool_names;
  std::mutex mu;

  static agent_status_t register_tools_cb(void* vctx, agent_tool_registry_t* registry) {
    if (!vctx || !registry) return AGENT_ERR_INVALID_ARGUMENT;
    auto* self = static_cast<Impl*>(vctx);
    std::lock_guard<std::mutex> lock(self->mu);

    // Capture existing tool names so plugins can't clobber base tools or each other.
    std::unordered_set<std::string> existing;
    const size_t before = agent_tool_registry_count(registry);
    existing.reserve(before + self->all_tool_names.size());
    for (size_t i = 0; i < before; i++) {
      agent_tool_def_view_t v{};
      if (agent_tool_registry_get(registry, i, &v) != AGENT_OK) continue;
      if (!v.name || !v.name[0]) continue;
      existing.insert(v.name);
    }

    for (const auto& pe : self->plugins) {
      for (const auto& t : pe.tools) {
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

  static agent_status_t execute_tool_cb(
    void* vctx,
    const char* tool_name,
    const char* arguments_json,
    agent_string_t* out_result
  ) {
    if (!vctx || !tool_name || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
    auto* self = static_cast<Impl*>(vctx);
    std::lock_guard<std::mutex> lock(self->mu);

    const auto it = self->tool_to_plugin.find(tool_name ? tool_name : "");
    if (it == self->tool_to_plugin.end()) {
      const char* err = "{\"ok\":false,\"error\":\"tool not owned by any plugin\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    const size_t idx = it->second;
    if (idx >= self->plugins.size()) {
      const char* err = "{\"ok\":false,\"error\":\"invalid plugin index\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    auto& pe = self->plugins[idx];
    if (!pe.execute_json_fn || !pe.free_fn) {
      const char* err = "{\"ok\":false,\"error\":\"plugin missing execute/free\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }

    const char* args = arguments_json ? arguments_json : "{}";
    char* out = pe.execute_json_fn(tool_name, args);
    if (!out) {
      const char* err = "{\"ok\":false,\"error\":\"plugin returned null\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    const size_t n = std::strlen(out);
    const agent_status_t st = agent_string_set_copy(out_result, out, n);
    pe.free_fn(out);
    return st;
  }
};

ToolPluginChain::ToolPluginChain() : impl_(new Impl()) {}

ToolPluginChain::~ToolPluginChain() {
  if (!impl_) return;
#if AGENTD_HAVE_DLOPEN
  for (auto& p : impl_->plugins) {
    if (p.handle) {
      dlclose(p.handle);
      p.handle = nullptr;
    }
  }
#endif
  delete impl_;
  impl_ = nullptr;
}

static bool parse_manifest_tools(const std::string& plugin_path, const char* manifest_json, std::vector<ToolDef>* out_tools, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_tools) return false;
  out_tools->clear();

  if (!manifest_json || !manifest_json[0]) {
    if (out_err) *out_err = "plugin manifest is empty";
    return false;
  }

  Json::Value root;
  std::string perr;
  if (!json_parse_any(manifest_json, &root, &perr)) {
    if (out_err) *out_err = "invalid plugin manifest json: " + perr;
    return false;
  }

  Json::Value tools;
  if (root.isArray()) {
    tools = root;
  } else if (root.isObject() && root.isMember("tools") && root["tools"].isArray()) {
    tools = root["tools"];
  } else if (root.isObject() && root.isMember("ok") && root["ok"].isBool() && root["ok"].asBool() == false) {
    if (out_err) {
      *out_err = "plugin manifest error";
      if (root.isMember("error") && root["error"].isString()) *out_err += ": " + root["error"].asString();
    }
    return false;
  } else {
    if (out_err) *out_err = "plugin manifest must be {tools:[...]} or an array";
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
      if (out_err) {
        *out_err = "tool missing parameters_json/parameters: " + name;
      }
      return false;
    }

    ToolDef td;
    td.name = name;
    td.description = desc;
    td.parameters_json = params_json;
    out_tools->push_back(std::move(td));
  }

  if (out_tools->empty()) {
    if (out_err) *out_err = "plugin provided no tools (path=" + plugin_path + ")";
    return false;
  }
  return true;
}

bool ToolPluginChain::load(const std::vector<ToolPluginSpec>& specs, std::string* out_error) {
  if (out_error) out_error->clear();
  loaded_ = false;
  if (!impl_) {
    if (out_error) *out_error = "ToolPluginChain missing impl";
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->plugins.clear();
  impl_->tool_to_plugin.clear();
  impl_->all_tool_names.clear();

  if (specs.empty()) {
    loaded_ = true;
    return true;
  }

#if !AGENTD_HAVE_DLOPEN
  if (out_error) *out_error = "tool plugins not supported on this platform (dlopen unavailable)";
  return false;
#else
  impl_->plugins.reserve(specs.size());

  for (const auto& s : specs) {
    if (s.path.empty()) continue;

    PluginEntry pe;
    pe.path = s.path;
    pe.config_json = s.config_json;

    pe.handle = dlopen(pe.path.c_str(), RTLD_NOW);
    if (!pe.handle) {
      if (out_error) *out_error = std::string("dlopen failed: ") + (dlerror() ? dlerror() : "unknown") + " (path=" + pe.path + ")";
      return false;
    }

    pe.manifest_json_fn = (const char* (*)())dlsym(pe.handle, "agentd_tool_plugin_manifest_json");
    pe.execute_json_fn = (char* (*)(const char*, const char*))dlsym(pe.handle, "agentd_tool_plugin_execute_json");
    pe.free_fn = (void (*)(char*))dlsym(pe.handle, "agentd_tool_plugin_free");

    if (!pe.manifest_json_fn || !pe.execute_json_fn || !pe.free_fn) {
      if (out_error) *out_error = "plugin missing required symbols (need agentd_tool_plugin_manifest_json / execute_json / free)";
      dlclose(pe.handle);
      pe.handle = nullptr;
      return false;
    }

    const char* mj = pe.manifest_json_fn();
    std::string perr;
    if (!parse_manifest_tools(pe.path, mj, &pe.tools, &perr)) {
      if (out_error) *out_error = perr;
      dlclose(pe.handle);
      pe.handle = nullptr;
      return false;
    }

    const size_t idx = impl_->plugins.size();
    for (const auto& td : pe.tools) {
      if (impl_->all_tool_names.find(td.name) != impl_->all_tool_names.end()) {
        if (out_error) *out_error = "duplicate tool name across plugins: " + td.name;
        dlclose(pe.handle);
        pe.handle = nullptr;
        return false;
      }
      impl_->all_tool_names.insert(td.name);
      impl_->tool_to_plugin[td.name] = idx;
    }

    impl_->plugins.push_back(std::move(pe));
  }

  loaded_ = true;
  return true;
#endif
}

ToolExtension ToolPluginChain::as_tool_extension() const {
  ToolExtension ext;
  if (!impl_ || !loaded_ || impl_->plugins.empty()) return ext;
  ext.ctx = (void*)impl_;
  ext.register_tools = Impl::register_tools_cb;
  ext.execute_tool = Impl::execute_tool_cb;
  return ext;
}

}  // namespace agentd

