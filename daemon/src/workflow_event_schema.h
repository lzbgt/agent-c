#pragma once

#include <string>

namespace agentd {

inline const char* workflow_event_schema_for_type(const std::string& type) {
  if (type == "workflow_created") return "run_event_payload_workflow_created_v1";
  if (type == "workflow_cancel_requested") return "run_event_payload_workflow_cancel_requested_v1";
  if (type == "workflow_status") return "run_event_payload_workflow_status_v1";
  if (type == "workflow_done") return "run_event_payload_workflow_done_v1";
  if (type == "workflow_budget_exceeded") return "run_event_payload_workflow_budget_exceeded_v1";
  if (type == "task_status") return "run_event_payload_task_status_v1";
  if (type == "memory_checkpoint") return "run_event_payload_memory_checkpoint_v1";
  return nullptr;
}

inline const char* edge_workflow_event_schema_for_type(const std::string& type) {
  if (type == "workflow_created") return "run_event_payload_workflow_created_v1";
  if (type == "workflow_canceled") return "run_event_payload_workflow_canceled_v1";
  if (type == "step_state") return "run_event_payload_step_state_v1";
  if (type == "step_retry_scheduled") return "run_event_payload_step_retry_scheduled_v1";
  if (type == "step_dispatched") return "run_event_payload_step_dispatched_v1";
  return nullptr;
}

}  // namespace agentd
