#pragma once

#include "tool_extension.h"

#include <string>
#include <vector>

namespace agentd {

struct ToolPluginSpec {
  std::string path;
  // Optional JSON config blob (plugin-defined). Empty means "no config".
  std::string config_json;
};

// Runtime tool plugin loader (dlopen-based).
//
// This allows the standalone `agentd` binary (and embedding hosts) to load additional tools without recompiling.
//
// The returned ToolExtension:
// - appends tools into a registry during `register_tools`
// - dispatches only those tool names to the plugin that owns them during `execute_tool`
//
// Lifetime:
// - The returned ToolExtension points to state owned by ToolPluginChain; it is valid until the chain is destroyed.
class ToolPluginChain {
 public:
  ToolPluginChain();
  ~ToolPluginChain();

  ToolPluginChain(const ToolPluginChain&) = delete;
  ToolPluginChain& operator=(const ToolPluginChain&) = delete;

  bool load(const std::vector<ToolPluginSpec>& specs, std::string* out_error);

  bool loaded() const { return loaded_; }

  // Returns a ToolExtension that can be passed into run/tools endpoints.
  // Valid only while this ToolPluginChain object is alive.
  ToolExtension as_tool_extension() const;

  // Returns the set of tool names provided by this chain (stable after load()).
  // Useful when composing multiple tool extensions (e.g. plugins + tool servers).
  std::vector<std::string> tool_names() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool loaded_ = false;
};

}  // namespace agentd
