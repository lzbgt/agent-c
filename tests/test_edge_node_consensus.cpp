#include "edge_node_consensus.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::EdgeConsensusEpochs;
using agentd::EdgeConsensusFrame;
using agentd::EdgeConsensusIdentity;
using agentd::EdgeConsensusReplica;

static EdgeConsensusIdentity make_identity(
  const std::string& node_id,
  uint64_t trust_roots_epoch,
  uint64_t revocations_epoch,
  uint64_t cert_roots_epoch,
  uint64_t membership_epoch = 1
) {
  EdgeConsensusIdentity out;
  out.cluster_id = "lab-consensus";
  out.node_id = node_id;
  out.manifest_sha256 = "sha256:" + std::string(64, node_id.empty() ? 'a' : node_id.back());
  out.membership_epoch = membership_epoch;
  out.trust_epochs.trust_roots_epoch = trust_roots_epoch;
  out.trust_epochs.revocations_epoch = revocations_epoch;
  out.trust_epochs.cert_roots_epoch = cert_roots_epoch;
  return out;
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

static void set_membership_all(EdgeConsensusReplica* replica, uint64_t membership_epoch, std::vector<std::string> members) {
  assert(replica);
  replica->set_membership(membership_epoch, members);
}

static void test_frame_json_roundtrip() {
  EdgeConsensusFrame frame;
  frame.frame_id = "node-a:vote_request:1";
  frame.kind = "vote_request";
  frame.term = 1;
  frame.decision_sha256 = "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  frame.candidate_node_id = "node-a";
  frame.from = make_identity("node-a", 7, 3, 5);

  const Json::Value json = agentd::edge_consensus_frame_to_json(frame);
  EdgeConsensusFrame parsed;
  std::string err;
  const bool ok = agentd::edge_consensus_frame_from_json(json, &parsed, &err);
  assert(ok);
  assert(err.empty());
  assert(parsed.kind == frame.kind);
  assert(parsed.term == frame.term);
  assert(parsed.candidate_node_id == frame.candidate_node_id);
  assert(parsed.from.node_id == frame.from.node_id);
  assert(parsed.from.membership_epoch == 1);
  assert(parsed.from.trust_epochs.trust_roots_epoch == 7);
}

static void test_frame_json_validation_errors() {
  EdgeConsensusFrame frame;
  frame.frame_id = "node-a:vote_request:1";
  frame.kind = "vote_request";
  frame.term = 1;
  frame.decision_sha256 = "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  frame.candidate_node_id = "node-a";
  frame.from = make_identity("node-a", 7, 3, 5);

  Json::Value json = agentd::edge_consensus_frame_to_json(frame);
  EdgeConsensusFrame parsed;
  std::string err;
  json["candidate_node_id"] = "bad/node";
  assert(!agentd::edge_consensus_frame_from_json(json, &parsed, &err));
  assert(err == "candidate_node_id invalid");

  json = agentd::edge_consensus_frame_to_json(frame);
  json["from"]["node_id"] = "bad/node";
  assert(!agentd::edge_consensus_frame_from_json(json, &parsed, &err));
  assert(err == "node_id invalid");
}

static void test_duplicate_vote_grants_do_not_count_twice() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5), 5);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5), 5);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5), 5);
  set_membership_all(&a, 1, {"node-a", "node-b", "node-c", "node-d", "node-e"});
  set_membership_all(&b, 1, {"node-a", "node-b", "node-c", "node-d", "node-e"});
  set_membership_all(&c, 1, {"node-a", "node-b", "node-c", "node-d", "node-e"});

  const EdgeConsensusFrame req = a.start_election(
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  assert(req.frame_id == "node-a:vote_request:1");

  std::vector<EdgeConsensusFrame> b_reply;
  deliver(b, req, &b_reply);
  assert(b_reply.size() == 1);
  assert(b_reply[0].frame_id == "node-b:vote_grant:1:node-a");
  std::vector<EdgeConsensusFrame> a_emit;
  deliver(a, b_reply[0], &a_emit);
  assert(a.leader_node_id().empty());
  assert(a_emit.empty());

  deliver(a, b_reply[0], &a_emit);
  assert(a.leader_node_id().empty());
  assert(a_emit.empty());

  std::vector<EdgeConsensusFrame> c_reply;
  deliver(c, req, &c_reply);
  assert(c_reply.size() == 1);
  deliver(a, c_reply[0], &a_emit);
  assert(a.leader_node_id() == "node-a");
  assert(a_emit.size() == 1);
  assert(a_emit[0].kind == "leader_commit");
  assert(a_emit[0].frame_id == "node-a:leader_commit:1:node-a");
  assert(a_emit[0].vote_witnesses.size() == 3);
}

