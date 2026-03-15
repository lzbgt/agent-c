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
  uint64_t cert_roots_epoch
) {
  EdgeConsensusIdentity out;
  out.cluster_id = "lab-consensus";
  out.node_id = node_id;
  out.manifest_sha256 = "sha256:" + std::string(64, node_id.empty() ? 'a' : node_id.back());
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
  assert(parsed.from.trust_epochs.trust_roots_epoch == 7);
}

static void test_duplicate_vote_grants_do_not_count_twice() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5), 5);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5), 5);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5), 5);

  const EdgeConsensusFrame req = a.start_election(
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  std::vector<EdgeConsensusFrame> b_reply;
  deliver(b, req, &b_reply);
  assert(b_reply.size() == 1);
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
  assert(a_emit[0].vote_witnesses.size() == 3);
}

static void test_partition_and_quorum_recovery() {
  EdgeConsensusReplica a(make_identity("node-a", 7, 3, 5), 5);
  EdgeConsensusReplica b(make_identity("node-b", 7, 3, 5), 5);
  EdgeConsensusReplica c(make_identity("node-c", 7, 3, 5), 5);
  EdgeConsensusReplica d(make_identity("node-d", 7, 3, 5), 5);
  EdgeConsensusReplica e(make_identity("node-e", 7, 3, 5), 5);

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

}  // namespace

int main() {
  test_frame_json_roundtrip();
  test_duplicate_vote_grants_do_not_count_twice();
  test_partition_and_quorum_recovery();
  test_trust_epoch_mismatch_requires_recovery();
  return 0;
}
