#include "edge_node_consensus.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::EdgeConsensusEpochs;
using agentd::EdgeConsensusFrame;
using agentd::EdgeConsensusIdentity;
using agentd::EdgeConsensusMembershipPolicyUpdate;
using agentd::EdgeConsensusNodeLoop;
using agentd::EdgeConsensusNodeLoopConfig;
using agentd::EdgeConsensusReplica;

static EdgeConsensusIdentity make_identity(const std::string& node_id) {
  EdgeConsensusIdentity out;
  out.cluster_id = "lab-consensus-loop";
  out.node_id = node_id;
  out.manifest_sha256 = "sha256:" + std::string(64, node_id.empty() ? 'a' : node_id.back());
  out.membership_epoch = 9;
  out.trust_epochs.trust_roots_epoch = 3;
  out.trust_epochs.revocations_epoch = 1;
  out.trust_epochs.cert_roots_epoch = 5;
  return out;
}

static EdgeConsensusNodeLoop make_loop(
  const std::string& node_id,
  const std::vector<std::string>& peers,
  const std::string& decision_sha256,
  int64_t campaign_delay_ms = 0,
  int64_t campaign_retry_ms = 0,
  int64_t campaign_retry_max_ms = 0,
  int64_t campaign_retry_backoff_factor = 1,
  int64_t leader_heartbeat_ms = 1000,
  int64_t leader_lease_ms = 5000,
  int64_t lease_expiry_recampaign_delay_ms = 0
) {
  EdgeConsensusNodeLoopConfig cfg;
  cfg.self = make_identity(node_id);
  cfg.peer_node_ids = peers;
  cfg.cluster_size = peers.size() + 1;
  cfg.campaign_delay_ms = campaign_delay_ms;
  cfg.campaign_retry_ms = campaign_retry_ms;
  cfg.campaign_retry_max_ms = campaign_retry_max_ms;
  cfg.campaign_retry_backoff_factor = campaign_retry_backoff_factor;
  cfg.leader_heartbeat_ms = leader_heartbeat_ms;
  cfg.leader_lease_ms = leader_lease_ms;
  cfg.lease_expiry_recampaign_delay_ms = lease_expiry_recampaign_delay_ms;
  cfg.decision_sha256 = decision_sha256;
  return EdgeConsensusNodeLoop(cfg);
}

static void deliver(
  EdgeConsensusReplica& target,
  const EdgeConsensusFrame& frame,
  std::vector<EdgeConsensusFrame>* out_frames = nullptr
) {
  std::vector<EdgeConsensusFrame> local;
  std::string err;
  const bool ok = target.handle_frame(frame, &local, &err);
  assert(ok);
  assert(err.empty());
  if (out_frames) *out_frames = local;
}

static void set_membership_all(EdgeConsensusReplica* replica, std::vector<std::string> members) {
  assert(replica);
  replica->set_membership(9, members);
}

static void test_tick_emits_election_once_after_delay() {
  auto loop = make_loop(
    "node-a",
    {"node-b", "node-c", "node-c", "node-a"},
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    200
  );

  std::vector<EdgeConsensusFrame> frames = loop.tick(1000);
  assert(frames.empty());
  frames = loop.tick(1199);
  assert(frames.empty());
  frames = loop.tick(1200);
  assert(frames.size() == 1);
  assert(frames[0].kind == "vote_request");
  assert(loop.election_started());

  const std::vector<std::string> targets = loop.target_node_ids_for_frame(frames[0]);
  assert(targets.size() == 2);
  assert(targets[0] == "node-b");
  assert(targets[1] == "node-c");

  frames = loop.tick(1300);
  assert(frames.empty());
}

static void test_tick_retries_election_after_retry_delay() {
  auto loop = make_loop(
    "node-a",
    {"node-b", "node-c"},
    "sha256:abababababababababababababababababababababababababababababababab",
    100,
    250
  );

  std::vector<EdgeConsensusFrame> frames = loop.tick(1000);
  assert(frames.empty());
  frames = loop.tick(1100);
  assert(frames.size() == 1);
  assert(frames[0].kind == "vote_request");
  assert(frames[0].term == 1);
  assert(loop.campaign_attempts() == 1);

  frames = loop.tick(1349);
  assert(frames.empty());
  frames = loop.tick(1350);
  assert(frames.size() == 1);
  assert(frames[0].kind == "vote_request");
  assert(frames[0].term == 2);
  assert(loop.campaign_attempts() == 2);
}

