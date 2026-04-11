#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "openai_client.h"
#include "tool_extension.h"

#include <json/json.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentd {

// Background durable workflow scheduler.
//
// A workflow is a task graph persisted in the agentd SQLite DB (schema v9). The engine:
// - polls for queued/running workflows
// - claims runnable tasks (deps satisfied)
// - executes them using the existing run pipeline
// - persists results and moves the workflow to done/error/cancelled
//
// It is intentionally conservative:
// - no cross-daemon distributed locking (single agentd process owns the DB)
// - at-least-once semantics on restart (running tasks are recovered back to queued)
class WorkflowEngine {
 public:
  struct Options {
    int max_concurrency = 4;         // number of worker threads
    int poll_ms = 200;              // idle poll sleep
    size_t max_scan_workflows = 64;  // per-iteration bound
    int max_inflight_per_workflow = 2; // fairness cap; prevents a single workflow from monopolizing all workers
    int max_inflight_per_session = 0;  // optional multi-tenant cap; 0 disables
    // Explicit fair-queue policy surface.
    //
    // - scan_rr: session-aware scan order (legacy)
    // - wrr: weighted round-robin over session buckets (v2.2)
    // - drr: deficit round-robin over session buckets (v2.3)
    //
    // When all weights are 1, both behave effectively the same.
    std::string fair_queue_policy = "wrr";
    int fair_queue_max_session_weight = 16;
    int fair_queue_max_schedule_len = 1024;
    // DRR cost charging:
    // - unit: each admitted task costs 1 (v2.3 baseline)
    // - simple_v1: best-effort estimated cost by task kind/request
    // - telemetry_v1: best-effort cost derived from last-attempt telemetry stored in workflow_tasks.result_json
    // - budget_pressure_v1: telemetry/simple estimate plus workflow-limit pressure from retry-safe usage totals
    std::string drr_cost_model = "unit";
  };

  WorkflowEngine(
    AgentDb* db,
    std::function<DaemonConfig()> cfg_snapshot,
    std::function<OpenAIClientConfig(const DaemonConfig&)> ocfg_from_cfg,
    const ToolExtension* tool_ext_or_null,
    std::string sessions_root_dir,
    Options opt
  );

  ~WorkflowEngine();

  WorkflowEngine(const WorkflowEngine&) = delete;
  WorkflowEngine& operator=(const WorkflowEngine&) = delete;

  bool start(std::string* out_error);
  void stop();
  bool running() const { return running_.load(); }

 private:
  void worker_main();

  bool pick_and_claim_one(
    int64_t now_unix_ms,
    AgentDb::WorkflowRow* out_wf,
    AgentDb::WorkflowTaskRow* out_task,
    std::string* out_error
  );

  void execute_claimed_task(
    const AgentDb::WorkflowRow& wf,
    const AgentDb::WorkflowTaskRow& task
  );

  void maybe_finalize_workflow(const std::string& workflow_id);

  static std::string json_stringify_compact(const Json::Value& v);

  AgentDb* db_ = nullptr;
  std::function<DaemonConfig()> cfg_snapshot_;
  std::function<OpenAIClientConfig(const DaemonConfig&)> ocfg_from_cfg_;
  const ToolExtension* tool_ext_or_null_ = nullptr;
  std::string sessions_root_dir_;
  Options opt_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> rr_cursor_{0};
  std::mutex fairq_mu_;
  std::unordered_map<std::string, int64_t> drr_deficit_by_session_;
  std::unordered_set<std::string> drr_loaded_sessions_;
  std::vector<std::thread> workers_;
};

}  // namespace agentd
