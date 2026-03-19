#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "session_voice_start_plan.h"
#include "session_voice_runtime_internal.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace agentd {

Json::Value voice_peer_runtime_to_json(const VoicePeerRuntime& st);

Json::Value voice_peer_runtime_backend_policy_drift_json(const DaemonConfig& cfg, const VoicePeerRuntime& st);

void voice_peer_add_runtime_snapshot(const DaemonConfig& cfg, const VoicePeerRuntime& st, Json::Value* out);

bool voice_peer_runtime_matches_start_request(
  const VoicePeerRuntime& st,
  const VoicePeerStartPlan& plan
);

bool voice_peer_runtime_from_json(const Json::Value& v, VoicePeerRuntime* out, std::string* out_err);

bool persist_voice_peer_runtime_record(AgentDb* db, const VoicePeerRuntime& st, std::string* out_err);

bool clear_voice_peer_runtime_record(AgentDb* db, const std::string& session_id, std::string* out_err);

bool load_voice_peer_runtime_record(
  AgentDb* db,
  const std::string& session_id,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  bool* out_self_healed,
  std::string* out_err
);

bool recover_voice_peer_runtime_record(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  Json::Value* out_updates,
  std::string* out_err
);

Json::Value cleanup_stale_persisted_voice_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id
);

Json::Value voice_peer_corrupt_record_cleanup_json(const DaemonConfig& cfg, const std::string& session_id);

}  // namespace agentd
