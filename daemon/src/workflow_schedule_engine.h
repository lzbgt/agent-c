#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace agentd {

class WorkflowScheduleEngine {
 public:
  struct Options {
    int poll_ms = 1000;
    size_t max_scan = 64;
  };

  WorkflowScheduleEngine(
    AgentDb* db,
    std::function<DaemonConfig()> cfg_snapshot,
    Options opt
  );
  ~WorkflowScheduleEngine();

  WorkflowScheduleEngine(const WorkflowScheduleEngine&) = delete;
  WorkflowScheduleEngine& operator=(const WorkflowScheduleEngine&) = delete;

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
