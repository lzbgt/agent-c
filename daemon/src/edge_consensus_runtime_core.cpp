#include "edge_consensus_runtime_core.h"

#include "edge_consensus_runtime_loop_adapter.h"
#include "string_util.h"

#include <chrono>
#include <thread>

namespace agentd {
namespace {

static int64_t now_utc_ms() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static void log_line(const EdgeConsensusRuntimeHooks& hooks, const std::string& line) {
  if (hooks.log_line) hooks.log_line(line);
}

static void status_update(const EdgeConsensusRuntimeHooks& hooks, const EdgeConsensusNodeLoop& loop) {
  if (hooks.status_update) hooks.status_update(loop.status_to_json());
}

static void notify_startup_ready(const EdgeConsensusRuntimeHooks& hooks) {
  if (hooks.startup_ready) hooks.startup_ready();
}

}  // namespace

bool run_edge_consensus_runtime_core(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusRuntimeHooks& hooks,
  const EdgeConsensusRuntimeTransportOps& transport,
  Json::Value* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_result) *out_result = Json::Value(Json::nullValue);
  if (!out_result) {
    if (out_error) *out_error = "out_result required";
    return false;
  }
  if (!transport.post_hello || !transport.send_consensus_frame || !transport.poll_outbox) {
    if (out_error) *out_error = "runtime transport incomplete";
    return false;
  }

  uint64_t msg_seq = 0;
  std::string err;
  if (!transport.post_hello(&msg_seq, &err)) {
    if (out_error) *out_error = "failed to post NODE_HELLO: " + err;
    return false;
  }

  EdgeConsensusNodeLoop loop(edge_consensus_runtime_node_loop_config(cfg));
  notify_startup_ready(hooks);
  status_update(hooks, loop);
  const int64_t started_ms = now_utc_ms();
  const int64_t deadline_at = started_ms + cfg.deadline_ms;
  int64_t cursor = 0;

  while (now_utc_ms() < deadline_at) {
    if (hooks.stop_requested && hooks.stop_requested->load()) {
      Json::Value result = edge_consensus_runtime_loop_result_json(cfg, loop, false, "stopped");
      status_update(hooks, loop);
      *out_result = result;
      return true;
    }

    const std::vector<EdgeConsensusFrame> scheduled = loop.tick(now_utc_ms());
    for (const auto& request : scheduled) {
      const std::vector<std::string> targets = loop.target_node_ids_for_frame(request);
      if (!transport.send_consensus_frame(request, targets, &msg_seq, &err)) {
        if (out_error) *out_error = "failed to send vote_request: " + err;
        return false;
      }
      log_line(hooks, "sent vote_request term=" + std::to_string((unsigned long long)request.term));
      status_update(hooks, loop);
    }

    Json::Value outbox;
    if (!transport.poll_outbox(cursor, &outbox, &err)) {
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
        if (!loop.handle_frame(frame, &generated, &herr, now_utc_ms())) {
          if (out_error) *out_error = "failed to handle relayed frame: " + herr;
          return false;
        }
        log_line(hooks, "handled " + frame.kind + " from " + frame.from.node_id + " term=" +
                             std::to_string((unsigned long long)frame.term));
        for (const auto& out_frame : generated) {
          const std::vector<std::string> targets = loop.target_node_ids_for_frame(out_frame);
          if (!transport.send_consensus_frame(out_frame, targets, &msg_seq, &err)) {
            if (out_error) *out_error = "failed to send generated frame: " + err;
            return false;
          }
          log_line(hooks, "sent " + out_frame.kind + " term=" + std::to_string((unsigned long long)out_frame.term));
        }
        status_update(hooks, loop);
        processed_message = true;
      }
    }

    if (!trim_copy(loop.committed_decision_sha256()).empty()) {
      Json::Value result = edge_consensus_runtime_loop_result_json(cfg, loop, true, "");
      status_update(hooks, loop);
      *out_result = result;
      return true;
    }

    if (!processed_message) std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_interval_ms));
  }

  Json::Value result = edge_consensus_runtime_loop_result_json(
    cfg, loop, false, "deadline exceeded before commit");
  status_update(hooks, loop);
  *out_result = result;
  return true;
}

}  // namespace agentd
