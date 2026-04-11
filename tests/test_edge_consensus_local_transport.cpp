#include "agent_db.h"
#include "edge_consensus_local_transport.h"
#include "edge_node_consensus.h"
#include "json_util.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::EdgeConsensusFrame;
using agentd::EdgeConsensusRuntimeConfig;
using agentd::poll_edge_consensus_local_outbox;
using agentd::post_edge_consensus_local_hello;
using agentd::send_edge_consensus_local_frame;

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

static EdgeConsensusRuntimeConfig make_cfg() {
  EdgeConsensusRuntimeConfig cfg;
  cfg.node_id = "node-a";
  cfg.cluster_id = "cluster-a";
  cfg.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  cfg.model = "edge_consensus_node";
  cfg.fw_git_sha = "agentd_managed_runtime";
  cfg.outbox_limit = 32;
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

static void test_post_hello_upserts_node_and_caps_request() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_local_transport_hello");
  AgentDb db;
  open_test_db(db_path, &db);

  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  uint64_t io_seq = 0;
  std::string err;
  assert(post_edge_consensus_local_hello(&db, cfg, &io_seq, &err));
  assert(err.empty());
  assert(io_seq == 1);

  AgentDb::EdgeNodeRow row;
  assert(db.get_edge_node(cfg.node_id, &row, &err));
  assert(row.caps_sha256 == cfg.manifest_sha256);
  assert(row.model == cfg.model);

  std::vector<AgentDb::EdgeOutboxMessageRow> msgs;
  assert(db.list_edge_outbox_messages(cfg.node_id, 0, 10, &msgs, &err));
  assert(msgs.size() == 1);
  Json::Value env(Json::nullValue);
  std::string perr;
  assert(agentd::json_parse_any(msgs[0].envelope_json, &env, &perr));
  assert(env["type"].asString() == "PLATFORM_CAPS_REQ");
#endif
}

static void test_send_frame_dedupes_targets_and_updates_health() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_local_transport_frame");
  AgentDb db;
  open_test_db(db_path, &db);

  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  uint64_t io_seq = 0;
  std::string err;
  assert(post_edge_consensus_local_hello(&db, cfg, &io_seq, &err));

  const EdgeConsensusFrame frame = make_frame();
  assert(send_edge_consensus_local_frame(
    &db, cfg, frame, {"node-a", "node-b", "node-b", "node-c", ""}, &io_seq, &err));
  assert(err.empty());
  assert(io_seq == 2);

  AgentDb::EdgeNodeRow row;
  assert(db.get_edge_node(cfg.node_id, &row, &err));
  Json::Value health(Json::nullValue);
  std::string perr;
  assert(agentd::json_parse_any(row.health_json, &health, &perr));
  const Json::Value consensus = health["consensus"];
  assert(consensus["last_frame_id"].asString() == "frame-1");
  assert(consensus["forwarded_count"].asUInt64() == 2);
  assert(consensus["target_node_ids"].size() == 2);
  assert(consensus["target_node_ids"][0].asString() == "node-b");
  assert(consensus["target_node_ids"][1].asString() == "node-c");

  std::vector<AgentDb::EdgeOutboxMessageRow> msgs_b;
  std::vector<AgentDb::EdgeOutboxMessageRow> msgs_c;
  assert(db.list_edge_outbox_messages("node-b", 0, 10, &msgs_b, &err));
  assert(db.list_edge_outbox_messages("node-c", 0, 10, &msgs_c, &err));
  assert(msgs_b.size() == 1);
  assert(msgs_c.size() == 1);
  Json::Value env_b(Json::nullValue);
  assert(agentd::json_parse_any(msgs_b[0].envelope_json, &env_b, &perr));
  assert(env_b["type"].asString() == AGENT_UM_BMP_TYPE_CONSENSUS_FRAME);
  assert(env_b["body"]["relay_from"].asString() == "node-a");
  assert(env_b["body"]["frame"]["frame_id"].asString() == "frame-1");
#endif
}

static void test_poll_outbox_returns_cursor_and_parsed_messages() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_local_transport_poll");
  AgentDb db;
  open_test_db(db_path, &db);

  const EdgeConsensusRuntimeConfig cfg = make_cfg();
  uint64_t io_seq = 0;
  std::string err;
  assert(post_edge_consensus_local_hello(&db, cfg, &io_seq, &err));

  Json::Value out(Json::nullValue);
  assert(poll_edge_consensus_local_outbox(&db, cfg, 0, &out, &err));
  assert(err.empty());
  assert(out["ok"].asBool());
  assert(out["node_id"].asString() == "node-a");
  assert(out["messages"].size() == 1);
  assert(out["messages"][0]["msg"]["type"].asString() == "PLATFORM_CAPS_REQ");
  assert(out["cursor_next"].asInt64() >= out["messages"][0]["outbox_id"].asInt64());
#endif
}

}  // namespace

int main() {
  test_post_hello_upserts_node_and_caps_request();
  test_send_frame_dedupes_targets_and_updates_health();
  test_poll_outbox_returns_cursor_and_parsed_messages();
  return 0;
}
