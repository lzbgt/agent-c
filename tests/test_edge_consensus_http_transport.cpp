#include "edge_consensus_http_transport.h"

#include "edge_node_consensus.h"

#include <cassert>

namespace {

using agentd::EdgeConsensusFrame;
using agentd::EdgeConsensusRuntimeConfig;
using agentd::build_edge_consensus_http_frame_envelope;
using agentd::build_edge_consensus_http_hello_envelope;
using agentd::build_edge_consensus_http_outbox_poll_url;
using agentd::edge_consensus_http_json_headers;

static EdgeConsensusRuntimeConfig make_cfg() {
  EdgeConsensusRuntimeConfig cfg;
  cfg.daemon_url = "http://127.0.0.1:8123/";
  cfg.auth_token = "daemon-token";
  cfg.node_id = "node-a";
  cfg.cluster_id = "cluster-a";
  cfg.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  cfg.model = "edge_consensus_node";
  cfg.fw_git_sha = "agentd_managed_runtime";
  cfg.outbox_limit = 17;
  return cfg;
}

static EdgeConsensusFrame make_frame() {
  EdgeConsensusFrame frame;
  frame.frame_id = "frame-1";
  frame.kind = "vote_request";
  frame.term = 7;
  frame.from.node_id = "node-a";
  frame.from.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  frame.candidate_node_id = "node-a";
  return frame;
}

static void test_headers_include_json_and_bearer_auth() {
  const auto headers = edge_consensus_http_json_headers(make_cfg());
  assert(headers.at("Content-Type") == "application/json");
  assert(headers.at("Authorization") == "Bearer daemon-token");
}

static void test_hello_envelope_shape() {
  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  const Json::Value env = build_edge_consensus_http_hello_envelope(cfg, "node-a:consensus-node:1", 12345);
  assert(env["msg_id"].asString() == "node-a:consensus-node:1");
  assert(env["ts_utc_ms"].asInt64() == 12345);
  assert(env["type"].asString() == "NODE_HELLO");
  assert(env["from"].asString() == "node:node-a");
  assert(env["to"].asString() == "platform");
  assert(env["body"]["node_id"].asString() == "node-a");
  assert(env["body"]["caps_sha256"].asString() == cfg.manifest_sha256);
}

static void test_frame_envelope_uses_single_target_field_when_possible() {
  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  const Json::Value env = build_edge_consensus_http_frame_envelope(
    cfg, make_frame(), {"node-a", "node-b", "node-b"}, "node-a:consensus-node:2", 22222);
  assert(env["type"].asString() == AGENT_UM_BMP_TYPE_CONSENSUS_FRAME);
  assert(env["body"]["target_node_id"].asString() == "node-b");
  assert(!env["body"].isMember("target_node_ids"));
  assert(env["body"]["frame"]["frame_id"].asString() == "frame-1");
}

static void test_frame_envelope_dedupes_multiple_targets() {
  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  const Json::Value env = build_edge_consensus_http_frame_envelope(
    cfg, make_frame(), {"node-c", "node-b", "node-b", "", "node-a"}, "node-a:consensus-node:3", 33333);
  assert(!env["body"].isMember("target_node_id"));
  assert(env["body"]["target_node_ids"].isArray());
  assert(env["body"]["target_node_ids"].size() == 2);
  assert(env["body"]["target_node_ids"][0].asString() == "node-c");
  assert(env["body"]["target_node_ids"][1].asString() == "node-b");
}

static void test_outbox_poll_url_shape() {
  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  const std::string url = build_edge_consensus_http_outbox_poll_url(cfg, 42);
  assert(url == "http://127.0.0.1:8123/api/v1/edge/outbox?node_id=node-a&cursor=42&limit=17");
}

}  // namespace

int main() {
  test_headers_include_json_and_bearer_auth();
  test_hello_envelope_shape();
  test_frame_envelope_uses_single_target_field_when_possible();
  test_frame_envelope_dedupes_multiple_targets();
  test_outbox_poll_url_shape();
  return 0;
}
