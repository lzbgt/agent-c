#include "edge_consensus_http_runtime.h"

#include "agent_db.h"
#include "edge_node_consensus.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <chrono>
#include <set>
#include <thread>
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

static void local_log_line(const EdgeConsensusHttpRuntimeHooks& hooks, const std::string& line) {
  if (hooks.log_line) hooks.log_line(line);
}

static void local_status_update(const EdgeConsensusHttpRuntimeHooks& hooks, const EdgeConsensusNodeLoop& loop) {
  if (hooks.status_update) hooks.status_update(loop.status_to_json());
}

static void local_notify_startup_ready(const EdgeConsensusHttpRuntimeHooks& hooks) {
  if (hooks.startup_ready) hooks.startup_ready();
}

static bool post_hello_local(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
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

static bool upsert_consensus_health_local(
  AgentDb* db,
  const std::string& node_id,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& target_node_ids,
  const std::string& original_msg_id,
  int64_t now_utc_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  AgentDb::EdgeNodeRow row;
  std::string err;
  if (!db->get_edge_node(node_id, &row, &err)) row.node_id = node_id;
  Json::Value health(Json::objectValue);
  if (!row.health_json.empty()) {
    std::string perr;
    if (!json_parse_any(row.health_json, &health, &perr) || !health.isObject()) {
      health = Json::Value(Json::objectValue);
    }
  }
  Json::Value consensus(Json::objectValue);
  if (health.isMember("consensus") && health["consensus"].isObject()) consensus = health["consensus"];
  consensus["schema"] = "edge_node_consensus_status_v1";
  consensus["updated_utc_ms"] = (Json::Int64)now_utc_ms;
  if (!original_msg_id.empty()) consensus["last_msg_id"] = original_msg_id;
  consensus["last_frame_id"] = frame.frame_id;
  consensus["last_frame_kind"] = frame.kind;
  consensus["current_term"] = Json::UInt64(frame.term);
  if (!frame.decision_sha256.empty()) consensus["decision_sha256"] = frame.decision_sha256;
  if (!frame.candidate_node_id.empty()) consensus["candidate_node_id"] = frame.candidate_node_id;
  if (!frame.leader_node_id.empty()) consensus["leader_node_id"] = frame.leader_node_id;
  if (frame.kind == "vote_grant") consensus["granted"] = frame.granted;
  consensus["from"] = edge_consensus_identity_to_json(frame.from);
  if (!target_node_ids.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& nid : target_node_ids) arr.append(nid);
    consensus["target_node_ids"] = arr;
    consensus["forwarded_count"] = (Json::UInt64)target_node_ids.size();
  } else {
    consensus["forwarded_count"] = (Json::UInt64)0;
    consensus.removeMember("target_node_ids");
  }
  if (!frame.vote_witnesses.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& witness : frame.vote_witnesses) {
      Json::Value w(Json::objectValue);
      w["node_id"] = witness.node_id;
      if (!witness.manifest_sha256.empty()) w["manifest_sha256"] = witness.manifest_sha256;
      w["trust_epochs"] = edge_consensus_epochs_to_json(witness.trust_epochs);
      arr.append(w);
    }
    consensus["vote_witnesses"] = arr;
    consensus["vote_witness_count"] = (Json::UInt64)frame.vote_witnesses.size();
  } else {
    consensus["vote_witness_count"] = (Json::UInt64)0;
    consensus.removeMember("vote_witnesses");
  }
  health["consensus"] = consensus;
  row.health_json = edge_json_stringify_compact(health);
  return db->upsert_edge_node(row, out_error);
}

