#include "edge_consensus_runtime_core.h"

#include <atomic>
#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::EdgeConsensusEpochs;
using agentd::EdgeConsensusFrame;
using agentd::EdgeConsensusRuntimeConfig;
using agentd::EdgeConsensusRuntimeHooks;
using agentd::EdgeConsensusIdentity;
using agentd::EdgeConsensusNodeLoop;
using agentd::EdgeConsensusNodeLoopConfig;
using agentd::EdgeConsensusReplica;
using agentd::EdgeConsensusRuntimeTransportOps;
using agentd::edge_consensus_frame_to_json;
using agentd::run_edge_consensus_runtime_core;

static EdgeConsensusRuntimeConfig make_config() {
  EdgeConsensusRuntimeConfig cfg;
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

static EdgeConsensusIdentity make_identity(const std::string& node_id, uint64_t membership_epoch = 9) {
  EdgeConsensusIdentity out;
  out.cluster_id = "cluster-a";
  out.node_id = node_id;
  out.manifest_sha256 = "sha256:" + std::string(64, node_id.empty() ? 'a' : node_id.back());
  out.membership_epoch = membership_epoch;
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
  env["type"] = AGENT_UM_BMP_TYPE_CONSENSUS_FRAME;
  Json::Value body(Json::objectValue);
  body["frame"] = edge_consensus_frame_to_json(frame);
  env["body"] = body;
  message["msg"] = env;
  outbox["messages"].append(message);
  return outbox;
}

static Json::Value make_membership_bundle(uint64_t membership_epoch, const std::vector<std::string>& members) {
  Json::Value bundle(Json::objectValue);
  bundle["schema"] = AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1;
  bundle["cluster_id"] = "cluster-a";
  bundle["membership_epoch"] = Json::UInt64(membership_epoch);
  bundle["campaign_delay_ms"] = Json::Int64(0);
  bundle["campaign_retry_ms"] = Json::Int64(25);
  bundle["campaign_retry_max_ms"] = Json::Int64(100);
  bundle["campaign_retry_backoff_factor"] = Json::Int64(2);
  bundle["leader_heartbeat_ms"] = Json::Int64(50);
  bundle["leader_lease_ms"] = Json::Int64(200);
  bundle["lease_expiry_recampaign_delay_ms"] = Json::Int64(75);
  Json::Value member_arr(Json::arrayValue);
  for (const auto& member : members) member_arr.append(member);
  bundle["member_node_ids"] = member_arr;
  return bundle;
}

static Json::Value make_outbox_with_membership_bundle(
  uint64_t membership_epoch,
  const std::vector<std::string>& members,
  int64_t cursor_next = 1
) {
  Json::Value outbox(Json::objectValue);
  outbox["cursor_next"] = (Json::Int64)cursor_next;
  Json::Value message(Json::objectValue);
  Json::Value env(Json::objectValue);
  env["type"] = AGENT_UM_BMP_TYPE_PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE;
  Json::Value body(Json::objectValue);
  body["membership"] = make_membership_bundle(membership_epoch, members);
  env["body"] = body;
  message["msg"] = env;
  outbox["messages"].append(message);
  return outbox;
}

static Json::Value make_outbox_with_unrelated_message(int64_t cursor_next = 1) {
  Json::Value outbox(Json::objectValue);
  outbox["cursor_next"] = (Json::Int64)cursor_next;
  Json::Value message(Json::objectValue);
  Json::Value env(Json::objectValue);
  env["type"] = AGENT_UM_BMP_TYPE_NODE_HELLO;
  Json::Value body(Json::objectValue);
  body["frame"] = "not a consensus frame";
  env["body"] = body;
  message["msg"] = env;
  outbox["messages"].append(message);
  return outbox;
}

static void set_membership_all(
  EdgeConsensusReplica* replica,
  uint64_t membership_epoch,
  const std::vector<std::string>& members
) {
  assert(replica);
  replica->set_membership(membership_epoch, members);
}

static std::vector<EdgeConsensusFrame> make_vote_grants_for_node_a(const EdgeConsensusRuntimeConfig& cfg) {
  EdgeConsensusNodeLoopConfig loop_cfg;
  loop_cfg.self = make_identity(cfg.node_id, cfg.membership_epoch);
  loop_cfg.peer_node_ids = cfg.peer_node_ids;
  loop_cfg.member_node_ids = cfg.member_node_ids;
  loop_cfg.cluster_size = cfg.cluster_size;
  loop_cfg.decision_sha256 = cfg.decision_sha256;
  EdgeConsensusNodeLoop loop(loop_cfg);
  const std::vector<EdgeConsensusFrame> requests = loop.tick(1);
  assert(requests.size() == 1);

  std::vector<EdgeConsensusFrame> out;
  for (const auto& peer : cfg.peer_node_ids) {
    EdgeConsensusReplica replica(make_identity(peer, cfg.membership_epoch), cfg.cluster_size);
    set_membership_all(&replica, cfg.membership_epoch, cfg.member_node_ids);
    std::vector<EdgeConsensusFrame> grants;
    std::string err;
    assert(replica.handle_frame(requests[0], &grants, &err));
    assert(err.empty());
    assert(grants.size() == 1);
    out.push_back(grants[0]);
  }
  return out;
}

static void test_runtime_core_rejects_incomplete_transport() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
  EdgeConsensusRuntimeTransportOps transport;
  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusRuntimeHooks(), transport, &result, &error);
  assert(!ok);
  assert(error == "runtime transport incomplete");
}

static void test_runtime_core_surfaces_hello_failure() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
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
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusRuntimeHooks(), transport, &result, &error);
  assert(!ok);
  assert(error == "failed to post NODE_HELLO: network down");
}

