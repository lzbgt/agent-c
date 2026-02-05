#pragma once

#include "agent_db.h"

#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

namespace agentd {
namespace workflow_engine_internal {

int64_t unix_ms_now();

bool json_parse_any_value(const std::string& s, Json::Value* out, std::string* out_err);
std::vector<std::string> parse_dep_ids(const std::string& deps_json);
std::string json_get_string(const Json::Value& obj, const char* k);
std::string json_stringify_compact_local(const Json::Value& v);

bool workflow_is_terminal_status(const std::string& s);

std::string workflow_task_kind_best_effort(const AgentDb::WorkflowTaskRow& t);
int64_t workflow_deadline_unix_ms_best_effort(const AgentDb::WorkflowRow& wf);
int workflow_session_weight_best_effort(const AgentDb::WorkflowRow& wf, int max_weight);

struct WorkflowLimits {
  int64_t max_tool_calls_total = 0;  // 0 disables (unlimited).
  int64_t max_steps_total = 0;       // 0 disables (unlimited).
  int64_t max_elapsed_ms_total = 0;  // 0 disables (unlimited). Best-effort wall time summed across tasks.
  int64_t max_total_tokens = 0;      // 0 disables (unlimited). Best-effort provider-reported tokens (usage.total_tokens).
};

WorkflowLimits workflow_limits_best_effort(const AgentDb::WorkflowRow& wf);

enum class WorkflowCancelReason {
  None = 0,
  CancelRequested = 1,
  DeadlineExceeded = 2,
};

struct WorkflowRunCancelCtx {
  AgentDb* db = nullptr;
  std::string workflow_id;
  int64_t deadline_unix_ms = 0;
  int64_t last_check_unix_ms = 0;
  WorkflowCancelReason reason = WorkflowCancelReason::None;
};

bool workflow_run_should_cancel(void* vctx);

void insert_workflow_event_best_effort(
  AgentDb* db,
  const std::string& workflow_id,
  const std::string& task_id,
  const std::string& type,
  int64_t ts_unix_ms,
  const Json::Value& data
);

bool apply_expectations(const std::string& expect_json, const Json::Value& run_out, std::string* out_err);

int64_t retry_backoff_ms(int attempt);

}  // namespace workflow_engine_internal
}  // namespace agentd

