#pragma once

#include "tool_extension.h"

#include <string>
#include <vector>

namespace agentd {

// Combines multiple ToolExtension instances into a single extension by routing tool execution by tool name.
//
// Motivation: allow mixing multiple extension sources (e.g. dlopen tool plugins + stdio tool servers)
// while keeping `run_endpoints.cpp` unchanged (it expects exactly one ToolExtension pointer).
class ToolExtensionMux {
 public:
  ToolExtensionMux();
  ~ToolExtensionMux();

  ToolExtensionMux(const ToolExtensionMux&) = delete;
  ToolExtensionMux& operator=(const ToolExtensionMux&) = delete;

  // Initializes the mux with a fixed set of extensions and their owned tool names.
  // - tool_names_by_ext[i] must include all tools registered/executed by exts[i]
  // - tool names must be unique across extensions (collisions return false).
  bool init(
    const std::vector<ToolExtension>& exts,
    const std::vector<std::vector<std::string>>& tool_names_by_ext,
    std::string* out_error
  );

  bool initialized() const { return initialized_; }

  // Returns a ToolExtension that can be passed into run/tools endpoints.
  // Valid only while this ToolExtensionMux object is alive.
  ToolExtension as_tool_extension() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool initialized_ = false;
};

}  // namespace agentd

