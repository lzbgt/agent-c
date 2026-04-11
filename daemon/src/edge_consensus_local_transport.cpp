#include "edge_consensus_local_transport.h"

#include "agent_db.h"
#include "edge_consensus_status.h"
#include "edge_node_consensus.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <chrono>
#include <set>
#include <vector>

namespace agentd {
namespace {

static int64_t now_utc_ms_local() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static std::string make_msg_id_local(const std::string& node_id, uint64_t seq) {
  return node_id + ":consensus-node:" + std::to_string(seq);
}

}  // namespace

bool post_edge_consensus_local_hello(
  AgentDb* db,
  const EdgeConsensusRuntimeConfig& cfg,
  uint64_t* io_seq,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (!io_seq) {
    if (out_error) *out_error = "msg sequence counter required";
    return false;
  }
  ++(*io_seq);
  const int64_t now = now_utc_ms_local();

  AgentDb::EdgeNodeRow row;
  row.node_id = cfg.node_id;
  row.model = cfg.model;
  row.fw_git_sha = cfg.fw_git_sha;
  row.caps_sha256 = cfg.manifest_sha256;
  row.last_hello_utc_ms = now;
  row.last_heartbeat_utc_ms = now;
  std::string err;
  if (!db->upsert_edge_node(row, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to persist node hello" : err;
    return false;
  }

  bool need_caps = false;
  if (!cfg.manifest_sha256.empty()) {
    AgentDb::EdgeNodeRow existing;
    if (db->get_edge_node(cfg.node_id, &existing, &err)) {
      if (existing.caps_sha256 != cfg.manifest_sha256 || existing.manifest_json.empty()) need_caps = true;
    } else {
      need_caps = true;
    }
  }
  if (!need_caps) return true;

  Json::Value env(Json::objectValue);
  env["msg_id"] = edge_make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)now;
  env["type"] = "PLATFORM_CAPS_REQ";
  env["from"] = "platform";
  env["to"] = edge_node_to_prefix(cfg.node_id);
  Json::Value body(Json::objectValue);
  body["node_id"] = cfg.node_id;
  body["want"] = "full";
  env["body"] = body;

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = cfg.node_id;
  orow.ts_utc_ms = now;
  orow.envelope_json = edge_json_stringify_compact(env);
  if (!db->insert_edge_outbox_message(orow, nullptr, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to enqueue PLATFORM_CAPS_REQ" : err;
    return false;
  }
  return true;
}

bool send_edge_consensus_local_frame(
  AgentDb* db,
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& raw_target_node_ids,
  uint64_t* io_seq,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (!io_seq) {
    if (out_error) *out_error = "msg sequence counter required";
    return false;
  }

  std::set<std::string> seen;
  std::vector<std::string> target_node_ids;
  for (const auto& raw : raw_target_node_ids) {
    const std::string nid = trim_copy(raw);
    if (nid.empty() || nid == cfg.node_id) continue;
    if (!seen.insert(nid).second) continue;
    target_node_ids.push_back(nid);
  }

  const int64_t now = now_utc_ms_local();
  const std::string original_msg_id = make_msg_id_local(cfg.node_id, ++(*io_seq));
  std::string err;
  if (!upsert_edge_node_consensus_health(db, cfg.node_id, frame, target_node_ids, original_msg_id, now, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to persist consensus status" : err;
    return false;
  }

  const Json::Value frame_json = edge_consensus_frame_to_json(frame);
  for (const auto& target_node_id : target_node_ids) {
    Json::Value relay_env(Json::objectValue);
    relay_env["msg_id"] = edge_make_uuidish_msg_id();
    relay_env["ts_utc_ms"] = (Json::Int64)now;
    relay_env["type"] = "CONSENSUS_FRAME";
    relay_env["from"] = "platform";
    relay_env["to"] = edge_node_to_prefix(target_node_id);
    Json::Value relay_body(Json::objectValue);
    relay_body["relay_from"] = cfg.node_id;
    relay_body["original_msg_id"] = original_msg_id;
    relay_body["frame"] = frame_json;
    relay_env["body"] = relay_body;

    AgentDb::EdgeOutboxMessageRow orow;
    orow.node_id = target_node_id;
    orow.ts_utc_ms = now;
    orow.envelope_json = edge_json_stringify_compact(relay_env);
    if (!db->insert_edge_outbox_message(orow, nullptr, &err)) {
      if (out_error) *out_error = err.empty() ? "failed to enqueue consensus relay" : err;
      return false;
    }
  }
  return true;
}

bool poll_edge_consensus_local_outbox(
  AgentDb* db,
  const EdgeConsensusRuntimeConfig& cfg,
  int64_t cursor,
  Json::Value* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out) *out = Json::Value(Json::nullValue);
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  std::vector<AgentDb::EdgeOutboxMessageRow> msgs;
  std::string err;
  if (!db->list_edge_outbox_messages(cfg.node_id, cursor, cfg.outbox_limit, &msgs, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to list outbox" : err;
    return false;
  }
  Json::Value root(Json::objectValue);
  root["ok"] = true;
  root["node_id"] = cfg.node_id;
  root["cursor_base"] = (Json::Int64)cursor;
  Json::Value arr(Json::arrayValue);
  int64_t cursor_next = cursor;
  for (const auto& m : msgs) {
    Json::Value row(Json::objectValue);
    row["outbox_id"] = (Json::Int64)m.outbox_id;
    row["ts_utc_ms"] = (Json::Int64)m.ts_utc_ms;
    Json::Value env;
    std::string perr;
    if (json_parse_any(m.envelope_json, &env, &perr) && env.isObject()) {
      row["msg"] = env;
    } else {
      row["msg_raw"] = m.envelope_json;
      row["parse_error"] = perr;
    }
    arr.append(row);
    cursor_next = std::max(cursor_next, m.outbox_id);
  }
  root["messages"] = arr;
  root["cursor_next"] = (Json::Int64)cursor_next;
  if (out) *out = std::move(root);
  return true;
}

EdgeConsensusRuntimeTransportOps make_edge_consensus_local_transport(
  AgentDb* db,
  const EdgeConsensusRuntimeConfig& cfg
) {
  EdgeConsensusRuntimeTransportOps transport;
  transport.post_hello = [db, cfg](uint64_t* io_seq, std::string* err) {
    return post_edge_consensus_local_hello(db, cfg, io_seq, err);
  };
  transport.send_consensus_frame =
    [db, cfg](
      const EdgeConsensusFrame& frame,
      const std::vector<std::string>& raw_target_node_ids,
      uint64_t* io_seq,
      std::string* err
    ) {
      return send_edge_consensus_local_frame(db, cfg, frame, raw_target_node_ids, io_seq, err);
    };
  transport.poll_outbox = [db, cfg](int64_t cursor, Json::Value* out, std::string* err) {
    return poll_edge_consensus_local_outbox(db, cfg, cursor, out, err);
  };
  return transport;
}

}  // namespace agentd
