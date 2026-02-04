#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace agentd {

class EdgeWorkflowEngine {
 public:
  struct Options {
    int poll_ms = 200;
    size_t max_scan_workflows = 64;
    size_t max_dispatch_per_tick = 64;
  };

  EdgeWorkflowEngine(AgentDb* db, std::function<DaemonConfig()> cfg_snapshot, Options opt);
  ~EdgeWorkflowEngine();

  EdgeWorkflowEngine(const EdgeWorkflowEngine&) = delete;
  EdgeWorkflowEngine& operator=(const EdgeWorkflowEngine&) = delete;

  bool start(std::string* out_error);
  void stop();

 private:
  void worker_main();

  AgentDb* db_ = nullptr;
  std::function<DaemonConfig()> cfg_snapshot_;
  Options opt_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace agentd

