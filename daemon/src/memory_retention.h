#pragma once

#include "daemon_config.h"
#include "memory_store.h"
#include <atomic>
#include <json/json.h>

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace agentd {

struct MemoryRetentionPolicy {
  bool dry_run = false;
  // Host-only lifecycle/qualification hooks; never accepted as JSON options.
  std::function<bool()> cancelled;
  std::function<void(const std::string &)> before_retire;
  std::function<void(host_tools_internal::MemoryPublishPhase)> publish_observer;
  int daily_max_days = 0;
  int64_t daily_max_bytes = 0;
  int checkpoint_max_days = 0;
  int checkpoint_max_count = 0;
  int structured_deprecate_days = 0;
  int structured_deprecate_max_entries = 0;
};

struct MemoryRetentionStats {
  int64_t generated_utc_ms = 0;
  bool complete = true, cancelled = false, budget_exhausted = false;
  int64_t daily_selected_count = 0, checkpoint_selected_count = 0,
          structured_selected_count = 0;
  int64_t deferred_count = 0, errors_dropped = 0;
  Json::Value outcomes{Json::arrayValue}, structured_write_response;
  int64_t daily_bytes_before = 0;
  int64_t daily_bytes_after = 0, daily_bytes_projected = 0;
  int64_t daily_deleted_count = 0;
  int64_t checkpoint_deleted_count = 0;
  int64_t structured_deprecated_count = 0;
  std::vector<std::string> daily_deleted;
  std::vector<std::string> checkpoint_deleted;
  std::vector<std::string> structured_deprecated_keys, structured_selected_keys;
  std::vector<std::string> errors;
};

Json::Value memory_retention_report(const MemoryRetentionPolicy &policy,
                                    const MemoryRetentionStats &stats,
                                    const std::string &error);

bool memory_retention_enforce(const DaemonConfig &cfg,
                              const MemoryRetentionPolicy &policy,
                              MemoryRetentionStats *out_stats,
                              std::string *out_error);

class MemoryRetentionEngine {
public:
  struct Options {
    int poll_ms = 1000;
  };

  explicit MemoryRetentionEngine(std::function<DaemonConfig()> cfg_snapshot,
                                 Options opt);
  ~MemoryRetentionEngine();

  MemoryRetentionEngine(const MemoryRetentionEngine &) = delete;
  MemoryRetentionEngine &operator=(const MemoryRetentionEngine &) = delete;

  bool start(std::string *out_error);
  void stop();

private:
  void worker_main();

  std::function<DaemonConfig()> cfg_snapshot_;
  Options opt_{};
  std::thread worker_;
  std::atomic<bool> running_{false}, stop_{false};
};

} // namespace agentd
