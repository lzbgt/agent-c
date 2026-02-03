#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "openai_client.h"
#include "tool_extension.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace agentd {

// Background resumable runner for async jobs submitted via /api/v1/run_async.
//
// Key goal: job continuity across daemon restarts.
// - Jobs persist their request JSON (redacted; api_key removed) in SQLite.
// - On restart, inflight jobs are recovered back to queued and resumed.
//
// Semantics:
// - at-least-once execution after restart (a job may run twice if the daemon dies mid-request)
// - cancellation is best-effort:
//   - queued jobs with cancel_requested=1 are finalized as cancelled
//   - running jobs rely on cooperative cancellation via job_manager hooks
class JobEngine {
 public:
  struct Options {
    int max_concurrency = 2;
    int poll_ms = 200;
    size_t max_scan_jobs = 64;
  };

  JobEngine(
    AgentDb* db,
    std::function<DaemonConfig()> cfg_snapshot,
    std::function<OpenAIClientConfig(const DaemonConfig&)> ocfg_from_cfg,
    const ToolExtension* tool_ext_or_null,
    std::string sessions_root_dir,
    Options opt
  );

  ~JobEngine();

  JobEngine(const JobEngine&) = delete;
  JobEngine& operator=(const JobEngine&) = delete;

  bool start(std::string* out_error);
  void stop();

 private:
  void worker_main();
  bool claim_one(int64_t now_unix_ms, AgentDb::JobRow* out_job);
  void execute_job(const AgentDb::JobRow& job);
  void recover_inflight_jobs_best_effort();

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