static void test_tick_applies_bounded_backoff() {
  auto loop = make_loop(
    "node-a",
    {"node-b", "node-c"},
    "sha256:efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef",
    100,
    200,
    500,
    2
  );

  std::vector<EdgeConsensusFrame> frames = loop.tick(1000);
  assert(frames.empty());
  frames = loop.tick(1100);
  assert(frames.size() == 1);
  assert(loop.campaign_attempts() == 1);
  assert(loop.current_campaign_delay_ms() == 200);

  frames = loop.tick(1299);
  assert(frames.empty());
  frames = loop.tick(1300);
  assert(frames.size() == 1);
  assert(loop.campaign_attempts() == 2);
  assert(loop.current_campaign_delay_ms() == 400);

  frames = loop.tick(1699);
  assert(frames.empty());
  frames = loop.tick(1700);
  assert(frames.size() == 1);
  assert(loop.campaign_attempts() == 3);
  assert(loop.current_campaign_delay_ms() == 500);

  frames = loop.tick(2199);
  assert(frames.empty());
  frames = loop.tick(2200);
  assert(frames.size() == 1);
  assert(loop.campaign_attempts() == 4);
  assert(loop.current_campaign_delay_ms() == 500);
}

static void test_vote_grant_routes_back_to_candidate() {
  auto loop_a = make_loop(
    "node-a",
    {"node-b", "node-c"},
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  );
  auto request = loop_a.tick(500);
  assert(request.size() == 1);

  EdgeConsensusReplica replica_b(make_identity("node-b"), 3);
  set_membership_all(&replica_b, {"node-a", "node-b", "node-c"});
  std::vector<EdgeConsensusFrame> replies;
  deliver(replica_b, request[0], &replies);
  assert(replies.size() == 1);
  assert(replies[0].kind == "vote_grant");

  auto loop_b = make_loop("node-b", {"node-a", "node-c"}, "");
  const std::vector<std::string> targets = loop_b.target_node_ids_for_frame(replies[0]);
  assert(targets.size() == 1);
  assert(targets[0] == "node-a");
}

