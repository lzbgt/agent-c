#include "edge_consensus_runtime_model.h"

#include "agent/edge_interop.h"

#include <cassert>
#include <string>

namespace {

using agentd::DaemonConfig;
using agentd::EdgeConsensusClusterPolicy;
using agentd::EdgeConsensusRuntimeConfig;
using agentd::EdgeConsensusRuntime;
using agentd::edge_consensus_runtime_build_config;
using agentd::edge_consensus_runtime_membership_epoch_recoverable;
using agentd::edge_consensus_runtime_response_json;
using agentd::edge_consensus_runtime_same_effective_config;

static DaemonConfig make_cfg() {
  DaemonConfig cfg;
  cfg.listen_host = "0.0.0.0";
  cfg.listen_port = 8080;
  cfg.auth_token = "daemon-token";
  cfg.edge_auth_trust_roots_epoch = 11;
  cfg.edge_auth_revocations_epoch = 12;
  cfg.edge_auth_cert_roots_epoch = 13;

  EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.previous_membership_epoch = 6;
  pol.member_node_ids = {"node-b", "node-a", "node-a"};
  pol.previous_member_node_ids = {"node-b", "node-c"};
  pol.membership_lineage.push_back({6, {"node-b", "node-c"}});
  pol.membership_lineage.push_back({5, {"node-b"}});
  pol.campaign_retry_ms = 1500;
  pol.campaign_retry_max_ms = 2500;
  pol.campaign_retry_backoff_factor = 2;
  pol.leader_heartbeat_ms = 1000;
  pol.leader_lease_ms = 5000;
  pol.lease_expiry_recampaign_delay_ms = 333;
  pol.stale_runtime_recovery_grace_ms = 4444;
  cfg.edge_consensus_clusters["cluster-a"] = pol;
  return cfg;
}

static void test_build_config_defaults_policy_and_trust_epochs() {
  const DaemonConfig cfg = make_cfg();
  Json::Value body(Json::objectValue);
  body["node_id"] = "node-a";
  body["cluster_id"] = "cluster-a";
  body["manifest_sha256"] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  EdgeConsensusRuntimeConfig run_cfg;
  EdgeConsensusRuntime st;
  std::string err;
  const bool ok = edge_consensus_runtime_build_config(cfg, body, &run_cfg, &st, &err);
  assert(ok);
  assert(err.empty());

  assert(run_cfg.daemon_url == "http://127.0.0.1:8080");
  assert(run_cfg.auth_token == "daemon-token");
  assert(run_cfg.member_node_ids.size() == 2);
  assert(run_cfg.member_node_ids[0] == "node-b");
  assert(run_cfg.member_node_ids[1] == "node-a");
  assert(run_cfg.peer_node_ids.size() == 1);
  assert(run_cfg.peer_node_ids[0] == "node-b");
  assert(run_cfg.cluster_size == 2);
  assert(run_cfg.trust_roots_epoch == 11);
  assert(run_cfg.revocations_epoch == 12);
  assert(run_cfg.cert_roots_epoch == 13);
  assert(run_cfg.membership_epoch == 7);
  assert(run_cfg.lease_expiry_recampaign_delay_ms == 333);
  assert(run_cfg.stale_runtime_recovery_grace_ms == 4444);

  assert(st.daemon_url == run_cfg.daemon_url);
  assert(st.member_node_ids == run_cfg.member_node_ids);
  assert(st.peer_node_ids == run_cfg.peer_node_ids);
  assert(st.cluster_size == run_cfg.cluster_size);
  assert(st.trust_roots_epoch == run_cfg.trust_roots_epoch);
  assert(st.revocations_epoch == run_cfg.revocations_epoch);
  assert(st.cert_roots_epoch == run_cfg.cert_roots_epoch);
  assert(st.membership_epoch == run_cfg.membership_epoch);
  assert(st.lease_expiry_recampaign_delay_ms == run_cfg.lease_expiry_recampaign_delay_ms);
  assert(st.stale_runtime_recovery_grace_ms == run_cfg.stale_runtime_recovery_grace_ms);
  assert(st.running);
}

static void test_build_config_dedupes_explicit_node_sets() {
  const DaemonConfig cfg = make_cfg();
  Json::Value body(Json::objectValue);
  body["node_id"] = "node-a";
  body["cluster_id"] = "cluster-a";
  body["manifest_sha256"] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  body["peer_node_ids"].append("node-a");
  body["peer_node_ids"].append("node-b");
  body["peer_node_ids"].append("node-b");
  body["member_node_ids"].append("node-b");
  body["member_node_ids"].append("node-a");
  body["member_node_ids"].append("node-b");

  EdgeConsensusRuntimeConfig run_cfg;
  EdgeConsensusRuntime st;
  std::string err;
  const bool ok = edge_consensus_runtime_build_config(cfg, body, &run_cfg, &st, &err);
  assert(ok);
  assert(err.empty());
  assert(run_cfg.peer_node_ids.size() == 1);
  assert(run_cfg.peer_node_ids[0] == "node-b");
  assert(run_cfg.member_node_ids.size() == 2);
  assert(run_cfg.member_node_ids[0] == "node-b");
  assert(run_cfg.member_node_ids[1] == "node-a");
  assert(st.peer_node_ids == run_cfg.peer_node_ids);
  assert(st.member_node_ids == run_cfg.member_node_ids);
}

static void test_build_config_uses_portable_policy_timing_bounds() {
  const DaemonConfig cfg = make_cfg();
  Json::Value body(Json::objectValue);
  body["node_id"] = "node-a";
  body["cluster_id"] = "cluster-a";
  body["manifest_sha256"] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  body["campaign_delay_ms"] = -7;
  body["campaign_retry_ms"] = 999999;
  body["campaign_retry_max_ms"] = 1;
  body["campaign_retry_backoff_factor"] = 99;
  body["leader_heartbeat_ms"] = 999999;
  body["leader_lease_ms"] = 1;
  body["lease_expiry_recampaign_delay_ms"] = 999999;
  body["stale_runtime_recovery_grace_ms"] = 999999999;

  EdgeConsensusRuntimeConfig run_cfg;
  EdgeConsensusRuntime st;
  std::string err;
  assert(edge_consensus_runtime_build_config(cfg, body, &run_cfg, &st, &err));
  assert(run_cfg.campaign_delay_ms == 0);
  assert(run_cfg.campaign_retry_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(run_cfg.campaign_retry_max_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(run_cfg.campaign_retry_backoff_factor == AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX);
  assert(run_cfg.leader_heartbeat_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(run_cfg.leader_lease_ms == AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  assert(run_cfg.lease_expiry_recampaign_delay_ms == AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS);
  assert(run_cfg.stale_runtime_recovery_grace_ms ==
         AGENT_EDGE_CONSENSUS_POLICY_STALE_RUNTIME_RECOVERY_GRACE_MAX_MS);
}

static void test_same_effective_config_ignores_builtin_daemon_url_drift_only() {
  EdgeConsensusRuntime builtin_a;
  builtin_a.runtime_kind = "builtin";
  builtin_a.node_id = "node-a";
  builtin_a.cluster_id = "cluster-a";
  builtin_a.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  builtin_a.daemon_url = "@local";

  EdgeConsensusRuntime builtin_b = builtin_a;
  builtin_b.daemon_url = "http://127.0.0.1:8123";

  assert(edge_consensus_runtime_same_effective_config(builtin_a, builtin_b));

  EdgeConsensusRuntime external_a = builtin_a;
  external_a.runtime_kind = "external";
  external_a.daemon_url = "http://127.0.0.1:8123";

  EdgeConsensusRuntime external_b = external_a;
  external_b.daemon_url = "http://127.0.0.1:8124";

  assert(!edge_consensus_runtime_same_effective_config(external_a, external_b));
}

static void test_response_json_surfaces_cluster_and_trust_drift() {
  const DaemonConfig cfg = make_cfg();
  EdgeConsensusRuntime st;
  st.runtime_kind = "builtin";
  st.node_id = "node-a";
  st.cluster_id = "cluster-a";
  st.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  st.member_node_ids = {"node-a", "node-c"};
  st.trust_roots_epoch = 21;
  st.revocations_epoch = 22;
  st.cert_roots_epoch = 23;
  st.membership_epoch = 9;
  st.lease_expiry_recampaign_delay_ms = 444;
  st.stale_runtime_recovery_grace_ms = 5555;

  const Json::Value out = edge_consensus_runtime_response_json(cfg, st);
  assert(out.isObject());
  assert(out.isMember("cluster_policy_drift"));
  assert(out["cluster_policy_drift"]["cluster_id"].asString() == "cluster-a");
  assert(out["cluster_policy_drift"]["current_policy"]["lease_expiry_recampaign_delay_ms"]
           .asInt64() == 333);
  assert(out["cluster_policy_drift"]["current_policy"]["stale_runtime_recovery_grace_ms"]
           .asInt64() == 4444);
  assert(out["cluster_policy_drift"]["current_policy"]["previous_membership_epoch"].asInt64() == 6);
  assert(out["cluster_policy_drift"]["current_policy"]["previous_member_node_ids"].size() == 2);
  assert(out["cluster_policy_drift"]["current_policy"]["previous_member_node_ids"][0].asString() == "node-b");
  assert(out["cluster_policy_drift"]["current_policy"]["previous_member_node_ids"][1].asString() == "node-c");
  assert(out["cluster_policy_drift"]["current_policy"]["membership_lineage"].size() == 2);
  assert(out["cluster_policy_drift"]["current_policy"]["membership_lineage"][0]["membership_epoch"].asInt64() == 6);
  assert(out["cluster_policy_drift"]["current_policy"]["membership_lineage"][1]["membership_epoch"].asInt64() == 5);

  const Json::Value changed = out["cluster_policy_drift"]["changed_fields"];
  assert(changed.isArray());
  bool saw_membership_epoch = false;
  bool saw_member_node_ids = false;
  bool saw_lease_delay = false;
  bool saw_stale_recovery_grace = false;
  for (Json::ArrayIndex i = 0; i < changed.size(); i++) {
    const std::string field = changed[i].asString();
    if (field == "membership_epoch") saw_membership_epoch = true;
    if (field == "member_node_ids") saw_member_node_ids = true;
    if (field == "lease_expiry_recampaign_delay_ms") saw_lease_delay = true;
    if (field == "stale_runtime_recovery_grace_ms") saw_stale_recovery_grace = true;
  }
  assert(saw_membership_epoch);
  assert(saw_member_node_ids);
  assert(saw_lease_delay);
  assert(saw_stale_recovery_grace);

  assert(out.isMember("trust_epoch_drift"));
  assert(out["trust_epoch_drift"]["current_trust_epochs"]["trust_roots_epoch"].asUInt64() == 11);
  assert(out["trust_epoch_drift"]["current_trust_epochs"]["revocations_epoch"].asUInt64() == 12);
  assert(out["trust_epoch_drift"]["current_trust_epochs"]["cert_roots_epoch"].asUInt64() == 13);
}

static void test_membership_epoch_recovery_requires_matching_member_set() {
  const DaemonConfig cfg = make_cfg();
  EdgeConsensusRuntime st;
  st.runtime_kind = "builtin";
  st.node_id = "node-b";
  st.cluster_id = "cluster-a";
  st.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  Json::Value policy(Json::objectValue);
  st.membership_epoch = 5;
  assert(edge_consensus_runtime_membership_epoch_recoverable(cfg, st, &policy));
  assert(policy["reason"].asString() == "membership_lineage");
  assert(policy["runtime_node_in_matching_member_set"].asBool());

  st.node_id = "node-a";
  policy = Json::Value(Json::objectValue);
  assert(!edge_consensus_runtime_membership_epoch_recoverable(cfg, st, &policy));
  assert(policy["reason"].asString() == "membership_lineage_node_not_member");
  assert(policy["matches_lineage_epoch"].asBool());
  assert(!policy["runtime_node_in_matching_member_set"].asBool());

  st.node_id = "node-c";
  st.membership_epoch = 6;
  policy = Json::Value(Json::objectValue);
  assert(edge_consensus_runtime_membership_epoch_recoverable(cfg, st, &policy));
  assert(policy["reason"].asString() == "previous_membership_epoch");
  assert(policy["runtime_node_in_matching_member_set"].asBool());

  st.node_id = "node-a";
  policy = Json::Value(Json::objectValue);
  assert(!edge_consensus_runtime_membership_epoch_recoverable(cfg, st, &policy));
  assert(policy["reason"].asString() == "previous_membership_epoch_node_not_member");

  st.membership_epoch = 7;
  policy = Json::Value(Json::objectValue);
  assert(edge_consensus_runtime_membership_epoch_recoverable(cfg, st, &policy));
  assert(policy["reason"].asString() == "current_membership_epoch");

  st.node_id = "node-c";
  policy = Json::Value(Json::objectValue);
  assert(!edge_consensus_runtime_membership_epoch_recoverable(cfg, st, &policy));
  assert(policy["reason"].asString() == "current_membership_epoch_node_not_member");
}

}  // namespace

int main() {
  test_build_config_defaults_policy_and_trust_epochs();
  test_build_config_dedupes_explicit_node_sets();
  test_build_config_uses_portable_policy_timing_bounds();
  test_same_effective_config_ignores_builtin_daemon_url_drift_only();
  test_response_json_surfaces_cluster_and_trust_drift();
  test_membership_epoch_recovery_requires_matching_member_set();
  return 0;
}
