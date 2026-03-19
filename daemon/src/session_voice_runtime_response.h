#pragma once

#include "daemon_config.h"
#include "session_voice_backend_start.h"

#include <json/json.h>

#include <mutex>
#include <string>

namespace agentd {

bool is_safe_printable_field(const std::string& s, size_t max_len);

void voice_peer_add_runtime_metadata(const DaemonConfig& cfg, Json::Value* out);

void merge_json_object_fields(const Json::Value& src, Json::Value* dst);

void voice_peer_apply_start_backend_failure(
  const DaemonConfig& cfg,
  std::mutex& runtime_mu,
  const VoicePeerBackendStartResult& start_result,
  Json::Value* out
);

void voice_peer_apply_start_backend_success(
  const DaemonConfig& cfg,
  std::mutex& runtime_mu,
  const VoicePeerBackendStartResult& start_result,
  Json::Value* out
);

}  // namespace agentd
