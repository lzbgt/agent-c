#pragma once

#include <json/json.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace agentd {

struct EdgeConsensusHttpRuntimeConfig {
  std::string daemon_url;
  std::string auth_token;
  std::string node_id;
  std::string cluster_id;
  std::string manifest_sha256;
  std::string model = "edge_consensus_node";
  std::string fw_git_sha = "consensus-node";
  std::string decision_sha256;
  std::vector<std::string> peer_node_ids;
  std::vector<std::string> member_node_ids;
  size_t cluster_size = 0;
  size_t outbox_limit = 128;
  int64_t campaign_delay_ms = 0;
  int64_t campaign_retry_ms = 1500;
  int64_t campaign_retry_max_ms = 1500;
  int64_t campaign_retry_backoff_factor = 1;
  int64_t leader_heartbeat_ms = 1000;
  int64_t leader_lease_ms = 5000;
  int64_t poll_interval_ms = 100;
  int64_t deadline_ms = 10000;
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
  uint64_t membership_epoch = 0;
};

struct EdgeConsensusHttpRuntimeHooks {
  std::atomic<bool>* stop_requested = nullptr;
  std::function<void(const std::string&)> log_line;
  std::function<void(const Json::Value&)> status_update;
  std::function<void()> startup_ready;
};

// Runs the poll/process/post consensus loop against agentd HTTP surfaces.
//
// Returns true when a structured final result JSON was produced in `out_result`.
// That includes successful commit, deadline expiry, or graceful stop.
// Returns false only for internal transport / parse failures, in which case
// `out_error` contains a human-readable error.
bool run_edge_consensus_http_runtime(
  const EdgeConsensusHttpRuntimeConfig& cfg,
  const EdgeConsensusHttpRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
);

}  // namespace agentd