static void test_partition_and_quorum_recovery() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5), 5);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5), 5);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5), 5);
  EdgeConsensusReplica d(make_identity("node-d", 7, 3, 5), 5);
  EdgeConsensusReplica e(make_identity("node-e", 7, 3, 5), 5);
  for (EdgeConsensusReplica* replica : {&a, &b, &c, &d, &e}) {
    set_membership_all(replica, 1, {"node-a", "node-b", "node-c", "node-d", "node-e"});
  }

  const EdgeConsensusFrame req_a =
    a.start_election("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  std::vector<EdgeConsensusFrame> reply_b;
  deliver(b, req_a, &reply_b);
  std::vector<EdgeConsensusFrame> a_emit;
  deliver(a, reply_b[0], &a_emit);
  assert(a.leader_node_id().empty());

  const EdgeConsensusFrame req_c =
    c.start_election("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
  std::vector<EdgeConsensusFrame> reply_d;
  std::vector<EdgeConsensusFrame> reply_e;
  deliver(d, req_c, &reply_d);
  deliver(e, req_c, &reply_e);
  std::vector<EdgeConsensusFrame> c_emit;
  deliver(c, reply_d[0], &c_emit);
  assert(c.leader_node_id().empty());
  deliver(c, reply_e[0], &c_emit);
  assert(c.leader_node_id() == "node-c");
  assert(c_emit.size() == 1);

  deliver(a, c_emit[0]);
  deliver(b, c_emit[0]);
  assert(a.leader_node_id() == "node-c");
  assert(b.leader_node_id() == "node-c");

  const EdgeConsensusFrame req_a2 =
    a.start_election("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
  std::vector<EdgeConsensusFrame> reply_b2;
  std::vector<EdgeConsensusFrame> reply_c2;
  deliver(b, req_a2, &reply_b2);
  deliver(c, req_a2, &reply_c2);
  deliver(a, reply_b2[0], &a_emit);
  assert(a.leader_node_id().empty());
  deliver(a, reply_c2[0], &a_emit);
  assert(a.leader_node_id() == "node-a");
  assert(a.current_term() == 2);
  assert(a_emit.size() == 1);
  deliver(c, a_emit[0]);
  assert(c.leader_node_id() == "node-a");
  assert(c.current_term() == 2);
}

static void test_trust_epoch_mismatch_requires_recovery() {
  EdgeConsensusReplica a(make_identity("node-a", 1, 1, 1), 3);
  EdgeConsensusReplica b(make_identity("node-b", 2, 1, 1), 3);
  EdgeConsensusReplica c(make_identity("node-c", 2, 1, 1), 3);
  set_membership_all(&a, 1, {"node-a", "node-b", "node-c"});
  set_membership_all(&b, 1, {"node-a", "node-b", "node-c"});
  set_membership_all(&c, 1, {"node-a", "node-b", "node-c"});

  const EdgeConsensusFrame stale_req =
    a.start_election("sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
  std::vector<EdgeConsensusFrame> reply_b;
  std::vector<EdgeConsensusFrame> reply_c;
  deliver(b, stale_req, &reply_b);
  deliver(c, stale_req, &reply_c);
  assert(reply_b.empty());
  assert(reply_c.empty());
  assert(a.leader_node_id().empty());

  EdgeConsensusEpochs recovered;
  recovered.trust_roots_epoch = 2;
  recovered.revocations_epoch = 1;
  recovered.cert_roots_epoch = 1;
  a.set_trust_epochs(recovered);
  a.set_membership(1, {"node-a", "node-b", "node-c"});

  const EdgeConsensusFrame fresh_req =
    a.start_election("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  deliver(b, fresh_req, &reply_b);
  deliver(c, fresh_req, &reply_c);
  std::vector<EdgeConsensusFrame> a_emit;
  deliver(a, reply_b[0], &a_emit);
  assert(a.leader_node_id() == "node-a");
  assert(a_emit.size() == 1);
  deliver(a, reply_c[0], &a_emit);
  assert(a.leader_node_id() == "node-a");
  const Json::Value status = a.status_to_json();
  assert(status["committed_vote_witnesses"].isArray());
  assert(status["committed_vote_witnesses"].size() == 2);
  for (Json::ArrayIndex i = 0; i < status["committed_vote_witnesses"].size(); i++) {
    assert(status["committed_vote_witnesses"][i]["trust_epochs"]["trust_roots_epoch"].asUInt64() == 2);
  }
}

static void test_membership_epoch_and_nonmember_rejection() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5, 4), 3);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5, 5), 3);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5, 4), 3);
  a.set_membership(4, {"node-a", "node-c"});
  b.set_membership(5, {"node-a", "node-b", "node-c"});
  c.set_membership(4, {"node-a", "node-c"});

  const EdgeConsensusFrame stale_req =
    a.start_election("sha256:1212121212121212121212121212121212121212121212121212121212121212");
  std::vector<EdgeConsensusFrame> reply_b;
  std::vector<EdgeConsensusFrame> reply_c;
  deliver(b, stale_req, &reply_b);
  deliver(c, stale_req, &reply_c);
  assert(reply_b.empty());
  assert(reply_c.size() == 1);

  EdgeConsensusReplica d(make_identity("node-d", 7, 3, 5, 4), 3);
  d.set_membership(4, {"node-c", "node-d", "node-e"});
  std::vector<EdgeConsensusFrame> reply_d;
  deliver(d, stale_req, &reply_d);
  assert(reply_d.empty());

  a.set_membership(5, {"node-a", "node-b", "node-c"});
  c.set_membership(5, {"node-a", "node-b", "node-c"});
  const EdgeConsensusFrame fresh_req =
    a.start_election("sha256:3434343434343434343434343434343434343434343434343434343434343434");
  deliver(b, fresh_req, &reply_b);
  deliver(c, fresh_req, &reply_c);
  assert(reply_b.size() == 1);
  assert(reply_c.size() == 1);
}

