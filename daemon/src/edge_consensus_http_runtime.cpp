#include "edge_consensus_http_runtime.h"

#include "edge_node_consensus.h"
#include "http_client.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <chrono>
#include <map>
#include <set>
#include <thread>

namespace agentd {
namespace {

static std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path.front() == '/') return base + path;
  return base + "/" + path;
}

static int64_t now_utc_ms() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static std::string json_compact(const Json::Value& root) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  wb["commentStyle"] = "None";
  return Json::writeString(wb, root);
}

static bool parse_json_text(const std::string& text, Json::Value* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out) *out = Json::Value();
  Json::CharReaderBuilder rb;
  rb["collectComments"] = false;
  std::string errs;
  const auto raw = trim_copy(text);
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  Json::Value parsed;
  if (!reader->parse(raw.data(), raw.data() + raw.size(), &parsed, &errs)) {
    if (out_error) *out_error = errs;
    return false;
  }
  if (out) *out = parsed;
  return true;
}

static std::map<std::string, std::string> header_map_with_json(const EdgeConsensusHttpRuntimeConfig& cfg) {
  std::map<std::string, std::string> headers;
  headers["Content-Type"] = "application/json";
  if (!cfg.auth_token.empty()) headers["Authorization"] = "Bearer " + cfg.auth_token;
  return headers;
}

static std::string make_msg_id(const std::string& node_id, uint64_t seq) {
  return node_id + ":consensus-node:" + std::to_string(seq);
}

static bool http_get_json(const std::string& url, const EdgeConsensusHttpRuntimeConfig& cfg, Json::Value* out, std::string* out_error) {
  const int64_t timeout_ms = std::max<int64_t>(250, std::min<int64_t>(5000, cfg.poll_interval_ms * 4));
  const HttpClientResult r =
    http_request(url, "GET", header_map_with_json(cfg), "", timeout_ms, 1024 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (out_error) *out_error = !r.error.empty() ? r.error : "http status " + std::to_string((int)r.http_status);
    return false;
  }
  return parse_json_text(r.response_body, out, out_error);
}

static bool http_post_json(
  const std::string& url,
  const Json::Value& body,
  const EdgeConsensusHttpRuntimeConfig& cfg,
  Json::Value* out,
  std::string* out_error
) {
  const int64_t timeout_ms = std::max<int64_t>(250, std::min<int64_t>(5000, cfg.poll_interval_ms * 4));
  const HttpClientResult r =
    http_request(url, "POST", header_map_with_json(cfg), json_compact(body), timeout_ms, 1024 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (out_error) {
      *out_error = !r.error.empty() ? r.error : "http status " + std::to_string((int)r.http_status);
      if (!trim_copy(r.response_body).empty()) *out_error += " body=" + r.response_body;
    }
    return false;
  }
  if (out) return parse_json_text(r.response_body, out, out_error);
  return true;
}

