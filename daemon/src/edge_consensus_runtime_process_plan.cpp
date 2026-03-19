#include "edge_consensus_runtime_process_plan.h"

#include "string_util.h"

#include <filesystem>

namespace agentd {

EdgeConsensusRuntimeArtifactsPlan plan_edge_consensus_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& node_id
) {
  std::error_code ec;
  std::filesystem::path base =
    cfg.state_dir.empty() ? std::filesystem::temp_directory_path(ec) : std::filesystem::path(cfg.state_dir);
  if (ec) base = std::filesystem::path(".");

  EdgeConsensusRuntimeArtifactsPlan plan;
  plan.runtime_dir = (base / "edge_consensus_runtimes" / trim_copy(node_id)).string();
  plan.stderr_log_path = (std::filesystem::path(plan.runtime_dir) / "stderr.log").string();
  return plan;
}

EdgeConsensusExternalProcessPlan make_edge_consensus_external_process_plan(
  const DaemonConfig& cfg,
  const EdgeConsensusHttpRuntimeConfig& run_cfg,
  const EdgeConsensusRuntime& runtime_state
) {
  EdgeConsensusExternalProcessPlan plan;
  plan.artifacts = plan_edge_consensus_runtime_artifacts(cfg, runtime_state.node_id);
  plan.tool_path = trim_copy(cfg.edge_consensus_node_tool_path);

  plan.argv.push_back(plan.tool_path);
  plan.argv.push_back("--daemon-url");
  plan.argv.push_back(run_cfg.daemon_url);
  plan.argv.push_back("--node-id");
  plan.argv.push_back(runtime_state.node_id);
  plan.argv.push_back("--cluster-id");
  plan.argv.push_back(runtime_state.cluster_id);
  plan.argv.push_back("--manifest-sha256");
  plan.argv.push_back(runtime_state.manifest_sha256);
  plan.argv.push_back("--cluster-size");
  plan.argv.push_back(std::to_string((unsigned long long)runtime_state.cluster_size));
  plan.argv.push_back("--outbox-limit");
  plan.argv.push_back(std::to_string((unsigned long long)runtime_state.outbox_limit));
  plan.argv.push_back("--campaign-delay-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.campaign_delay_ms));
  plan.argv.push_back("--campaign-retry-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.campaign_retry_ms));
  plan.argv.push_back("--campaign-retry-max-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.campaign_retry_max_ms));
  plan.argv.push_back("--campaign-retry-backoff-factor");
  plan.argv.push_back(std::to_string((long long)runtime_state.campaign_retry_backoff_factor));
  plan.argv.push_back("--leader-heartbeat-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.leader_heartbeat_ms));
  plan.argv.push_back("--leader-lease-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.leader_lease_ms));
  plan.argv.push_back("--lease-expiry-recampaign-delay-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.lease_expiry_recampaign_delay_ms));
  plan.argv.push_back("--poll-interval-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.poll_interval_ms));
  plan.argv.push_back("--deadline-ms");
  plan.argv.push_back(std::to_string((long long)runtime_state.deadline_ms));
  plan.argv.push_back("--trust-roots-epoch");
  plan.argv.push_back(std::to_string((unsigned long long)runtime_state.trust_roots_epoch));
  plan.argv.push_back("--revocations-epoch");
  plan.argv.push_back(std::to_string((unsigned long long)runtime_state.revocations_epoch));
  plan.argv.push_back("--cert-roots-epoch");
  plan.argv.push_back(std::to_string((unsigned long long)runtime_state.cert_roots_epoch));
  plan.argv.push_back("--membership-epoch");
  plan.argv.push_back(std::to_string((unsigned long long)runtime_state.membership_epoch));
  plan.argv.push_back("--model");
  plan.argv.push_back(runtime_state.model);
  plan.argv.push_back("--fw-git-sha");
  plan.argv.push_back(runtime_state.fw_git_sha);
  if (!runtime_state.decision_sha256.empty()) {
    plan.argv.push_back("--decision-sha256");
    plan.argv.push_back(runtime_state.decision_sha256);
  }
  if (!run_cfg.auth_token.empty()) {
    plan.argv.push_back("--auth-token");
    plan.argv.push_back(run_cfg.auth_token);
  }
  for (const auto& peer : runtime_state.peer_node_ids) {
    plan.argv.push_back("--peer-node-id");
    plan.argv.push_back(peer);
  }
  for (const auto& member : runtime_state.member_node_ids) {
    plan.argv.push_back("--member-node-id");
    plan.argv.push_back(member);
  }
  return plan;
}

bool edge_consensus_runtime_stdout_event_is_startup_ready(const Json::Value& parsed) {
  return
    parsed.isObject() &&
    parsed.isMember("schema") &&
    parsed["schema"].isString() &&
    trim_copy(parsed["schema"].asString()) == "edge_node_consensus_runtime_event_v1" &&
    parsed.isMember("event") &&
    parsed["event"].isString() &&
    trim_copy(parsed["event"].asString()) == "startup_ready";
}

bool edge_consensus_runtime_stdout_event_live_status(
  const Json::Value& parsed,
  Json::Value* out_status
) {
  if (out_status) *out_status = Json::Value(Json::nullValue);
  if (!parsed.isObject()) return false;
  if (!parsed.isMember("schema") || !parsed["schema"].isString()) return false;
  if (trim_copy(parsed["schema"].asString()) != "edge_node_consensus_live_status_v1") return false;
  if (!parsed.isMember("status") || !parsed["status"].isObject()) return false;
  if (out_status) *out_status = parsed["status"];
  return true;
}

}  // namespace agentd
