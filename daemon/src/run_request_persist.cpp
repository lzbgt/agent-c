#include "run_request_persist.h"

#include "agent_db.h"
#include "scene_store.h"
#include "openai_client.h"
#include "tool_loop.h"
#include "run_request_replay.h"

#include "agent/agent.h"
#include "agent/multimodal_prefix.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace agentd {

void persist_run_request(const RunRequestPersistInput& in) {
  if (!in.db || !in.db->is_open() || !in.session || !in.session_id || !in.args || !in.response_json ||
      !in.trace_id || !in.prompt || !in.tools || !in.run_cfg || !in.err || !in.http_body || !in.job_id ||
      !in.host_policy || !in.assistant_text || !in.events_out) {
    return;
  }

  AgentDb& db = *in.db;

  // Mirror session messages (as of the end of this run).
  std::vector<AgentDb::MessageRow> msgs;
  msgs.reserve(agent_session_message_count(in.session));
  for (size_t i = 0; i < agent_session_message_count(in.session); i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(in.session, i, &v) != AGENT_OK) continue;
    AgentDb::MessageRow row;
    row.role = agent_role_to_string(v.role);
    const std::string content = std::string(v.content ? v.content : "", v.content_len);
    const char* text = nullptr;
    size_t text_len = 0;
    const char* mm_json = nullptr;
    size_t mm_json_len = 0;
    const uint8_t has_mm = agent_parse_multimodal_prefix(
      content.c_str(),
      content.size(),
      &text,
      &text_len,
      &mm_json,
      &mm_json_len
    );
    if (has_mm && mm_json && mm_json_len > 0) {
      row.mm_json.assign(mm_json, mm_json_len);
      row.mm_bytes = (int64_t)mm_json_len;
      row.mm_truncated = 0;
      row.content.assign(text ? text : "", text_len);
    } else {
      row.content = content;
    }
    msgs.push_back(std::move(row));
  }
  (void)db.replace_session_messages(*in.session_id, msgs, in.run_ts_ms, nullptr);

  RunReplayBundle replay = build_run_replay_bundle(
    *in.args,
    *in.response_json,
    in.use_tool_loop ? in.tool_loop_result : nullptr,
    *in.trace_id
  );

  AgentDb::RunRow rr;
  rr.session_id = *in.session_id;
  rr.job_id = *in.job_id;
  rr.ts_unix_ms = in.run_ts_ms;
  rr.prompt = *in.prompt;
  rr.tools = *in.tools;
  rr.model = in.run_cfg->model;
  rr.base_url = in.run_cfg->base_url;
  rr.stream_assistant = in.stream_assistant;
  rr.ok = in.ok;
  rr.steps_executed = (in.use_tool_loop && in.tool_loop_result) ? (int64_t)in.tool_loop_result->steps_executed : 0;
  rr.tool_calls_total =
    (in.use_tool_loop && in.tool_loop_result) ? (int64_t)in.tool_loop_result->tool_records.size() : 0;
  {
    // stop_reason: best-effort extracted from events.
    // - ok=true: "done"
    // - ok=false: error event's `reason` when present, else "error"
    std::string stop_reason = in.ok ? "done" : "error";
    std::string last_err_reason;
    if (in.events_out->isArray()) {
      for (Json::ArrayIndex i = 0; i < in.events_out->size(); i++) {
        const auto& ev = (*in.events_out)[i];
        if (!ev.isObject()) continue;
        const auto& t = ev["type"];
        const auto& d = ev["data"];
        if (!t.isString() || !d.isObject()) continue;
        if (t.asString() == "error") {
          if (d.isMember("reason") && d["reason"].isString()) {
            last_err_reason = d["reason"].asString();
          }
        }
        if (t.asString() == "done") {
          stop_reason = in.ok ? "done" : stop_reason;
        }
        if (t.asString() == "cancelled") {
          if (d.isMember("reason") && d["reason"].isString()) {
            stop_reason = d["reason"].asString();
          } else {
            stop_reason = "cancelled";
          }
        }
      }
    }
    if (!in.ok && !last_err_reason.empty()) {
      stop_reason = last_err_reason;
    }
    rr.stop_reason = stop_reason;
    rr.last_error_reason = last_err_reason;
  }
  rr.request_json = replay.request_json;
  rr.response_json = replay.response_json;
  rr.replay_sha256 = replay.sha256;
  rr.replay_sha256_alg = replay.sha256_alg;
  rr.replay_sha256_schema = replay.sha256_schema;
  rr.replay_error = replay.error;
  if (in.use_tool_loop && in.tool_loop_result) {
    // tool_calls_by_tool_json: compact map for troubleshooting.
    Json::Value m(Json::objectValue);
    for (const auto& tr : in.tool_loop_result->tool_records) {
      if (tr.tool_name.empty()) continue;
      const auto key = tr.tool_name;
      if (!m.isMember(key)) m[key] = (Json::UInt64)0;
      m[key] = (Json::UInt64)(m[key].asUInt64() + 1);
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    rr.tool_calls_by_tool_json = Json::writeString(wb, m);
  }
  rr.error = *in.err;
  rr.http_status = in.http_status;
  rr.http_body = *in.http_body;

  int64_t run_id = 0;
  if (db.insert_run(rr, &run_id, nullptr) && run_id > 0) {
    if (!in.trace_id->empty()) {
      (void)db.backfill_approval_run_id(*in.trace_id, run_id, nullptr);
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    if (in.events_out->isArray()) {
      int64_t last_scene_update_ms = 0;
      auto next_scene_ts_ms = [&]() -> int64_t {
        const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        int64_t v = now_ms;
        if (v <= last_scene_update_ms) v = last_scene_update_ms + 1;
        last_scene_update_ms = v;
        return v;
      };
      for (const auto& ev : *in.events_out) {
        if (!ev.isObject()) continue;
        const auto& t = ev["type"];
        const auto& d = ev["data"];
        if (!t.isString() || !d.isObject()) continue;
        Json::Value d2 = d;
        if (!in.trace_id->empty() && !d2.isMember("trace_id")) d2["trace_id"] = *in.trace_id;
        (void)db.insert_event(run_id, in.run_ts_ms, t.asString(), Json::writeString(wb, d2), nullptr);

        if (t.asString() == "artifact") {
          const auto& art = d["artifact"];
          if (art.isObject()) {
            AgentDb::ArtifactRow ar;
            ar.run_id = run_id;
            ar.ts_unix_ms = in.run_ts_ms;
            ar.session_id = *in.session_id;
            if (d.isMember("tool_call_id") && d["tool_call_id"].isString()) ar.tool_call_id = d["tool_call_id"].asString();
            if (art.isMember("path") && art["path"].isString()) ar.path = art["path"].asString();
            if (art.isMember("kind") && art["kind"].isString()) ar.kind = art["kind"].asString();
            if (art.isMember("mime") && art["mime"].isString()) ar.mime = art["mime"].asString();
            if (art.isMember("title") && art["title"].isString()) ar.title = art["title"].asString();
            if (art.isMember("autoplay") && art["autoplay"].isBool()) ar.autoplay = art["autoplay"].asBool();
            if (art.isMember("repeat") && art["repeat"].isInt()) ar.repeat = std::max(1, art["repeat"].asInt());
            ar.artifact_json = Json::writeString(wb, art);
            // Best-effort; ignore failures (DB is troubleshooting mirror).
            if (!ar.path.empty()) {
              int64_t artifact_id = 0;
              (void)db.insert_artifact(ar, &artifact_id, nullptr);
              if (artifact_id > 0 && art.isMember("blob_id") && art["blob_id"].isString()) {
                const std::string blob_id = art["blob_id"].asString();
                if (!blob_id.empty()) {
                  (void)db.attach_blob_to_artifact(artifact_id, blob_id, nullptr);
                }
              }
              // High-leverage UX: mirror artifacts into the durable server-owned Scene so they're visible
              // in the collaboration surface even after refresh (and even if no client RPCs run).
              (void)scene_store_mirror_artifact(&db, *in.session_id, art, ar.tool_call_id, next_scene_ts_ms(), nullptr);
            }
          }
        }
        if (t.asString() == "scene_apply") {
          // Durable Scene update requested by the agent (server-side; refresh-proof).
          const auto& ops = d["ops"];
          if (ops.isArray()) {
            (void)scene_store_apply_ops(&db, *in.session_id, ops, next_scene_ts_ms(), nullptr, nullptr, nullptr);
          }
        }
        if (t.asString() == "ui_action") {
          const auto& act = d["action"];
          if (act.isObject()) {
            AgentDb::UiActionRow ur;
            ur.run_id = run_id;
            ur.ts_unix_ms = in.run_ts_ms;
            ur.session_id = *in.session_id;
            if (d.isMember("tool_call_id") && d["tool_call_id"].isString()) ur.tool_call_id = d["tool_call_id"].asString();
            if (act.isMember("type") && act["type"].isString()) ur.type = act["type"].asString();
            if (act.isMember("title") && act["title"].isString()) ur.title = act["title"].asString();
            if (act.isMember("message") && act["message"].isString()) ur.message = act["message"].asString();
            if (act.isMember("path") && act["path"].isString()) ur.path = act["path"].asString();
            if (act.isMember("mime") && act["mime"].isString()) ur.mime = act["mime"].asString();
            if (act.isMember("autoplay") && act["autoplay"].isBool()) ur.autoplay = act["autoplay"].asBool();
            if (act.isMember("repeat") && act["repeat"].isInt()) ur.repeat = std::max(1, act["repeat"].asInt());
            ur.action_json = Json::writeString(wb, act);
            (void)db.insert_ui_action(ur, nullptr);
          }
        }
      }
    }
    if (in.use_tool_loop && in.tool_loop_result && !in.tool_loop_result->tool_records.empty()) {
      for (const auto& tr : in.tool_loop_result->tool_records) {
        AgentDb::ToolRecordRow trr;
        trr.run_id = run_id;
        trr.tool_name = tr.tool_name;
        trr.tool_call_id = tr.tool_call_id;
        trr.arguments_json = tr.arguments_json;
        trr.result_text = tr.result_string;
        trr.result_for_prompt_text = tr.result_string_for_prompt;
        trr.result_truncated_for_prompt = tr.result_truncated_for_prompt;
        (void)db.insert_tool_record(trr, nullptr);
      }
    }
  }

  // Append a per-run audit record (used by `/api/v1/session/audit`).
  if (!in.session_id->empty()) {
    Json::Value record(Json::objectValue);
    record["ts_unix_ms"] = (Json::Int64)in.run_ts_ms;
    record["session_id"] = *in.session_id;
    record["trace_id"] = *in.trace_id;
    record["ok"] = in.ok;
    record["model"] = in.run_cfg->model;
    record["base_url"] = in.run_cfg->base_url;
    record["tools"] = *in.tools;
    record["yolo"] = in.yolo;
    record["host_policy"] = *in.host_policy;
    record["prompt"] = *in.prompt;
    record["assistant_text"] = *in.assistant_text;
    if (in.http_status) record["http_status"] = (Json::Int64)in.http_status;
    if (!in.http_body->empty()) record["http_body"] = *in.http_body;
    if (!in.ok) record["error"] = *in.err;
    if (in.events_out->isArray()) {
      record["events"] = *in.events_out;
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    (void)db.insert_audit_record(*in.session_id, in.run_ts_ms, run_id, Json::writeString(wb, record), nullptr);
  }
}

}  // namespace agentd