static void test_leader_commit_requires_valid_witness_quorum() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5), 3);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5), 3);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5), 3);
  for (EdgeConsensusReplica* replica : {&a, &b, &c}) {
    set_membership_all(replica, 1, {"node-a", "node-b", "node-c"});
  }

  const EdgeConsensusFrame req =
    a.start_election("sha256:5656565656565656565656565656565656565656565656565656565656565656");
  std::vector<EdgeConsensusFrame> reply_b;
  deliver(b, req, &reply_b);
  assert(reply_b.size() == 1);
  std::vector<EdgeConsensusFrame> a_emit;
  deliver(a, reply_b[0], &a_emit);
  assert(a_emit.size() == 1);
  const EdgeConsensusFrame commit = a_emit[0];
  assert(commit.kind == "leader_commit");
  assert(commit.vote_witnesses.size() == 2);

  EdgeConsensusFrame no_witness_commit = commit;
  no_witness_commit.vote_witnesses.clear();
  deliver(c, no_witness_commit);
  assert(c.leader_node_id().empty());

  EdgeConsensusFrame no_leader_witness_commit = commit;
  no_leader_witness_commit.frame_id += ":no_leader";
  no_leader_witness_commit.vote_witnesses.clear();
  no_leader_witness_commit.vote_witnesses.push_back(make_identity("node-b", 7, 3, 5));
  no_leader_witness_commit.vote_witnesses.push_back(make_identity("node-c", 7, 3, 5));
  deliver(c, no_leader_witness_commit);
  assert(c.leader_node_id().empty());

  EdgeConsensusFrame bad_trust_witness_commit = commit;
  bad_trust_witness_commit.frame_id += ":bad_trust";
  bad_trust_witness_commit.vote_witnesses[1].trust_epochs.trust_roots_epoch = 99;
  deliver(c, bad_trust_witness_commit);
  assert(c.leader_node_id().empty());

  deliver(c, commit);
  assert(c.leader_node_id() == "node-a");
  assert(c.committed_decision_sha256() == commit.decision_sha256);
}

static void test_frame_role_identity_rejection() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5), 3);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5), 3);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5), 3);
  for (EdgeConsensusReplica* replica : {&a, &b, &c}) {
    set_membership_all(replica, 1, {"node-a", "node-b", "node-c"});
  }

  EdgeConsensusFrame forged_req =
    a.start_election("sha256:7878787878787878787878787878787878787878787878787878787878787878");
  forged_req.frame_id += ":forged_sender";
  forged_req.from = make_identity("node-b", 7, 3, 5);
  std::vector<EdgeConsensusFrame> forged_req_reply;
  deliver(c, forged_req, &forged_req_reply);
  assert(forged_req_reply.empty());

  const EdgeConsensusFrame req =
    a.start_election("sha256:9090909090909090909090909090909090909090909090909090909090909090");
  std::vector<EdgeConsensusFrame> reply_b;
  deliver(b, req, &reply_b);
  assert(reply_b.size() == 1);

  EdgeConsensusFrame forged_self_grant = reply_b[0];
  forged_self_grant.frame_id += ":self_grant";
  forged_self_grant.from = make_identity("node-a", 7, 3, 5);
  std::vector<EdgeConsensusFrame> a_emit;
  deliver(a, forged_self_grant, &a_emit);
  assert(a.leader_node_id().empty());
  assert(a_emit.empty());

  deliver(a, reply_b[0], &a_emit);
  assert(a_emit.size() == 1);
  const EdgeConsensusFrame commit = a_emit[0];

  EdgeConsensusFrame forged_commit_sender = commit;
  forged_commit_sender.frame_id += ":forged_sender";
  forged_commit_sender.from = make_identity("node-b", 7, 3, 5);
  deliver(c, forged_commit_sender);
  assert(c.leader_node_id().empty());

  deliver(c, commit);
  assert(c.leader_node_id() == "node-a");
}

}  // namespace

int main() {
  test_frame_json_roundtrip();
  test_frame_json_validation_errors();
  test_duplicate_vote_grants_do_not_count_twice();
  test_partition_and_quorum_recovery();
  test_trust_epoch_mismatch_requires_recovery();
  test_membership_epoch_and_nonmember_rejection();
  test_leader_commit_requires_valid_witness_quorum();
  test_frame_role_identity_rejection();
  return 0;
}
