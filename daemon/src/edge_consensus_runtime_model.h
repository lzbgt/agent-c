#pragma once

#include "daemon_config.h"
#include "edge_consensus_runtime_execution.h"

#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#endif

namespace agentd {

struct EdgeConsensusRuntime {
  std::string runtime_kind = "builtin";
  std::string status_source = "memory";
  std::string node_id;
  std::string cluster_id;
  std::string manifest_sha256;
  std::string decision_sha256;
  std::vector<std::string> peer_node_ids;
  std::vector<std::string> member_node_ids;
  std::string daemon_url;
  std::string tool_path;
  std::string model;
  std::string fw_git_sha;
  std::string stderr_log_path;
  int64_t started_unix_ms = 0;
  int64_t ended_unix_ms = 0;
  int64_t campaign_delay_ms = 0;
  int64_t campaign_retry_ms = 0;
  int64_t campaign_retry_max_ms = 0;
  int64_t campaign_retry_backoff_factor = 1;
  int64_t leader_heartbeat_ms = 1000;
  int64_t leader_lease_ms = 5000;
  int64_t lease_expiry_recampaign_delay_ms = 0;
  int64_t poll_interval_ms = 100;
  int64_t deadline_ms = 10000;
  uint64_t cluster_size = 0;
  uint64_t outbox_limit = 128;
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
  uint64_t membership_epoch = 0;
  bool running = false;
  int exit_code = 0;
  int exit_signal = 0;
  std::string last_error;
  std::string last_stdout_line;
  Json::Value last_stdout_json = Json::Value(Json::nullValue);
  Json::Value live_status_json = Json::Value(Json::nullValue);
  std::shared_ptr<std::atomic<bool>> stop_requested;
  std::shared_ptr<std::atomic<bool>> startup_ready;
#if defined(_WIN32)
  intptr_t pid = 0;
#else
  pid_t pid = -1;
#endif
};

Json::Value edge_consensus_trust_epochs_to_json(
  uint64_t trust_roots_epoch,
  uint64_t revocations_epoch,
  uint64_t cert_roots_epoch
);

Json::Value edge_consensus_cluster_policy_to_json(
  const std::string& cluster_id,
  const EdgeConsensusClusterPolicy& pol
);

Json::Value edge_consensus_runtime_to_json(const EdgeConsensusRuntime& st);

Json::Value edge_consensus_runtime_cluster_policy_drift_json(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
);

Json::Value edge_consensus_runtime_trust_epoch_drift_json(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
);

Json::Value edge_consensus_runtime_response_json(
  const DaemonConfig& cfg,
  const EdgeConsensusRuntime& st
);

bool edge_consensus_runtime_same_effective_config(
  const EdgeConsensusRuntime& a,
  const EdgeConsensusRuntime& b
);

bool edge_consensus_runtime_build_config(
  const DaemonConfig& cfg,
  const Json::Value& body,
  EdgeConsensusRuntimeConfig* out_cfg,
  EdgeConsensusRuntime* out_state,
  std::string* out_err
);

std::string edge_consensus_default_local_daemon_url(const DaemonConfig& cfg);

}  // namespace agentd
