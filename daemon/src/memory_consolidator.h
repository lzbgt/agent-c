#pragma once

#include "daemon_config.h"

#include <json/json.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace agentd {

struct MemoryConsolidateOptions {
  int daily_days = 14;
  int keep_checkpoints = 100;
  int max_entries = 256;
  int max_file_bytes = 1024 * 1024;
  bool dry_run = false;
};

// Deterministic consolidation:
// - scans recent daily memory files for explicit "@mem" markers
// - promotes them into structured memory (memory/STRUCTURED.md) via memory_put(entries)
//
// Returns true on success and sets out_report.
bool memory_consolidate_once(
  const DaemonConfig& cfg,
  const MemoryConsolidateOptions& opt,
  Json::Value* out_report,
  std::string* out_err
);

class MemoryConsolidatorEngine {
 public:
  struct Options {
    int poll_ms = 1000;
  };

  explicit MemoryConsolidatorEngine(std::function<DaemonConfig()> cfg_snapshot, Options opt);
  ~MemoryConsolidatorEngine();

  MemoryConsolidatorEngine(const MemoryConsolidatorEngine&) = delete;
  MemoryConsolidatorEngine& operator=(const MemoryConsolidatorEngine&) = delete;

  bool start(std::string* out_error);
  void stop();

 private:
  void worker_main();

  std::function<DaemonConfig()> cfg_snapshot_;
  Options opt_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace agentd

