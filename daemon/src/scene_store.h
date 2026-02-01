#pragma once

#include "agent_db.h"

#include <cstdint>
#include <string>

#include <json/json.h>

namespace agentd {

// Reads the server-owned Scene state (entity_id -> entity object). Missing state is treated as empty.
bool scene_store_get(
  AgentDb* db,
  const std::string& session_id,
  Json::Value* out_scene_obj,
  int64_t* out_updated_unix_ms,
  std::string* out_error
);

// Applies ops to the Scene and persists updated state to the DB.
//
// Ops schema (intentionally flexible / YOLO-friendly):
// - create: { op:"create", id?, entity_kind, title?, props? }
// - update: { op:"update", id, props }
// - delete: { op:"delete", id }
// - clear:  { op:"clear", entity_kind? }
// - action: { op:"action", id, action, args? }  (stored as props.last_action; no hardcoded behavior)
bool scene_store_apply_ops(
  AgentDb* db,
  const std::string& session_id,
  const Json::Value& ops,
  int64_t now_unix_ms,
  Json::Value* out_apply_result,
  int64_t* out_updated_unix_ms,
  std::string* out_error
);

// Convenience helper: creates/updates a stable "artifact" entity in the scene for a given artifact JSON object.
bool scene_store_mirror_artifact(
  AgentDb* db,
  const std::string& session_id,
  const Json::Value& artifact_obj,
  const std::string& tool_call_id,
  int64_t now_unix_ms,
  std::string* out_error
);

}  // namespace agentd

