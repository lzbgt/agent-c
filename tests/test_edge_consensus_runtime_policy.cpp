#include "edge_consensus_runtime_policy.h"

#include "agent/edge_interop.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

using agentd::DaemonConfig;
using agentd::default_edge_consensus_runtime_kind;
using agentd::default_edge_consensus_runtime_kind_source;
using agentd::edge_consensus_external_runtime_unavailable_reason;
using agentd::edge_consensus_normalize_policy_timing;
using agentd::edge_consensus_runtime_backend_metadata_json;

static void test_default_runtime_kind_auto_builtin() {
  DaemonConfig cfg;
  assert(default_edge_consensus_runtime_kind(cfg) == "builtin");
  assert(default_edge_consensus_runtime_kind_source(cfg) == "auto");

  const Json::Value meta = edge_consensus_runtime_backend_metadata_json(cfg);
  assert(meta["default_runtime_kind"].asString() == "builtin");
  assert(meta["default_runtime_kind_source"].asString() == "auto");
  assert(meta["default_runtime_kind_available"].asBool());
}

static void test_external_default_reports_missing_tool() {
  DaemonConfig cfg;
  cfg.edge_consensus_default_runtime_kind = "external";
  cfg.edge_consensus_default_runtime_kind_from_env = true;

  assert(default_edge_consensus_runtime_kind(cfg) == "external");
  assert(default_edge_consensus_runtime_kind_source(cfg) == "env");
  assert(edge_consensus_external_runtime_unavailable_reason(cfg) ==
         "edge_consensus_node_tool_path not configured");

  const Json::Value meta = edge_consensus_runtime_backend_metadata_json(cfg);
  assert(meta["default_runtime_kind"].asString() == "external");
  assert(meta["default_runtime_kind_source"].asString() == "env");
  assert(!meta["external_available"].asBool());
  assert(!meta["default_runtime_kind_available"].asBool());
  assert(meta["default_runtime_kind_unavailable_reason"].asString() ==
         "edge_consensus_node_tool_path not configured");
}

static void test_external_availability_with_real_tool_path() {
  const std::filesystem::path tool_path =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_tool_" + std::to_string((long long)getpid()));
  {
    std::ofstream out(tool_path);
    out << "#!/bin/sh\nexit 0\n";
  }
#if !defined(_WIN32)
  std::filesystem::permissions(
    tool_path,
    std::filesystem::perms::owner_read |
      std::filesystem::perms::owner_write |
      std::filesystem::perms::owner_exec,
    std::filesystem::perm_options::replace);
#endif

  DaemonConfig cfg;
  cfg.edge_consensus_node_tool_path = tool_path.string();
  cfg.edge_consensus_default_runtime_kind = "external";

  const std::string reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  assert(reason.empty());

  const Json::Value meta = edge_consensus_runtime_backend_metadata_json(cfg);
  assert(meta["external_available"].asBool());
  assert(meta["default_runtime_kind_available"].asBool());
  assert(meta["tool_configured"].asBool());
  assert(meta["node_tool_path_configured"].asBool());
  assert(meta["tool_path"].asString() == tool_path.string());

  std::error_code ec;
  std::filesystem::remove(tool_path, ec);
}

static void test_policy_timing_uses_portable_bounds() {
  agentd::EdgeConsensusClusterPolicy pol;
  pol.campaign_delay_ms = -1;
  pol.campaign_retry_ms = 999999;
  pol.campaign_retry_max_ms = 10;
  pol.campaign_retry_backoff_factor = 99;
  pol.leader_heartbeat_ms = 999999;
  pol.leader_lease_ms = 1;
  pol.lease_expiry_recampaign_delay_ms = 999999;
  pol.stale_runtime_recovery_grace_ms = 999999999;
  edge_consensus_normalize_policy_timing(&pol);
  assert(pol.campaign_delay_ms == 0);
  assert(pol.campaign_retry_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(pol.campaign_retry_max_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(pol.campaign_retry_backoff_factor == AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX);
  assert(pol.leader_heartbeat_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(pol.leader_lease_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(pol.lease_expiry_recampaign_delay_ms == AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS);
  assert(pol.stale_runtime_recovery_grace_ms == AGENT_EDGE_CONSENSUS_POLICY_STALE_RUNTIME_RECOVERY_GRACE_MAX_MS);
  edge_consensus_normalize_policy_timing(nullptr);
}

}  // namespace

int main() {
  test_default_runtime_kind_auto_builtin();
  test_external_default_reports_missing_tool();
  test_external_availability_with_real_tool_path();
  test_policy_timing_uses_portable_bounds();
  return 0;
}
