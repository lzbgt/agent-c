#pragma once

#include "tool_extension.h"

#include <string>
#include <vector>

namespace agentd {

// Tool servers: out-of-process tool providers connected via a strict stdin/stdout JSON-lines protocol.
//
// Rationale:
// - isolates crashes/failures from agentd
// - enables ecosystem growth (Playwright, AVM policy runner, device bridges) without embedding
//
// Protocol (newline-delimited JSON objects):
// - Request:  {"id":<int>,"op":"manifest"}
// - Response: {"id":<int>,"ok":true,"tools":[{name,description,parameters|parameters_json}...]}
//
// - Request:  {"id":<int>,"op":"execute","tool_name":"...","arguments":{...}}
// - Response: {"id":<int>,"ok":true,"tool_result":<json>}   (tool_result may be an object or a string)
//             {"id":<int>,"ok":false,"error":"...","tool_result":<optional json>}
//
// The agentd tool executor returns the `tool_result` JSON (stringified if object) as the tool output.
struct ToolServerSpec {
  std::string cmd;  // executed via `/bin/sh -lc <cmd>` on Unix
  int timeout_ms = 30000;
  size_t max_line_bytes = 4 * 1024 * 1024;
};

class ToolServerChain {
 public:
  ToolServerChain();
  ~ToolServerChain();

  ToolServerChain(const ToolServerChain&) = delete;
  ToolServerChain& operator=(const ToolServerChain&) = delete;

  bool load(const std::vector<ToolServerSpec>& specs, std::string* out_error);
  bool loaded() const { return loaded_; }

  ToolExtension as_tool_extension() const;
  std::vector<std::string> tool_names() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool loaded_ = false;
};

}  // namespace agentd
