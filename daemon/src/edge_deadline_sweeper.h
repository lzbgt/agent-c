#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace agentd {

class EdgeDeadlineSweeperEngine {
 public:
  struct Options {
    int poll_ms = 500;
    size_t max_scan_rows = 128;
  };

  EdgeDeadlineSweeperEngine(AgentDb* db, std::function<DaemonConfig()> cfg_snapshot, Options opt);
  ~EdgeDeadlineSweeperEngine();

  EdgeDeadlineSweeperEngine(const EdgeDeadlineSweeperEngine&) = delete;
  EdgeDeadlineSweeperEngine& operator=(const EdgeDeadlineSweeperEngine&) = delete;

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