static bool post_hello(const EdgeConsensusHttpRuntimeConfig& cfg, uint64_t* io_seq, std::string* out_error) {
  if (!io_seq) return false;
  Json::Value env(Json::objectValue);
  env["msg_id"] = make_msg_id(cfg.node_id, ++(*io_seq));
  env["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  env["type"] = "NODE_HELLO";
  env["from"] = "node:" + cfg.node_id;
  env["to"] = "platform";
  Json::Value body(Json::objectValue);
  body["node_id"] = cfg.node_id;
  body["model"] = cfg.model;
  body["fw_git_sha"] = cfg.fw_git_sha;
  body["caps_sha256"] = cfg.manifest_sha256;
  env["body"] = body;
  Json::Value resp;
  return http_post_json(join_base_path(cfg.daemon_url, "/api/v1/edge/message"), env, cfg, &resp, out_error);
}

static bool send_consensus_frame(
  const EdgeConsensusHttpRuntimeConfig& cfg,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& raw_target_node_ids,
  uint64_t* io_seq,
  std::string* out_error
) {
  if (!io_seq) return false;
  std::set<std::string> seen;
  std::vector<std::string> target_node_ids;
  for (const auto& raw : raw_target_node_ids) {
    const std::string nid = trim_copy(raw);
    if (nid.empty() || nid == cfg.node_id) continue;
    if (!seen.insert(nid).second) continue;
    target_node_ids.push_back(nid);
  }
  if (target_node_ids.empty()) return true;

  Json::Value env(Json::objectValue);
  env["msg_id"] = make_msg_id(cfg.node_id, ++(*io_seq));
  env["ts_utc_ms"] = (Json::Int64)now_utc_ms();
  env["type"] = "CONSENSUS_FRAME";
  env["from"] = "node:" + cfg.node_id;
  env["to"] = "platform";

  Json::Value body(Json::objectValue);
  body["frame"] = edge_consensus_frame_to_json(frame);
  if (target_node_ids.size() == 1) {
    body["target_node_id"] = target_node_ids.front();
  } else {
    Json::Value arr(Json::arrayValue);
    for (const auto& nid : target_node_ids) arr.append(nid);
    body["target_node_ids"] = arr;
  }
  env["body"] = body;

  Json::Value resp;
  return http_post_json(join_base_path(cfg.daemon_url, "/api/v1/edge/message"), env, cfg, &resp, out_error);
}

static bool poll_outbox(
  const EdgeConsensusHttpRuntimeConfig& cfg,
  int64_t cursor,
  Json::Value* out,
  std::string* out_error
) {
  const std::string url = join_base_path(
    cfg.daemon_url,
    "/api/v1/edge/outbox?node_id=" + cfg.node_id + "&cursor=" + std::to_string(cursor) +
      "&limit=" + std::to_string((int)cfg.outbox_limit)
  );
  return http_get_json(url, cfg, out, out_error);
}

static void log_line(const EdgeConsensusHttpRuntimeHooks& hooks, const std::string& line) {
  if (hooks.log_line) hooks.log_line(line);
}

}  // namespace

bool run_edge_consensus_http_runtime(
  const EdgeConsensusHttpRuntimeConfig& cfg,
  const EdgeConsensusHttpRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_result) *out_result = Json::Value(Json::nullValue);
  if (!out_result) {
    if (out_error) *out_error = "out_result required";
    return false;
  }

  uint64_t msg_seq = 0;
  std::string err;
  if (!post_hello(cfg, &msg_seq, &err)) {
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
  loop_cfg.decision_sha256 = cfg.decision_sha256;
  EdgeConsensusNodeLoop loop(loop_cfg);
  const int64_t started_ms = now_utc_ms();
  const int64_t deadline_at = started_ms + cfg.deadline_ms;
  int64_t cursor = 0;

  while (now_utc_ms() < deadline_at) {
    if (hooks.stop_requested && hooks.stop_requested->load()) {
      Json::Value result(Json::objectValue);
      result["ok"] = false;
      result["node_id"] = cfg.node_id;
      result["error"] = "stopped";
      result["current_term"] = Json::UInt64(loop.replica().current_term());
      result["status"] = loop.status_to_json();
      *out_result = result;
      return true;
    }

    const std::vector<EdgeConsensusFrame> scheduled = loop.tick(now_utc_ms());
    for (const auto& request : scheduled) {
      const std::vector<std::string> targets = loop.target_node_ids_for_frame(request);
      if (!send_consensus_frame(cfg, request, targets, &msg_seq, &err)) {
        if (out_error) *out_error = "failed to send vote_request: " + err;
        return false;
      }
      log_line(hooks, "sent vote_request term=" + std::to_string((unsigned long long)request.term));
    }

    Json::Value outbox;
    if (!poll_outbox(cfg, cursor, &outbox, &err)) {
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
        if (!loop.handle_frame(frame, &generated, &herr)) {
          if (out_error) *out_error = "failed to handle relayed frame: " + herr;
          return false;
        }
        log_line(hooks, "handled " + frame.kind + " from " + frame.from.node_id + " term=" +
                             std::to_string((unsigned long long)frame.term));
        for (const auto& out_frame : generated) {
          const std::vector<std::string> targets = loop.target_node_ids_for_frame(out_frame);
          if (!send_consensus_frame(cfg, out_frame, targets, &msg_seq, &err)) {
            if (out_error) *out_error = "failed to send generated frame: " + err;
            return false;
          }
          log_line(hooks, "sent " + out_frame.kind + " term=" + std::to_string((unsigned long long)out_frame.term));
        }
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
  *out_result = result;
  return true;
}

}  // namespace agentd
