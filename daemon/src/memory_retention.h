#pragma once

#include "daemon_config.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace agentd {

struct MemoryRetentionPolicy {
  bool dry_run = false;
  int daily_max_days = 0;
  int64_t daily_max_bytes = 0;
  int checkpoint_max_days = 0;
  int checkpoint_max_count = 0;
  int structured_deprecate_days = 0;
  int structured_deprecate_max_entries = 0;
};

struct MemoryRetentionStats {
  int64_t generated_utc_ms = 0;
  int64_t daily_bytes_before = 0;
  int64_t daily_bytes_after = 0;
  int64_t daily_deleted_count = 0;
  int64_t checkpoint_deleted_count = 0;
  int64_t structured_deprecated_count = 0;
  std::vector<std::string> daily_deleted;
  std::vector<std::string> checkpoint_deleted;
  std::vector<std::string> structured_deprecated_keys;
  std::vector<std::string> errors;
};

bool memory_retention_enforce(
  const DaemonConfig& cfg,
  const MemoryRetentionPolicy& policy,
  MemoryRetentionStats* out_stats,
  std::string* out_error
);

class MemoryRetentionEngine {
 public:
  struct Options {
    int poll_ms = 1000;
  };

  explicit MemoryRetentionEngine(std::function<DaemonConfig()> cfg_snapshot, Options opt);
  ~MemoryRetentionEngine();

  MemoryRetentionEngine(const MemoryRetentionEngine&) = delete;
  MemoryRetentionEngine& operator=(const MemoryRetentionEngine&) = delete;

  bool start(std::string* out_error);
  void stop();

 private:
  void worker_main();

  std::function<DaemonConfig()> cfg_snapshot_;
  Options opt_{};
  std::thread worker_;
  bool running_ = false;
  bool stop_ = false;
};

}  // namespace agentd
