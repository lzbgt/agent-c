#include "run_request_replay.h"

#include "json_util.h"

#include "agent/json_c14n.h"
#include "tool_loop.h"

#include <json/json.h>

#include <string>

namespace agentd {
namespace {

static constexpr size_t kReplayRequestMaxBytes = 512 * 1024;
static constexpr size_t kReplayResponseMaxBytes = 1024 * 1024;

static void redact_replay_request(Json::Value* v) {
  if (!v) return;
  if (v->isMember("api_key")) v->removeMember("api_key");
  if (v->isMember("Authorization")) v->removeMember("Authorization");
  if (v->isMember("auth_token")) v->removeMember("auth_token");
  if (v->isMember("trace_text")) v->removeMember("trace_text");
  if (v->isMember("http_body")) v->removeMember("http_body");
  if (v->isMember("input_files") && (*v)["input_files"].isArray()) {
    auto& files = (*v)["input_files"];
    for (Json::ArrayIndex i = 0; i < files.size(); i++) {
      auto& item = files[i];
      if (item.isObject() && item.isMember("data_base64")) {
        item.removeMember("data_base64");
      }
    }
  }
}

static void redact_replay_response(Json::Value* v) {
  if (!v) return;
  if (v->isMember("http_body")) v->removeMember("http_body");
  if (v->isMember("trace_text")) v->removeMember("trace_text");
}

static bool json_stringify_capped(
  const Json::Value& v,
  size_t max_bytes,
  std::string* out_json,
  std::string* out_error,
  const char* err_code
) {
  if (out_error) out_error->clear();
  if (out_json) out_json->clear();
  if (!out_json) return false;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  *out_json = Json::writeString(wb, v);
  if (out_json->size() > max_bytes) {
    if (out_error && err_code) *out_error = err_code;
    out_json->clear();
    return false;
  }
  return true;
}

}  // namespace

RunReplayBundle build_run_replay_bundle(
  const Json::Value& request_args,
  const Json::Value& response,
  const ToolLoopResult* tool_loop_result,
  const std::string& trace_id
) {
  RunReplayBundle out;
  auto append_error = [&](const std::string& e) {
    if (e.empty()) return;
    if (!out.error.empty()) out.error.append(";");
    out.error.append(e);
  };

  Json::Value replay_req = request_args;
  if (!trace_id.empty()) replay_req["trace_id"] = trace_id;
  redact_replay_request(&replay_req);
  {
    std::string err_code;
    if (!json_stringify_capped(replay_req, kReplayRequestMaxBytes, &out.request_json, &err_code, "request_json_too_large")) {
      append_error(err_code.empty() ? "request_json_unavailable" : err_code);
    }
  }

  Json::Value replay_resp = response;
  redact_replay_response(&replay_resp);
  {
    std::string err_code;
    if (!json_stringify_capped(replay_resp, kReplayResponseMaxBytes, &out.response_json, &err_code, "response_json_too_large")) {
      append_error(err_code.empty() ? "response_json_unavailable" : err_code);
    }
  }

  Json::Value replay_tools(Json::arrayValue);
  if (tool_loop_result && !tool_loop_result->tool_records.empty()) {
    for (const auto& tr : tool_loop_result->tool_records) {
      Json::Value t(Json::objectValue);
      t["tool_name"] = tr.tool_name;
      if (!tr.tool_call_id.empty()) t["tool_call_id"] = tr.tool_call_id;
      if (!tr.arguments_json.empty()) t["arguments_json"] = tr.arguments_json;
      if (!tr.result_string.empty()) t["result_text"] = tr.result_string;
      if (!tr.result_string_for_prompt.empty()) t["result_for_prompt_text"] = tr.result_string_for_prompt;
      t["result_truncated_for_prompt"] = tr.result_truncated_for_prompt;
      replay_tools.append(t);
    }
  }

  if (!out.request_json.empty() && !out.response_json.empty()) {
    Json::Value bundle(Json::objectValue);
    bundle["schema"] = "run_replay_bundle_v1";
    bundle["request"] = replay_req;
    bundle["response"] = replay_resp;
    bundle["tool_records"] = replay_tools;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string bundle_json = Json::writeString(wb, bundle);
    char token[80] = {0};
    char err_buf[128] = {0};
    const agent_status_t st = agent_json_c14n_sha256_token(bundle_json.data(), bundle_json.size(), token, err_buf, sizeof(err_buf));
    if (st == AGENT_OK) {
      out.sha256 = std::string(token);
      out.sha256_alg = "agent_json_c14n_v1";
      out.sha256_schema = "run_replay_bundle_v1";
    } else {
      append_error(err_buf[0] ? std::string(err_buf) : "replay_hash_failed");
    }
  }

  return out;
}

}  // namespace agentd
