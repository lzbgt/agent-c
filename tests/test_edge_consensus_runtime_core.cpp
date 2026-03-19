#include "edge_consensus_runtime_core.h"

#include <atomic>
#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::EdgeConsensusEpochs;
using agentd::EdgeConsensusFrame;
using agentd::EdgeConsensusHttpRuntimeConfig;
using agentd::EdgeConsensusHttpRuntimeHooks;
using agentd::EdgeConsensusIdentity;
using agentd::EdgeConsensusNodeLoop;
using agentd::EdgeConsensusNodeLoopConfig;
using agentd::EdgeConsensusReplica;
using agentd::EdgeConsensusRuntimeTransportOps;
using agentd::edge_consensus_frame_to_json;
using agentd::run_edge_consensus_runtime_core;

static EdgeConsensusHttpRuntimeConfig make_config() {
  EdgeConsensusHttpRuntimeConfig cfg;
  cfg.daemon_url = "http://127.0.0.1:8123";
  cfg.auth_token = "token";
  cfg.node_id = "node-a";
  cfg.cluster_id = "cluster-a";
  cfg.manifest_sha256 = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  cfg.model = "edge_consensus_node";
  cfg.fw_git_sha = "agentd_managed_runtime";
  cfg.decision_sha256 = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  cfg.peer_node_ids = {"node-b", "node-c"};
  cfg.member_node_ids = {"node-a", "node-b", "node-c"};
  cfg.cluster_size = 3;
  cfg.poll_interval_ms = 0;
  cfg.deadline_ms = 200;
  cfg.membership_epoch = 9;
  cfg.trust_roots_epoch = 3;
  cfg.revocations_epoch = 1;
  cfg.cert_roots_epoch = 5;
  return cfg;
}

static EdgeConsensusIdentity make_identity(const std::string& node_id) {
  EdgeConsensusIdentity out;
  out.cluster_id = "cluster-a";
  out.node_id = node_id;
  out.manifest_sha256 = "sha256:" + std::string(64, node_id.empty() ? 'a' : node_id.back());
  out.membership_epoch = 9;
  out.trust_epochs.trust_roots_epoch = 3;
  out.trust_epochs.revocations_epoch = 1;
  out.trust_epochs.cert_roots_epoch = 5;
  return out;
}

static Json::Value make_outbox_with_frame(const EdgeConsensusFrame& frame, int64_t cursor_next = 1) {
  Json::Value outbox(Json::objectValue);
  outbox["cursor_next"] = (Json::Int64)cursor_next;
  Json::Value message(Json::objectValue);
  Json::Value env(Json::objectValue);
  env["type"] = "CONSENSUS_FRAME";
  Json::Value body(Json::objectValue);
  body["frame"] = edge_consensus_frame_to_json(frame);
  env["body"] = body;
  message["msg"] = env;
  outbox["messages"].append(message);
  return outbox;
}

static void set_membership_all(EdgeConsensusReplica* replica) {
  assert(replica);
  replica->set_membership(9, {"node-a", "node-b", "node-c"});
}

static std::vector<EdgeConsensusFrame> make_vote_grants_for_node_a(const EdgeConsensusHttpRuntimeConfig& cfg) {
  EdgeConsensusNodeLoopConfig loop_cfg;
  loop_cfg.self = make_identity(cfg.node_id);
  loop_cfg.peer_node_ids = cfg.peer_node_ids;
  loop_cfg.member_node_ids = cfg.member_node_ids;
  loop_cfg.cluster_size = cfg.cluster_size;
  loop_cfg.decision_sha256 = cfg.decision_sha256;
  EdgeConsensusNodeLoop loop(loop_cfg);
  const std::vector<EdgeConsensusFrame> requests = loop.tick(1);
  assert(requests.size() == 1);

  EdgeConsensusReplica replica_b(make_identity("node-b"), cfg.cluster_size);
  EdgeConsensusReplica replica_c(make_identity("node-c"), cfg.cluster_size);
  set_membership_all(&replica_b);
  set_membership_all(&replica_c);

  std::vector<EdgeConsensusFrame> grants_b;
  std::vector<EdgeConsensusFrame> grants_c;
  std::string err;
  assert(replica_b.handle_frame(requests[0], &grants_b, &err));
  assert(err.empty());
  assert(replica_c.handle_frame(requests[0], &grants_c, &err));
  assert(err.empty());
  assert(grants_b.size() == 1);
  assert(grants_c.size() == 1);
  return {grants_b[0], grants_c[0]};
}

static void test_runtime_core_rejects_incomplete_transport() {
  const EdgeConsensusHttpRuntimeConfig cfg = make_config();
  EdgeConsensusRuntimeTransportOps transport;
  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusHttpRuntimeHooks(), transport, &result, &error);
  assert(!ok);
  assert(error == "runtime transport incomplete");
}

