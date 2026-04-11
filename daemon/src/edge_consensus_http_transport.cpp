#include "edge_consensus_http_transport.h"

#include "edge_node_consensus.h"
#include "http_client.h"
#include "json_util.h"
#include "string_util.h"

#include <chrono>
#include <map>
#include <set>

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

static std::string make_msg_id(const std::string& node_id, uint64_t seq) {
  return node_id + ":consensus-node:" + std::to_string(seq);
}

static bool http_get_json(const std::string& url, const EdgeConsensusRuntimeConfig& cfg, Json::Value* out, std::string* out_error) {
  const int64_t timeout_ms = std::max<int64_t>(250, std::min<int64_t>(5000, cfg.poll_interval_ms * 4));
  const HttpClientResult r =
    http_request(url, "GET", edge_consensus_http_json_headers(cfg), "", timeout_ms, 1024 * 1024, "", nullptr);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    if (out_error) *out_error = !r.error.empty() ? r.error : "http status " + std::to_string((int)r.http_status);
    return false;
  }
  return parse_json_text(r.response_body, out, out_error);
}

static bool http_post_json(
  const std::string& url,
  const Json::Value& body,
  const EdgeConsensusRuntimeConfig& cfg,
  Json::Value* out,
  std::string* out_error
) {
  const int64_t timeout_ms = std::max<int64_t>(250, std::min<int64_t>(5000, cfg.poll_interval_ms * 4));
  const HttpClientResult r =
    http_request(url, "POST", edge_consensus_http_json_headers(cfg), json_compact(body), timeout_ms, 1024 * 1024, "", nullptr);
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

}  // namespace

std::map<std::string, std::string> edge_consensus_http_json_headers(
  const EdgeConsensusRuntimeConfig& cfg
) {
  std::map<std::string, std::string> headers;
  headers["Content-Type"] = "application/json";
  if (!cfg.auth_token.empty()) headers["Authorization"] = "Bearer " + cfg.auth_token;
  return headers;
}

Json::Value build_edge_consensus_http_hello_envelope(
  const EdgeConsensusRuntimeConfig& cfg,
  const std::string& msg_id,
  int64_t ts_utc_ms
) {
  Json::Value env(Json::objectValue);
  env["msg_id"] = msg_id;
  env["ts_utc_ms"] = (Json::Int64)ts_utc_ms;
  env["type"] = "NODE_HELLO";
  env["from"] = "node:" + cfg.node_id;
  env["to"] = "platform";
  Json::Value body(Json::objectValue);
  body["node_id"] = cfg.node_id;
  body["model"] = cfg.model;
  body["fw_git_sha"] = cfg.fw_git_sha;
  body["caps_sha256"] = cfg.manifest_sha256;
  env["body"] = body;
  return env;
}

Json::Value build_edge_consensus_http_frame_envelope(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& raw_target_node_ids,
  const std::string& msg_id,
  int64_t ts_utc_ms
) {
  std::set<std::string> seen;
  std::vector<std::string> target_node_ids;
  for (const auto& raw : raw_target_node_ids) {
    const std::string nid = trim_copy(raw);
    if (nid.empty() || nid == cfg.node_id) continue;
    if (!seen.insert(nid).second) continue;
    target_node_ids.push_back(nid);
  }

  Json::Value env(Json::objectValue);
  env["msg_id"] = msg_id;
  env["ts_utc_ms"] = (Json::Int64)ts_utc_ms;
  env["type"] = AGENT_UM_BMP_TYPE_CONSENSUS_FRAME;
  env["from"] = "node:" + cfg.node_id;
  env["to"] = "platform";

  Json::Value body(Json::objectValue);
  body["frame"] = edge_consensus_frame_to_json(frame);
  if (target_node_ids.size() == 1) {
    body["target_node_id"] = target_node_ids.front();
  } else if (!target_node_ids.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& nid : target_node_ids) arr.append(nid);
    body["target_node_ids"] = arr;
  }
  env["body"] = body;
  return env;
}

std::string build_edge_consensus_http_outbox_poll_url(
  const EdgeConsensusRuntimeConfig& cfg,
  int64_t cursor
) {
  return join_base_path(
    cfg.daemon_url,
    "/api/v1/edge/outbox?node_id=" + cfg.node_id + "&cursor=" + std::to_string(cursor) +
      "&limit=" + std::to_string((int)cfg.outbox_limit)
  );
}

EdgeConsensusRuntimeTransportOps make_edge_consensus_http_transport(
  const EdgeConsensusRuntimeConfig& cfg
) {
  EdgeConsensusRuntimeTransportOps transport;
  transport.post_hello = [cfg](uint64_t* io_seq, std::string* out_error) {
    if (!io_seq) {
      if (out_error) *out_error = "msg sequence counter required";
      return false;
    }
    const Json::Value env = build_edge_consensus_http_hello_envelope(
      cfg, make_msg_id(cfg.node_id, ++(*io_seq)), now_utc_ms());
    Json::Value resp(Json::nullValue);
    return http_post_json(join_base_path(cfg.daemon_url, "/api/v1/edge/message"), env, cfg, &resp, out_error);
  };
  transport.send_consensus_frame =
    [cfg](
      const EdgeConsensusFrame& frame,
      const std::vector<std::string>& raw_target_node_ids,
      uint64_t* io_seq,
      std::string* out_error
    ) {
      if (!io_seq) {
        if (out_error) *out_error = "msg sequence counter required";
        return false;
      }
      const Json::Value env = build_edge_consensus_http_frame_envelope(
        cfg, frame, raw_target_node_ids, make_msg_id(cfg.node_id, ++(*io_seq)), now_utc_ms());
      if (!env["body"].isObject() || (!env["body"].isMember("target_node_id") && !env["body"].isMember("target_node_ids"))) {
        return true;
      }
      Json::Value resp(Json::nullValue);
      return http_post_json(join_base_path(cfg.daemon_url, "/api/v1/edge/message"), env, cfg, &resp, out_error);
    };
  transport.poll_outbox = [cfg](int64_t cursor, Json::Value* out, std::string* out_error) {
    return http_get_json(build_edge_consensus_http_outbox_poll_url(cfg, cursor), cfg, out, out_error);
  };
  return transport;
}

}  // namespace agentd
