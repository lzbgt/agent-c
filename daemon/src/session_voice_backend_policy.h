#pragma once

#include "daemon_config.h"

#include <json/json.h>

#include <string>

namespace agentd {

std::string discover_bundled_audio_peer_tool_path(const DaemonConfig& cfg);

std::string voice_peer_builtin_runtime_mode(const DaemonConfig& cfg);

bool builtin_voice_peer_runtime_enabled(const DaemonConfig& cfg);

std::string default_voice_peer_runtime_kind(const DaemonConfig& cfg);

std::string default_voice_peer_runtime_kind_source(const DaemonConfig& cfg);

std::string voice_peer_backend_unavailable_reason(const DaemonConfig& cfg, const std::string& runtime_kind);

bool resolve_voice_peer_backend(
  const DaemonConfig& cfg,
  const std::string& runtime_kind,
  std::string* out_tool_path,
  std::string* out_node_bin,
  std::string* out_err
);

Json::Value session_voice_webrtc_backend_metadata_json(const DaemonConfig& cfg);

}  // namespace agentd
