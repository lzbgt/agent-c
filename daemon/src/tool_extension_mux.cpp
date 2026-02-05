#include "tool_extension_mux.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentd {

struct ToolExtensionMux::Impl {
  std::vector<ToolExtension> exts;
  std::unordered_map<std::string, size_t> tool_to_ext;
  std::mutex mu;

  static agent_status_t register_tools_cb(void* vctx, agent_tool_registry_t* registry) {
    if (!vctx || !registry) return AGENT_ERR_INVALID_ARGUMENT;
    auto* self = static_cast<Impl*>(vctx);
    std::lock_guard<std::mutex> lock(self->mu);
    for (const auto& ext : self->exts) {
      if (!ext.register_tools) continue;
      const agent_status_t st = ext.register_tools(ext.ctx, registry);
      if (st != AGENT_OK) return st;
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

    const auto it = self->tool_to_ext.find(tool_name ? tool_name : "");
    if (it == self->tool_to_ext.end()) {
      const char* err = "{\"ok\":false,\"error\":\"tool not owned by any extension\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    const size_t idx = it->second;
    if (idx >= self->exts.size()) {
      const char* err = "{\"ok\":false,\"error\":\"invalid extension index\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    const auto& ext = self->exts[idx];
    if (!ext.execute_tool) {
      const char* err = "{\"ok\":false,\"error\":\"extension missing execute_tool\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    return ext.execute_tool(ext.ctx, tool_name, arguments_json, out_result);
  }
};

ToolExtensionMux::ToolExtensionMux() : impl_(new Impl()) {}

ToolExtensionMux::~ToolExtensionMux() {
  delete impl_;
  impl_ = nullptr;
}

bool ToolExtensionMux::init(
  const std::vector<ToolExtension>& exts,
  const std::vector<std::vector<std::string>>& tool_names_by_ext,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  initialized_ = false;
  if (!impl_) {
    if (out_error) *out_error = "ToolExtensionMux missing impl";
    return false;
  }
  if (exts.empty()) {
    if (out_error) *out_error = "no extensions provided";
    return false;
  }
  if (tool_names_by_ext.size() != exts.size()) {
    if (out_error) *out_error = "tool_names_by_ext size mismatch";
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->exts = exts;
  impl_->tool_to_ext.clear();

  for (size_t i = 0; i < tool_names_by_ext.size(); i++) {
    for (const auto& name : tool_names_by_ext[i]) {
      if (name.empty()) continue;
      const auto it = impl_->tool_to_ext.find(name);
      if (it != impl_->tool_to_ext.end()) {
        if (out_error) *out_error = "duplicate tool name across extensions: " + name;
        impl_->exts.clear();
        impl_->tool_to_ext.clear();
        return false;
      }
      impl_->tool_to_ext[name] = i;
    }
  }

  initialized_ = true;
  return true;
}

ToolExtension ToolExtensionMux::as_tool_extension() const {
  ToolExtension ext;
  if (!impl_ || !initialized_) return ext;
  ext.ctx = (void*)impl_;
  ext.register_tools = &Impl::register_tools_cb;
  ext.execute_tool = &Impl::execute_tool_cb;
  return ext;
}

}  // namespace agentd

