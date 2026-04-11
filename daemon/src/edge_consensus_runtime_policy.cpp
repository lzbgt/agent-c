#include "edge_consensus_runtime_policy.h"

#include "edge_consensus_runtime_model.h"
#include "string_util.h"

#include "agent/edge_interop.h"

#include <filesystem>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace agentd {
namespace {

static bool is_safe_shellish_token(const std::string& s_in, size_t max_len) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > max_len) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string configured_default_edge_consensus_runtime_kind(const DaemonConfig& cfg) {
  const std::string kind = lower_copy(trim_copy(cfg.edge_consensus_default_runtime_kind));
  return (kind == "builtin" || kind == "external") ? kind : std::string();
}

}  // namespace

std::string edge_consensus_external_runtime_unavailable_reason(const DaemonConfig& cfg) {
  const std::string tool_path = trim_copy(cfg.edge_consensus_node_tool_path);
  if (tool_path.empty()) return "edge_consensus_node_tool_path not configured";
  if (!std::filesystem::exists(std::filesystem::path(tool_path))) {
    return "edge_consensus_node_tool_path not found";
  }
  if (!is_safe_shellish_token(tool_path, 512)) return "invalid edge_consensus_node_tool_path";
#if !defined(_WIN32)
  if (::access(tool_path.c_str(), X_OK) != 0) {
    return std::string("edge_consensus_node_tool_path not executable: ") + std::strerror(errno);
  }
#endif
  return "";
}

std::string default_edge_consensus_runtime_kind_source(const DaemonConfig& cfg) {
  if (configured_default_edge_consensus_runtime_kind(cfg).empty()) return "auto";
  return cfg.edge_consensus_default_runtime_kind_from_env ? "env" : "config";
}

std::string default_edge_consensus_runtime_kind(const DaemonConfig& cfg) {
  const std::string configured = configured_default_edge_consensus_runtime_kind(cfg);
  return configured.empty() ? "builtin" : configured;
}

void edge_consensus_normalize_policy_timing(EdgeConsensusClusterPolicy* pol) {
  if (!pol) return;
  agent_edge_consensus_policy_timing_t timing;
  timing.campaign_delay_ms = pol->campaign_delay_ms;
  timing.campaign_retry_ms = pol->campaign_retry_ms;
  timing.campaign_retry_max_ms = pol->campaign_retry_max_ms;
  timing.campaign_retry_backoff_factor = pol->campaign_retry_backoff_factor;
  timing.leader_heartbeat_ms = pol->leader_heartbeat_ms;
  timing.leader_lease_ms = pol->leader_lease_ms;
  timing.lease_expiry_recampaign_delay_ms = pol->lease_expiry_recampaign_delay_ms;
  timing.stale_runtime_recovery_grace_ms = pol->stale_runtime_recovery_grace_ms;
  if (agent_edge_consensus_policy_timing_normalize(&timing) != AGENT_OK) return;
  pol->campaign_delay_ms = timing.campaign_delay_ms;
  pol->campaign_retry_ms = timing.campaign_retry_ms;
  pol->campaign_retry_max_ms = timing.campaign_retry_max_ms;
  pol->campaign_retry_backoff_factor = timing.campaign_retry_backoff_factor;
  pol->leader_heartbeat_ms = timing.leader_heartbeat_ms;
  pol->leader_lease_ms = timing.leader_lease_ms;
  pol->lease_expiry_recampaign_delay_ms = timing.lease_expiry_recampaign_delay_ms;
  pol->stale_runtime_recovery_grace_ms = timing.stale_runtime_recovery_grace_ms;
}

Json::Value edge_consensus_runtime_backend_metadata_json(const DaemonConfig& cfg) {
  Json::Value out(Json::objectValue);
  out["builtin_available"] = true;
  const std::string default_runtime_kind = default_edge_consensus_runtime_kind(cfg);
  out["default_runtime_kind"] = default_runtime_kind;
  out["default_runtime_kind_source"] = default_edge_consensus_runtime_kind_source(cfg);
  out["tool_configured"] = !trim_copy(cfg.edge_consensus_node_tool_path).empty();
  out["node_tool_path_configured"] = !trim_copy(cfg.edge_consensus_node_tool_path).empty();
  if (!cfg.edge_consensus_node_tool_path.empty()) out["tool_path"] = cfg.edge_consensus_node_tool_path;
  out["default_daemon_url"] = edge_consensus_default_local_daemon_url(cfg);
  const std::string external_reason = edge_consensus_external_runtime_unavailable_reason(cfg);
  out["external_available"] = external_reason.empty();
  const std::string default_reason =
    default_runtime_kind == "external" ? external_reason : std::string();
  out["default_runtime_kind_available"] = default_reason.empty();
  if (!external_reason.empty()) out["external_unavailable_reason"] = external_reason;
  if (!default_reason.empty()) out["default_runtime_kind_unavailable_reason"] = default_reason;
  return out;
}

}  // namespace agentd