static bool send_consensus_frame_local(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
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
  if (!upsert_consensus_health_local(db, cfg.node_id, frame, target_node_ids, original_msg_id, now, &err)) {
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

static bool poll_outbox_local(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
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

}  // namespace

bool run_edge_consensus_local_runtime(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
  const EdgeConsensusHttpRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_result) *out_result = Json::Value(Json::nullValue);
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (!out_result) {
    if (out_error) *out_error = "out_result required";
    return false;
  }

  uint64_t msg_seq = 0;
  std::string err;
  if (!post_hello_local(db, cfg, &msg_seq, &err)) {
    if (out_error) *out_error = "failed to post NODE_HELLO: " + err;
    return false;
  }

  EdgeConsensusIdentity self;
  self.cluster_id = cfg.cluster_id;
  self.node_id = cfg.node_id;
  self.manifest_sha256 = cfg.manifest_sha256;
  self.membership_epoch = cfg.membership_epoch;
  self.trust_epochs.trust_roots_epoch = cfg.trust_roots_epoch;
  self.trust_epochs.revocations_epoch = cfg.revocations_epoch;
  self.trust_epochs.cert_roots_epoch = cfg.cert_roots_epoch;

  EdgeConsensusNodeLoopConfig loop_cfg;
  loop_cfg.self = self;
  loop_cfg.peer_node_ids = cfg.peer_node_ids;
  loop_cfg.member_node_ids = cfg.member_node_ids;
  loop_cfg.cluster_size = cfg.cluster_size;
  loop_cfg.campaign_delay_ms = cfg.campaign_delay_ms;
  loop_cfg.campaign_retry_ms = cfg.campaign_retry_ms;
  loop_cfg.campaign_retry_max_ms = cfg.campaign_retry_max_ms;
  loop_cfg.campaign_retry_backoff_factor = cfg.campaign_retry_backoff_factor;
  loop_cfg.leader_heartbeat_ms = cfg.leader_heartbeat_ms;
  loop_cfg.leader_lease_ms = cfg.leader_lease_ms;
  loop_cfg.decision_sha256 = cfg.decision_sha256;
  EdgeConsensusNodeLoop loop(loop_cfg);
  local_notify_startup_ready(hooks);
  local_status_update(hooks, loop);
  const int64_t started_ms = now_utc_ms_local();
  const int64_t deadline_at = started_ms + cfg.deadline_ms;
  int64_t cursor = 0;

  while (now_utc_ms_local() < deadline_at) {
    if (hooks.stop_requested && hooks.stop_requested->load()) {
      Json::Value result(Json::objectValue);
      result["ok"] = false;
      result["node_id"] = cfg.node_id;
      result["error"] = "stopped";
      result["current_term"] = Json::UInt64(loop.replica().current_term());
      result["status"] = loop.status_to_json();
      local_status_update(hooks, loop);
      *out_result = result;
      return true;
    }

    const std::vector<EdgeConsensusFrame> scheduled = loop.tick(now_utc_ms_local());
    for (const auto& request : scheduled) {
      const std::vector<std::string> targets = loop.target_node_ids_for_frame(request);
      if (!send_consensus_frame_local(db, cfg, request, targets, &msg_seq, &err)) {
        if (out_error) *out_error = "failed to send vote_request: " + err;
        return false;
      }
      local_log_line(hooks, "sent vote_request term=" + std::to_string((unsigned long long)request.term));
      local_status_update(hooks, loop);
    }

    Json::Value outbox;
    if (!poll_outbox_local(db, cfg, cursor, &outbox, &err)) {
      if (out_error) *out_error = "failed to poll outbox: " + err;
      return false;
    }
    if (outbox.isMember("cursor_next") && outbox["cursor_next"].isInt64()) {
      cursor = std::max(cursor, outbox["cursor_next"].asInt64());
    }

    bool processed_message = false;
    if (outbox.isMember("messages") && outbox["messages"].isArray()) {
      for (Json::ArrayIndex i = 0; i < outbox["messages"].size(); i++) {
        const Json::Value row = outbox["messages"][i];
        if (!row.isObject() || !row.isMember("msg") || !row["msg"].isObject()) continue;
        const Json::Value env = row["msg"];
        const std::string type = env.isMember("type") && env["type"].isString() ? trim_copy(env["type"].asString()) : "";
        if (type != "CONSENSUS_FRAME") continue;
        const Json::Value body = env.isMember("body") && env["body"].isObject() ? env["body"] : Json::Value(Json::objectValue);
        if (!body.isMember("frame") || !body["frame"].isObject()) continue;
        EdgeConsensusFrame frame;
        std::string ferr;
        if (!edge_consensus_frame_from_json(body["frame"], &frame, &ferr)) {
          if (out_error) *out_error = "invalid relayed consensus frame: " + ferr;
          return false;
        }
        std::vector<EdgeConsensusFrame> generated;
        std::string herr;
        if (!loop.handle_frame(frame, &generated, &herr, now_utc_ms_local())) {
          if (out_error) *out_error = "failed to handle relayed frame: " + herr;
          return false;
        }
        local_log_line(hooks, "handled " + frame.kind + " from " + frame.from.node_id + " term=" +
                               std::to_string((unsigned long long)frame.term));
        for (const auto& out_frame : generated) {
          const std::vector<std::string> targets = loop.target_node_ids_for_frame(out_frame);
          if (!send_consensus_frame_local(db, cfg, out_frame, targets, &msg_seq, &err)) {
            if (out_error) *out_error = "failed to send generated frame: " + err;
            return false;
          }
          local_log_line(hooks, "sent " + out_frame.kind + " term=" + std::to_string((unsigned long long)out_frame.term));
        }
        local_status_update(hooks, loop);
        processed_message = true;
      }
    }

    if (!trim_copy(loop.committed_decision_sha256()).empty()) {
      Json::Value result(Json::objectValue);
      result["ok"] = true;
      result["node_id"] = cfg.node_id;
      result["leader_node_id"] = loop.leader_node_id();
      result["committed_decision_sha256"] = loop.committed_decision_sha256();
      result["current_term"] = Json::UInt64(loop.replica().current_term());
      result["status"] = loop.status_to_json();
      local_status_update(hooks, loop);
      *out_result = result;
      return true;
    }

    if (!processed_message) std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_interval_ms));
  }

  Json::Value result(Json::objectValue);
  result["ok"] = false;
  result["node_id"] = cfg.node_id;
  result["error"] = "deadline exceeded before commit";
  result["current_term"] = Json::UInt64(loop.replica().current_term());
  result["status"] = loop.status_to_json();
  local_status_update(hooks, loop);
  *out_result = result;
  return true;
}

}  // namespace agentd
