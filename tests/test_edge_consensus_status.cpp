#include "agent_db.h"
#include "edge_consensus_status.h"
#include "edge_node_consensus.h"
#include "json_util.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using agentd::AgentDb;
using agentd::EdgeConsensusFrame;
using agentd::collect_edge_consensus_target_node_ids;
using agentd::upsert_edge_node_consensus_health;

static std::filesystem::path make_temp_db_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".sqlite");
}

static void open_test_db(const std::filesystem::path& path, AgentDb* out_db) {
  assert(out_db);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::string err;
  const bool ok = out_db->open(path.string(), &err);
  assert(ok);
  (void)err;
}

static EdgeConsensusFrame make_frame() {
  EdgeConsensusFrame frame;
  frame.frame_id = "frame-1";
  frame.kind = "vote_grant";
  frame.term = 7;
  frame.decision_sha256 = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  frame.candidate_node_id = "node-a";
  frame.leader_node_id = "node-a";
  frame.granted = true;
  frame.from.cluster_id = "cluster-a";
  frame.from.node_id = "node-a";
  frame.from.manifest_sha256 =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  frame.from.membership_epoch = 3;
  frame.from.trust_epochs.trust_roots_epoch = 11;
  frame.from.trust_epochs.revocations_epoch = 12;
  frame.from.trust_epochs.cert_roots_epoch = 13;
  frame.vote_witnesses.push_back(frame.from);
  frame.vote_witnesses.back().node_id = "node-b";
  return frame;
}

static void test_target_collection_dedupes_body_and_falls_back_to_envelope() {
  Json::Value body(Json::objectValue);
  body["target_node_id"] = " node-b ";
  body["target_node_ids"] = Json::Value(Json::arrayValue);
  body["target_node_ids"].append("node-c");
  body["target_node_ids"].append("node-b");
  body["target_node_ids"].append("");
  body["target_node_ids"].append("node-d");

  std::vector<std::string> targets;
  std::string err;
  assert(collect_edge_consensus_target_node_ids(body, "node:ignored", &targets, &err));
  assert(err.empty());
  assert(targets.size() == 3);
  assert(targets[0] == "node-b");
  assert(targets[1] == "node-c");
  assert(targets[2] == "node-d");

  Json::Value empty(Json::objectValue);
  targets.clear();
  assert(collect_edge_consensus_target_node_ids(empty, "node:node-z", &targets, &err));
  assert(err.empty());
  assert(targets.size() == 1);
  assert(targets[0] == "node-z");
}

static void test_target_collection_rejects_invalid_shapes() {
  Json::Value body(Json::objectValue);
  body["target_node_ids"] = Json::Value(Json::arrayValue);
  body["target_node_ids"].append("node-a");
  body["target_node_ids"].append(123);

  std::vector<std::string> targets;
  std::string err;
  assert(!collect_edge_consensus_target_node_ids(body, "", &targets, &err));
  assert(err == "target_node_ids entries must be strings");

  body["target_node_ids"].clear();
  body["target_node_ids"].append("node/evil");
  err.clear();
  assert(!collect_edge_consensus_target_node_ids(body, "", &targets, &err));
  assert(err == "invalid consensus target_node_id");
}

static void test_upsert_consensus_health_preserves_health_and_replaces_consensus() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_status");
  AgentDb db;
  open_test_db(db_path, &db);

  AgentDb::EdgeNodeRow row;
  row.node_id = "node-a";
  row.health_json = R"({"existing":true,"consensus":{"target_node_ids":["old"],"vote_witnesses":[{"node_id":"old"}]}})";
  std::string err;
  assert(db.upsert_edge_node(row, &err));

  const EdgeConsensusFrame frame = make_frame();
  assert(upsert_edge_node_consensus_health(
    &db,
    "node-a",
    frame,
    {"node-b", "node-c"},
    "msg-1",
    12345,
    &err));
  assert(err.empty());

  AgentDb::EdgeNodeRow loaded;
  assert(db.get_edge_node("node-a", &loaded, &err));
  Json::Value health(Json::nullValue);
  std::string perr;
  assert(agentd::json_parse_any(loaded.health_json, &health, &perr));
  assert(health["existing"].asBool());
  const Json::Value consensus = health["consensus"];
  assert(consensus["schema"].asString() == "edge_node_consensus_status_v1");
  assert(consensus["updated_utc_ms"].asInt64() == 12345);
  assert(consensus["last_msg_id"].asString() == "msg-1");
  assert(consensus["last_frame_id"].asString() == "frame-1");
  assert(consensus["last_frame_kind"].asString() == "vote_grant");
  assert(consensus["current_term"].asUInt64() == 7);
  assert(consensus["granted"].asBool());
  assert(consensus["forwarded_count"].asUInt64() == 2);
  assert(consensus["target_node_ids"].size() == 2);
  assert(consensus["target_node_ids"][0].asString() == "node-b");
  assert(consensus["target_node_ids"][1].asString() == "node-c");
  assert(consensus["from"]["node_id"].asString() == "node-a");
  assert(consensus["vote_witness_count"].asUInt64() == 1);
  assert(consensus["vote_witnesses"][0]["node_id"].asString() == "node-b");

  EdgeConsensusFrame no_witness = frame;
  no_witness.kind = "leader_commit";
  no_witness.vote_witnesses.clear();
  assert(upsert_edge_node_consensus_health(&db, "node-a", no_witness, {}, "msg-2", 12346, &err));
  assert(db.get_edge_node("node-a", &loaded, &err));
  assert(agentd::json_parse_any(loaded.health_json, &health, &perr));
  const Json::Value replaced = health["consensus"];
  assert(replaced["last_msg_id"].asString() == "msg-2");
  assert(replaced["last_frame_kind"].asString() == "leader_commit");
  assert(replaced["forwarded_count"].asUInt64() == 0);
  assert(!replaced.isMember("target_node_ids"));
  assert(replaced["vote_witness_count"].asUInt64() == 0);
  assert(!replaced.isMember("vote_witnesses"));
#endif
}

}  // namespace

int main() {
  test_target_collection_dedupes_body_and_falls_back_to_envelope();
  test_target_collection_rejects_invalid_shapes();
  test_upsert_consensus_health_preserves_health_and_replaces_consensus();
  return 0;
}
