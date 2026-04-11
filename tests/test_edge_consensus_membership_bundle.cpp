#include "daemon_config.h"
#include "edge_consensus_membership_bundle.h"
#include "runtime_config.h"

#include "agent/edge_interop.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

static void test_member_normalization_dedupes_safe_ids() {
  const std::vector<std::string> normalized =
    agentd::edge_consensus_normalize_member_node_ids(
      {" node-a ", "node-b", "node-a", "", "bad/node", "node-c", "node-b "});
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

static std::filesystem::path make_temp_db_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".sqlite");
}

static void open_test_db(const std::filesystem::path& path, agentd::AgentDb* out_db) {
  assert(out_db);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::string err;
  const bool ok = out_db->open(path.string(), &err);
  assert(ok);
  (void)err;
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

static void test_runtime_config_load_dedupes_member_lineage_with_portable_matcher() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_config_members");
  agentd::AgentDb db;
  open_test_db(db_path, &db);

  agentd::DaemonConfig saved;
  agentd::EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.previous_membership_epoch = 6;
  pol.member_node_ids = {" node-a ", "node-a", "bad/node", "node-b"};
  pol.previous_member_node_ids = {"node-a", "node-a ", "node-c"};
  pol.membership_lineage.push_back({6, {" node-a ", "node-a", "node-c", "bad/node"}});
  saved.edge_consensus_clusters["cluster-a"] = pol;

  std::string err;
  assert(agentd::save_runtime_config_best_effort(db, saved, &err));
  assert(err.empty());

  agentd::DaemonConfig loaded;
  assert(agentd::load_runtime_config_best_effort(db, &loaded, &err));
  assert(err.empty());
  const auto it = loaded.edge_consensus_clusters.find("cluster-a");
  assert(it != loaded.edge_consensus_clusters.end());
  assert(it->second.member_node_ids.size() == 2);
  assert(it->second.member_node_ids[0] == "node-a");
  assert(it->second.member_node_ids[1] == "node-b");
  assert(it->second.previous_member_node_ids.size() == 2);
  assert(it->second.previous_member_node_ids[0] == "node-a");
  assert(it->second.previous_member_node_ids[1] == "node-c");
  assert(it->second.membership_lineage.size() == 1);
  assert(it->second.membership_lineage[0].member_node_ids.size() == 2);
  assert(it->second.membership_lineage[0].member_node_ids[0] == "node-a");
  assert(it->second.membership_lineage[0].member_node_ids[1] == "node-c");

  std::error_code ec;
  std::filesystem::remove(db_path, ec);
#endif
}

}  // namespace

int main() {
  test_member_normalization_dedupes_safe_ids();
  test_membership_bundle_fields();
  test_membership_bundle_hmac_attestation();
  test_membership_bundle_rejects_missing_policy();
  test_runtime_config_load_dedupes_member_lineage_with_portable_matcher();
  return 0;
}