static void test_runtime_core_returns_structured_stop_result() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
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

  EdgeConsensusRuntimeHooks hooks;
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
  EdgeConsensusRuntimeConfig cfg = make_config();
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
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusRuntimeHooks(), transport, &result, &error);
  assert(ok);
  assert(error.empty());
  assert(!result["ok"].asBool());
  assert(result["error"].asString() == "deadline exceeded before commit");
}

static void test_runtime_core_commits_from_relayed_vote_grants() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
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

  EdgeConsensusRuntimeHooks hooks;
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

static void test_runtime_core_ignores_unrelated_outbox_messages() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
  const std::vector<EdgeConsensusFrame> grants = make_vote_grants_for_node_a(cfg);
  int send_calls = 0;
  int poll_calls = 0;

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
      *out = make_outbox_with_unrelated_message(1);
    } else if (poll_calls == 2) {
      *out = make_outbox_with_frame(grants[0], 2);
    } else {
      *out = Json::Value(Json::objectValue);
    }
    return true;
  };

  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, EdgeConsensusRuntimeHooks(), transport, &result, &error);
  assert(ok);
  assert(error.empty());
  assert(result["ok"].asBool());
  assert(result["leader_node_id"].asString() == "node-a");
  assert(send_calls == 2);
  assert(poll_calls == 2);
}

static void test_runtime_core_adopts_membership_bundle_before_vote_grants() {
  EdgeConsensusRuntimeConfig cfg = make_config();
  cfg.campaign_delay_ms = 100000;

  EdgeConsensusRuntimeConfig next_cfg = cfg;
  next_cfg.membership_epoch = 10;
  next_cfg.member_node_ids = {"node-a", "node-b"};
  next_cfg.peer_node_ids = {"node-b"};
  next_cfg.cluster_size = 2;
  next_cfg.campaign_delay_ms = 0;
  next_cfg.campaign_retry_ms = 25;
  next_cfg.campaign_retry_max_ms = 100;
  next_cfg.campaign_retry_backoff_factor = 2;
  next_cfg.leader_heartbeat_ms = 50;
  next_cfg.leader_lease_ms = 200;
  next_cfg.lease_expiry_recampaign_delay_ms = 75;
  const std::vector<EdgeConsensusFrame> grants = make_vote_grants_for_node_a(next_cfg);
  assert(grants.size() == 1);

  int send_calls = 0;
  int poll_calls = 0;
  Json::Value statuses(Json::arrayValue);
  Json::Value log_lines(Json::arrayValue);

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
    assert(frame.from.membership_epoch == 10);
    assert(targets.size() == 1);
    assert(targets[0] == "node-b");
    if (send_calls == 1) {
      assert(frame.kind == "vote_request");
    } else {
      assert(frame.kind == "leader_commit");
    }
    return true;
  };
  transport.poll_outbox = [&poll_calls, &grants](int64_t, Json::Value* out, std::string*) {
    poll_calls++;
    if (!out) return true;
    if (poll_calls == 1) {
      *out = make_outbox_with_membership_bundle(10, {"node-a", "node-b"}, 1);
    } else if (poll_calls == 2) {
      *out = make_outbox_with_frame(grants[0], 2);
    } else {
      *out = Json::Value(Json::objectValue);
    }
    return true;
  };

  EdgeConsensusRuntimeHooks hooks;
  hooks.status_update = [&statuses](const Json::Value& status) { statuses.append(status); };
  hooks.log_line = [&log_lines](const std::string& line) { log_lines.append(line); };

  Json::Value result(Json::nullValue);
  std::string error;
  const bool ok = run_edge_consensus_runtime_core(cfg, hooks, transport, &result, &error);
  assert(ok);
  assert(error.empty());
  assert(result["ok"].asBool());
  assert(result["leader_node_id"].asString() == "node-a");
  assert(result["status"]["self"]["membership_epoch"].asUInt64() == 10);
  assert(result["status"]["member_node_ids"].size() == 2);
  assert(result["status"]["member_node_ids"][0].asString() == "node-a");
  assert(result["status"]["member_node_ids"][1].asString() == "node-b");
  assert(result["status"]["campaign_retry_ms"].asInt64() == 25);
  assert(result["status"]["campaign_retry_max_ms"].asInt64() == 100);
  assert(result["status"]["leader_heartbeat_ms"].asInt64() == 50);
  assert(result["status"]["leader_lease_ms"].asInt64() == 200);
  assert(send_calls == 2);
  assert(poll_calls == 2);
  bool saw_membership_status = false;
  for (Json::ArrayIndex i = 0; i < statuses.size(); i++) {
    if (statuses[i]["self"]["membership_epoch"].asUInt64() == 10) saw_membership_status = true;
  }
  assert(saw_membership_status);
  bool saw_adopted_log = false;
  for (Json::ArrayIndex i = 0; i < log_lines.size(); i++) {
    if (log_lines[i].asString().find("adopted membership_bundle epoch=10") != std::string::npos) {
      saw_adopted_log = true;
    }
  }
  assert(saw_adopted_log);
}

}  // namespace

int main() {
  test_runtime_core_rejects_incomplete_transport();
  test_runtime_core_surfaces_hello_failure();
  test_runtime_core_returns_structured_stop_result();
  test_runtime_core_returns_deadline_result_without_commit();
  test_runtime_core_commits_from_relayed_vote_grants();
  test_runtime_core_ignores_unrelated_outbox_messages();
  test_runtime_core_adopts_membership_bundle_before_vote_grants();
  return 0;
}
