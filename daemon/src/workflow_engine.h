#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "openai_client.h"
#include "tool_extension.h"

#include <json/json.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
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
  std::vector<std::thread> workers_;
};

}  // namespace agentd
