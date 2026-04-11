#include "daemon_config.h"
#include "edge_consensus_membership_bundle.h"

#include "agent/edge_interop.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

static void test_member_normalization_dedupes_safe_ids() {
  const std::vector<std::string> normalized =
    agentd::edge_consensus_normalize_member_node_ids({" node-a ", "node-b", "node-a", "", "bad/node", "node-c"});
  assert(normalized.size() == 3);
  assert(normalized[0] == "node-a");
  assert(normalized[1] == "node-b");
  assert(normalized[2] == "node-c");
}

static agentd::DaemonConfig make_config() {
  agentd::DaemonConfig cfg;
  agentd::EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.previous_membership_epoch = 6;
  pol.updated_utc_ms = 123456;
  pol.member_node_ids = {"node-a", "node-b"};
  pol.previous_member_node_ids = {"node-a", "node-c"};
  pol.membership_lineage.push_back({6, {"node-a", "node-c"}});
  pol.membership_lineage.push_back({5, {"node-a"}});
  pol.campaign_delay_ms = 10;
  pol.campaign_retry_ms = 20;
  pol.campaign_retry_max_ms = 30;
  pol.campaign_retry_backoff_factor = 2;
  pol.leader_heartbeat_ms = 40;
  pol.leader_lease_ms = 50;
  pol.lease_expiry_recampaign_delay_ms = 60;
  pol.stale_runtime_recovery_grace_ms = 70;
  cfg.edge_consensus_clusters["cluster-a"] = pol;
  return cfg;
}

static void test_membership_bundle_fields() {
  const agentd::DaemonConfig cfg = make_config();
  Json::Value bundle;
  std::string err;
  assert(agentd::build_edge_consensus_membership_bundle(cfg, "cluster-a", &bundle, &err));
  assert(err.empty());
  assert(bundle["schema"].asString() == AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1);
  assert(bundle["cluster_id"].asString() == "cluster-a");
  assert(bundle["membership_epoch"].asInt64() == 7);
  assert(bundle["previous_membership_epoch"].asInt64() == 6);
  assert(bundle["updated_utc_ms"].asInt64() == 123456);
  assert(bundle["campaign_delay_ms"].asInt64() == 10);
  assert(bundle["campaign_retry_ms"].asInt64() == 20);
  assert(bundle["campaign_retry_max_ms"].asInt64() == 30);
  assert(bundle["campaign_retry_backoff_factor"].asInt64() == 2);
  assert(bundle["leader_heartbeat_ms"].asInt64() == 40);
  assert(bundle["leader_lease_ms"].asInt64() == 50);
  assert(bundle["lease_expiry_recampaign_delay_ms"].asInt64() == 60);
  assert(bundle["stale_runtime_recovery_grace_ms"].asInt64() == 70);
  assert(bundle["member_node_ids"].size() == 2);
  assert(bundle["member_node_ids"][0].asString() == "node-a");
  assert(bundle["member_node_ids"][1].asString() == "node-b");
  assert(bundle["previous_member_node_ids"].size() == 2);
  assert(bundle["previous_member_node_ids"][0].asString() == "node-a");
  assert(bundle["previous_member_node_ids"][1].asString() == "node-c");
  assert(bundle["membership_lineage"].size() == 2);
  assert(bundle["membership_lineage"][0]["membership_epoch"].asInt64() == 6);
  assert(bundle["membership_lineage"][0]["member_node_ids"].size() == 2);
  assert(bundle["membership_lineage"][1]["membership_epoch"].asInt64() == 5);
  assert(bundle["membership_lineage"][1]["member_node_ids"].size() == 1);
  assert(!bundle.isMember("attest"));
}

static void test_membership_bundle_hmac_attestation() {
  agentd::DaemonConfig cfg = make_config();
  cfg.run_attest_hmac_kid = "kid-a";
  cfg.run_attest_hmac_key = "secret";
  Json::Value bundle;
  std::string err;
  assert(agentd::build_edge_consensus_membership_bundle(cfg, "cluster-a", &bundle, &err));
  assert(err.empty());
  const Json::Value att = bundle["attest"];
  assert(att["schema"].asString() == AGENT_EDGE_CONSENSUS_MEMBERSHIP_ATTEST_SCHEMA_V1);
  assert(att["alg"].asString() == "hmac-sha256");
  assert(att["kid"].asString() == "kid-a");
  assert(att["cluster_id"].asString() == "cluster-a");
  assert(att["membership_epoch"].asInt64() == 7);
  assert(att["previous_membership_epoch"].asInt64() == 6);
  assert(!att["sig"].asString().empty());
}

static void test_membership_bundle_rejects_missing_policy() {
  const agentd::DaemonConfig cfg;
  Json::Value bundle;
  std::string err;
  assert(!agentd::build_edge_consensus_membership_bundle(cfg, "cluster-a", &bundle, &err));
  assert(err == "consensus_membership_unavailable");
}

}  // namespace

int main() {
  test_member_normalization_dedupes_safe_ids();
  test_membership_bundle_fields();
  test_membership_bundle_hmac_attestation();
  test_membership_bundle_rejects_missing_policy();
  return 0;
}