static void test_quorum_commit_emits_leader_commit() {
  EdgeConsensusNodeLoopConfig cfg_a;
  cfg_a.self = make_identity("node-a");
  cfg_a.peer_node_ids = {"node-b", "node-c", "node-d", "node-e"};
  cfg_a.cluster_size = 5;
  cfg_a.decision_sha256 = "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  EdgeConsensusNodeLoop loop_a(cfg_a);
  EdgeConsensusReplica replica_b(make_identity("node-b"), 5);
  EdgeConsensusReplica replica_c(make_identity("node-c"), 5);
  set_membership_all(&replica_b, {"node-a", "node-b", "node-c", "node-d", "node-e"});
  set_membership_all(&replica_c, {"node-a", "node-b", "node-c", "node-d", "node-e"});

  const std::vector<EdgeConsensusFrame> request = loop_a.tick(100);
  assert(request.size() == 1);

  std::vector<EdgeConsensusFrame> reply_b;
  std::vector<EdgeConsensusFrame> reply_c;
  deliver(replica_b, request[0], &reply_b);
  deliver(replica_c, request[0], &reply_c);
  assert(reply_b.size() == 1);
  assert(reply_c.size() == 1);

  std::vector<EdgeConsensusFrame> generated;
  std::string err;
  bool ok = loop_a.handle_frame(reply_b[0], &generated, &err);
  assert(ok);
  assert(err.empty());
  assert(generated.empty());

  ok = loop_a.handle_frame(reply_c[0], &generated, &err);
  assert(ok);
  assert(err.empty());
  assert(generated.size() == 1);
  assert(generated[0].kind == "leader_commit");
  assert(generated[0].leader_node_id == "node-a");
  assert(generated[0].vote_witnesses.size() == 3);

  const std::vector<std::string> targets = loop_a.target_node_ids_for_frame(generated[0]);
  assert(targets.size() == 4);
  assert(loop_a.committed_decision_sha256() ==
         "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
  assert(loop_a.leader_node_id() == "node-a");
}

static void test_status_surfaces_loop_config() {
  auto loop = make_loop(
    "node-z",
    {"node-y", "node-x"},
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
    50,
    250,
    500,
    2,
    300,
    1200,
    450
  );
  (void)loop.tick(1000);
  (void)loop.tick(1050);
  const Json::Value status = loop.status_to_json();
  assert(status["self"]["node_id"].asString() == "node-z");
  assert(status["campaign_delay_ms"].asInt64() == 50);
  assert(status["campaign_retry_ms"].asInt64() == 250);
  assert(status["campaign_retry_max_ms"].asInt64() == 500);
  assert(status["campaign_retry_backoff_factor"].asInt64() == 2);
  assert(status["leader_heartbeat_ms"].asInt64() == 300);
  assert(status["leader_lease_ms"].asInt64() == 1200);
  assert(status["lease_expiry_recampaign_delay_ms"].asInt64() == 450);
  assert(status["self"]["membership_epoch"].asUInt64() == 9);
  assert(status["peer_node_ids"].isArray());
  assert(status["peer_node_ids"].size() == 2);
  assert(status["member_node_ids"].isArray());
  assert(status["member_node_ids"].size() == 3);
  assert(status["election_started"].asBool());
  assert(status["campaign_attempts"].asUInt64() == 1);
  assert(status["current_campaign_delay_ms"].asInt64() == 250);
  assert(status["last_campaign_started_utc_ms"].asInt64() == 1050);
  assert(status["next_campaign_utc_ms"].asInt64() == 1300);
  assert(status["replica"]["campaign_decision_sha256"].asString() ==
         "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
  assert(status["replica"]["member_node_ids"].isArray());
  assert(status["replica"]["member_node_ids"].size() == 3);
}

static void test_leader_heartbeat_and_follower_lease_expiry() {
  const std::string decision = "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  auto loop_a = make_loop("node-a", {"node-b", "node-c"}, decision, 0, 300, 600, 2, 200, 700);
  auto loop_b = make_loop("node-b", {"node-a", "node-c"}, "", 0, 300, 600, 2, 200, 700, 250);
  EdgeConsensusReplica replica_b(make_identity("node-b"), 3);
  EdgeConsensusReplica replica_c(make_identity("node-c"), 3);
  set_membership_all(&replica_b, {"node-a", "node-b", "node-c"});
  set_membership_all(&replica_c, {"node-a", "node-b", "node-c"});

  const std::vector<EdgeConsensusFrame> request = loop_a.tick(100);
  assert(request.size() == 1);

  std::vector<EdgeConsensusFrame> reply_b;
  std::vector<EdgeConsensusFrame> reply_c;
  deliver(replica_b, request[0], &reply_b);
  deliver(replica_c, request[0], &reply_c);

  std::vector<EdgeConsensusFrame> generated;
  std::string err;
  bool ok = loop_a.handle_frame(reply_b[0], &generated, &err, 120);
  assert(ok);
  assert(err.empty());
  assert(generated.size() == 1);
  assert(generated[0].kind == "leader_commit");
  const EdgeConsensusFrame first_commit = generated[0];

  ok = loop_a.handle_frame(reply_c[0], &generated, &err, 130);
  assert(ok);
  assert(err.empty());
  assert(generated.empty());

  std::vector<EdgeConsensusFrame> follower_generated;
  ok = loop_b.handle_frame(first_commit, &follower_generated, &err, 140);
  assert(ok);
  assert(err.empty());
  assert(follower_generated.empty());
  assert(loop_b.committed_decision_sha256() == decision);
  assert(loop_b.leader_node_id() == "node-a");

  std::vector<EdgeConsensusFrame> heartbeat = loop_a.tick(319);
  assert(heartbeat.empty());
  heartbeat = loop_a.tick(320);
  assert(heartbeat.size() == 1);
  assert(heartbeat[0].kind == "leader_commit");
  assert(heartbeat[0].leader_node_id == "node-a");

  ok = loop_b.handle_frame(heartbeat[0], &follower_generated, &err, 340);
  assert(ok);
  assert(err.empty());
  assert(loop_b.committed_decision_sha256() == decision);

  std::vector<EdgeConsensusFrame> retry = loop_b.tick(1039);
  assert(retry.empty());
  retry = loop_b.tick(1040);
  assert(retry.empty());
  retry = loop_b.tick(1289);
  assert(retry.empty());
  retry = loop_b.tick(1290);
  assert(retry.size() == 1);
  assert(retry[0].kind == "vote_request");
  assert(retry[0].candidate_node_id == "node-b");
  assert(retry[0].decision_sha256 == decision);
  assert(loop_b.leader_node_id().empty());
  assert(loop_b.committed_decision_sha256().empty());

  const Json::Value status = loop_b.status_to_json();
  assert(status["last_known_decision_sha256"].asString() == decision);
  assert(status["leader_heartbeat_ms"].asInt64() == 200);
  assert(status["leader_lease_ms"].asInt64() == 700);
  assert(status["lease_expiry_recampaign_delay_ms"].asInt64() == 250);
  assert(status["campaign_attempts"].asUInt64() == 1);
  assert(status["leader_lease_expired_count"].asUInt64() == 1);
  assert(status["last_leader_lease_expired_utc_ms"].asInt64() == 1040);
  assert(status["lease_expiry_recampaign_ready_utc_ms"].asInt64() == 1290);
}

static void test_adopt_membership_policy_advances_epoch_and_resets_loop() {
  auto loop = make_loop(
    "node-a",
    {"node-b", "node-c"},
    "sha256:9999999999999999999999999999999999999999999999999999999999999999",
    0,
    200
  );
  const std::vector<EdgeConsensusFrame> first_campaign = loop.tick(100);
  assert(first_campaign.size() == 1);
  assert(loop.election_started());
  assert(loop.campaign_attempts() == 1);
  assert(loop.replica().current_term() == 1);

  EdgeConsensusMembershipPolicyUpdate stale;
  stale.cluster_id = "lab-consensus-loop";
  stale.membership_epoch = 9;
  stale.member_node_ids = {"node-a", "node-b", "node-d"};
  bool adopted = true;
  std::string reason;
  assert(loop.adopt_membership_policy(stale, &adopted, &reason));
  assert(!adopted);
  assert(reason == "membership_epoch not newer");
  assert(loop.config().self.membership_epoch == 9);

  EdgeConsensusMembershipPolicyUpdate update;
  update.cluster_id = "lab-consensus-loop";
  update.membership_epoch = 10;
  update.member_node_ids = {"node-d", "node-a", "node-b", "node-d"};
  update.campaign_delay_ms = 0;
  update.campaign_retry_ms = 125;
  update.campaign_retry_max_ms = 500;
  update.campaign_retry_backoff_factor = 2;
  update.leader_heartbeat_ms = 300;
  update.leader_lease_ms = 1200;
  update.lease_expiry_recampaign_delay_ms = 450;
  assert(loop.adopt_membership_policy(update, &adopted, &reason));
  assert(adopted);
  assert(reason == "adopted");
  assert(!loop.election_started());
  assert(loop.campaign_attempts() == 0);
  assert(loop.replica().current_term() == 0);
  assert(loop.replica().leader_node_id().empty());
  assert(loop.replica().committed_decision_sha256().empty());

  const Json::Value status = loop.status_to_json();
  assert(status["self"]["membership_epoch"].asUInt64() == 10);
  assert(status["member_node_ids"].size() == 3);
  assert(status["member_node_ids"][0].asString() == "node-a");
  assert(status["member_node_ids"][1].asString() == "node-b");
  assert(status["member_node_ids"][2].asString() == "node-d");
  assert(status["peer_node_ids"].size() == 2);
  assert(status["peer_node_ids"][0].asString() == "node-b");
  assert(status["peer_node_ids"][1].asString() == "node-d");
  assert(status["cluster_size"].asUInt64() == 3);
  assert(status["campaign_retry_ms"].asInt64() == 125);
  assert(status["campaign_retry_max_ms"].asInt64() == 500);
  assert(status["campaign_retry_backoff_factor"].asInt64() == 2);
  assert(status["leader_heartbeat_ms"].asInt64() == 300);
  assert(status["leader_lease_ms"].asInt64() == 1200);
  assert(status["lease_expiry_recampaign_delay_ms"].asInt64() == 450);

  EdgeConsensusMembershipPolicyUpdate removed_self = update;
  removed_self.membership_epoch = 11;
  removed_self.member_node_ids = {"node-b", "node-d"};
  assert(loop.adopt_membership_policy(removed_self, &adopted, &reason));
  assert(!adopted);
  assert(reason == "self node missing from membership");
  assert(loop.config().self.membership_epoch == 10);

  const std::vector<EdgeConsensusFrame> next_campaign = loop.tick(200);
  assert(next_campaign.size() == 1);
  assert(next_campaign[0].from.membership_epoch == 10);
  const std::vector<std::string> targets = loop.target_node_ids_for_frame(next_campaign[0]);
  assert(targets.size() == 2);
  assert(targets[0] == "node-b");
  assert(targets[1] == "node-d");
}

}  // namespace

int main() {
  test_tick_emits_election_once_after_delay();
  test_tick_retries_election_after_retry_delay();
  test_tick_applies_bounded_backoff();
  test_vote_grant_routes_back_to_candidate();
  test_quorum_commit_emits_leader_commit();
  test_status_surfaces_loop_config();
  test_leader_heartbeat_and_follower_lease_expiry();
  test_adopt_membership_policy_advances_epoch_and_resets_loop();
  return 0;
}
