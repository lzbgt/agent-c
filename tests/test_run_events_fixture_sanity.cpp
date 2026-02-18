#include <json/json.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static bool parse_json_any(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) *out_err = errs;
    return false;
  }
  *out = v;
  return true;
}

static bool is_nonnegative_int(const Json::Value& v) {
  if (!v.isInt64() && !v.isInt()) return false;
  return v.asInt64() >= 0;
}

static const char* expected_schema_for_type(const std::string& type) {
  if (type == "assistant_delta") return "run_event_payload_assistant_delta_v1";
  if (type == "assistant_message") return "run_event_payload_assistant_message_v1";
  if (type == "user_message") return "run_event_payload_user_message_v1";
  if (type == "tool_call") return "run_event_payload_tool_call_v1";
  if (type == "tool_result") return "run_event_payload_tool_result_v1";
  if (type == "llm_usage") return "run_event_payload_llm_usage_v1";
  if (type == "artifact") return "run_event_payload_artifact_v1";
  if (type == "ui_action") return "run_event_payload_ui_action_v1";
  if (type == "heartbeat") return "run_event_payload_heartbeat_v1";
  if (type == "error") return "run_event_payload_error_v1";
  if (type == "task_status") return "run_event_payload_task_status_v1";
  if (type == "workflow_status") return "run_event_payload_workflow_status_v1";
  if (type == "workflow_done") return "run_event_payload_workflow_done_v1";
  if (type == "workflow_budget_exceeded") return "run_event_payload_workflow_budget_exceeded_v1";
  if (type == "memory_checkpoint") return "run_event_payload_memory_checkpoint_v1";
  if (type == "workflow_created") return "run_event_payload_workflow_created_v1";
  if (type == "workflow_cancel_requested") return "run_event_payload_workflow_cancel_requested_v1";
  if (type == "workflow_canceled") return "run_event_payload_workflow_canceled_v1";
  if (type == "step_state") return "run_event_payload_step_state_v1";
  if (type == "step_retry_scheduled") return "run_event_payload_step_retry_scheduled_v1";
  if (type == "step_dispatched") return "run_event_payload_step_dispatched_v1";
  return nullptr;
}

int main(int argc, char** argv) {
  const std::string fixtures_path = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  if (fixtures_path.empty()) {
    std::fprintf(stderr, "usage: test_run_events_fixture_sanity <path-to-fixture.jsonl>\n");
    return 2;
  }

  std::ifstream f(fixtures_path);
  if (!f) {
    std::fprintf(stderr, "failed to read fixture: %s\n", fixtures_path.c_str());
    return 1;
  }

  std::string line;
  int line_no = 0;
  while (std::getline(f, line)) {
    line_no++;
    if (line.empty()) continue;
    Json::Value v;
    std::string err;
    if (!parse_json_any(line, &v, &err)) {
      std::fprintf(stderr, "invalid JSON at line %d: %s\n", line_no, err.c_str());
      return 1;
    }
    if (!v.isObject()) {
      std::fprintf(stderr, "expected object at line %d\n", line_no);
      return 1;
    }
    if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
      std::fprintf(stderr, "missing type at line %d\n", line_no);
      return 1;
    }
    const std::string type = v["type"].asString();
    const char* expected_schema = expected_schema_for_type(type);
    if (expected_schema) {
      if (!v.isMember("schema") || !v["schema"].isString() || v["schema"].asString() != expected_schema) {
        std::fprintf(stderr, "schema mismatch for type=%s at line %d\n", type.c_str(), line_no);
        return 1;
      }
    }
    if (v.isMember("schema") && (!v["schema"].isString() || v["schema"].asString().empty())) {
      std::fprintf(stderr, "invalid schema at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("ts_unix_ms") && !is_nonnegative_int(v["ts_unix_ms"])) {
      std::fprintf(stderr, "invalid ts_unix_ms at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("step") && !is_nonnegative_int(v["step"])) {
      std::fprintf(stderr, "invalid step at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("epoch") && !is_nonnegative_int(v["epoch"])) {
      std::fprintf(stderr, "invalid epoch at line %d\n", line_no);
      return 1;
    }
  }
  return 0;
}