static void test_runtime_core_surfaces_hello_failure() {
  const EdgeConsensusHttpRuntimeConfig cfg = make_config();
  EdgeConsensusRuntimeTransportOps transport;
  transport.post_hello = [](uint64_t*, std::string* out_error) {
    if (out_error) *out_error = "network down";
    return false;
  };
  transport.send_consensus_frame = [](const EdgeConsensusFrame&, const std::vector<std::string>&, uint64_t*, std::string*) {
    return true;
  };
  transport.poll_outbox = [](int64_t, Json::Value* out, std::string*) {
    if (out) *out = Json::Value(Json::objectValue);
    return true;
  };

  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusHttpRuntimeHooks(), transport, &result, &error);
  assert(!ok);
  assert(error == "failed to post NODE_HELLO: network down");
}

static void test_runtime_core_returns_structured_stop_result() {
  const EdgeConsensusHttpRuntimeConfig cfg = make_config();
  std::atomic<bool> stop_requested(true);
  int startup_ready_calls = 0;
  int status_updates = 0;

  EdgeConsensusRuntimeTransportOps transport;
  transport.post_hello = [](uint64_t* io_seq, std::string*) {
    assert(io_seq);
    ++(*io_seq);
    return true;
  };
  transport.send_consensus_frame = [](const EdgeConsensusFrame&, const std::vector<std::string>&, uint64_t*, std::string*) {
    assert(false && "stop path should not send frames");
    return false;
  };
  transport.poll_outbox = [](int64_t, Json::Value* out, std::string*) {
    if (out) *out = Json::Value(Json::objectValue);
    return true;
  };

  EdgeConsensusHttpRuntimeHooks hooks;
  hooks.stop_requested = &stop_requested;
  hooks.startup_ready = [&startup_ready_calls]() { startup_ready_calls++; };
  hooks.status_update = [&status_updates](const Json::Value&) { status_updates++; };

  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, hooks, transport, &result, &error);
  assert(ok);
  assert(error.empty());
  assert(startup_ready_calls == 1);
  assert(status_updates >= 2);
  assert(!result["ok"].asBool());
  assert(result["error"].asString() == "stopped");
  assert(result["node_id"].asString() == "node-a");
}

static void test_runtime_core_returns_deadline_result_without_commit() {
  EdgeConsensusHttpRuntimeConfig cfg = make_config();
  cfg.deadline_ms = 0;

  EdgeConsensusRuntimeTransportOps transport;
  transport.post_hello = [](uint64_t* io_seq, std::string*) {
    assert(io_seq);
    ++(*io_seq);
    return true;
  };
  transport.send_consensus_frame = [](const EdgeConsensusFrame&, const std::vector<std::string>&, uint64_t*, std::string*) {
    return true;
  };
  transport.poll_outbox = [](int64_t, Json::Value* out, std::string*) {
    if (out) *out = Json::Value(Json::objectValue);
    return true;
  };

  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusHttpRuntimeHooks(), transport, &result, &error);
  assert(ok);
  assert(error.empty());
  assert(!result["ok"].asBool());
  assert(result["error"].asString() == "deadline exceeded before commit");
}

static void test_runtime_core_commits_from_relayed_vote_grants() {
  const EdgeConsensusHttpRuntimeConfig cfg = make_config();
  const std::vector<EdgeConsensusFrame> grants = make_vote_grants_for_node_a(cfg);
  int send_calls = 0;
  int poll_calls = 0;
  Json::Value statuses(Json::arrayValue);

  EdgeConsensusRuntimeTransportOps transport;
  transport.post_hello = [](uint64_t* io_seq, std::string*) {
    assert(io_seq);
    ++(*io_seq);
    return true;
  };
  transport.send_consensus_frame = [&send_calls](
    const EdgeConsensusFrame& frame,
    const std::vector<std::string>& targets,
    uint64_t* io_seq,
    std::string*
  ) {
    assert(io_seq);
    ++(*io_seq);
    send_calls++;
    if (frame.kind == "vote_request") {
      assert(targets.size() == 2);
    } else if (frame.kind == "leader_commit") {
      assert(targets.size() == 2);
    } else {
      assert(false && "unexpected frame kind");
    }
    return true;
  };
  transport.poll_outbox = [&poll_calls, &grants](int64_t, Json::Value* out, std::string*) {
    poll_calls++;
    if (!out) return true;
    if (poll_calls == 1) {
      *out = make_outbox_with_frame(grants[0], 1);
    } else {
      *out = Json::Value(Json::objectValue);
    }
    return true;
  };

  EdgeConsensusHttpRuntimeHooks hooks;
  hooks.status_update = [&statuses](const Json::Value& status) { statuses.append(status); };

  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, hooks, transport, &result, &error);
  assert(ok);
  assert(error.empty());
  assert(result["ok"].asBool());
  assert(result["leader_node_id"].asString() == "node-a");
  assert(result["committed_decision_sha256"].asString() == cfg.decision_sha256);
  assert(send_calls == 2);
  assert(poll_calls == 1);
  assert(statuses.isArray());
  assert(statuses.size() >= 3);
}

}  // namespace

int main() {
  test_runtime_core_rejects_incomplete_transport();
  test_runtime_core_surfaces_hello_failure();
  test_runtime_core_returns_structured_stop_result();
  test_runtime_core_returns_deadline_result_without_commit();
  test_runtime_core_commits_from_relayed_vote_grants();
  return 0;
}
