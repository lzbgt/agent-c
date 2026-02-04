#pragma once

#include "agent_db.h"

#include <json/json.h>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {

int64_t edge_unix_ms_now();
std::string edge_json_stringify_compact(const Json::Value& v);

bool edge_id_is_safe(const std::string& s);
std::string edge_make_uuidish_msg_id();
std::string edge_node_to_prefix(const std::string& node_id);

bool edge_parse_string_set(const std::string& json, std::unordered_set<std::string>* out);

void edge_manifest_extract_best_effort(
  const Json::Value& manifest,
  std::string* out_tags_json,
  std::string* out_tools_json,
  std::string* out_hw_presence_json
);

bool edge_select_node_match_any(
  AgentDb* db,
  const std::vector<std::string>& requires_tools,
  const std::vector<std::string>& tags_all,
  const std::vector<std::string>& tags_any,
  const std::vector<std::string>& tags_none,
  std::string* out_node_id
);

bool edge_is_terminal_task_state(const std::string& s);

struct EdgeToolMeta {
  std::string side_effect_level;
  std::unordered_set<std::string> hazards;
  bool has_rate_limit = false;
  int max_per_minute = 0;
  int cooldown_ms = 0;
};

bool edge_tool_meta_from_manifest(
  const Json::Value& manifest,
  const std::string& tool_name,
  EdgeToolMeta* out_meta,
  std::string* out_error
);

bool edge_rate_limit_check_best_effort(
  AgentDb* db,
  const std::string& node_id,
  const std::string& tool_name,
  const EdgeToolMeta& meta,
  int64_t now_utc_ms,
  AgentDb::EdgeToolRateStateRow* out_next_state,
  std::string* out_error
);

void edge_rate_limit_commit_best_effort(
  AgentDb* db,
  const AgentDb::EdgeToolRateStateRow& next_state
);

bool edge_enqueue_task_assign(
  AgentDb* db,
  const std::string& node_id,
  const std::string& task_id,
  const std::string& step_id,
  const std::string& idempotency_key,
  const std::string& mode,
  int64_t deadline_utc_ms,
  int attempt,
  const Json::Value& payload,
  const std::unordered_set<std::string>& allow_hazards,
  bool allow_high_side_effect,
  bool enforce_safety,
  bool enforce_rate_limit,
  int64_t* out_outbox_id,
  bool* out_deduped,
  std::string* out_error,
  int* out_http_status
);

}  // namespace agentd
