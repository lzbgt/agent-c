#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <string>
#include <vector>

namespace agentd {

struct BlobTierPolicy {
  bool dry_run = false;
  bool force_evict_all = false;
  int64_t local_max_bytes = 0;
  int64_t local_max_age_ms = 0;
  int64_t promote_after_ms = 0;
  int64_t promote_max_bytes = 0;
  size_t max_rows = 5000;
};

struct BlobTierEnforceStats {
  int64_t generated_utc_ms = 0;
  int64_t promoted_count = 0;
  int64_t promoted_bytes = 0;
  int64_t evicted_count = 0;
  int64_t evicted_bytes = 0;
  int64_t total_local_bytes_before = 0;
  int64_t total_local_bytes_after = 0;
  std::vector<std::string> errors;
};

bool blob_tier_enforce(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  const BlobTierPolicy& policy,
  BlobTierEnforceStats* out_stats,
  std::string* out_error
);

}  // namespace agentd
