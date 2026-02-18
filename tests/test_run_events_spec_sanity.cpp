#include <json/json.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

static bool read_all(const std::string& path, std::string* out) {
  if (!out) return false;
  out->clear();
  std::ifstream f(path, std::ios::in | std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static bool parse_json_object(const std::string& s, Json::Value* out, std::string* out_err) {
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
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  *out = v;
  return true;
}

static bool ends_with(const std::string& s, const char* suffix) {
  if (!suffix) return false;
  const std::string suf(suffix);
  if (s.size() < suf.size()) return false;
  return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static std::string trim_ws(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}

static bool is_nonneg_int(const Json::Value& v) {
  if (!(v.isInt() || v.isUInt() || v.isInt64() || v.isUInt64())) return false;
  return v.asInt64() >= 0;
}

static bool validate_event_object(const Json::Value& v, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
    if (out_err) *out_err = "missing/invalid type (expected non-empty string)";
    return false;
  }
  if (v.isMember("schema") && !v["schema"].isString()) {
    if (out_err) *out_err = "schema must be string";
    return false;
  }
  if (v.isMember("ts_unix_ms") && !is_nonneg_int(v["ts_unix_ms"])) {
    if (out_err) *out_err = "ts_unix_ms must be integer >= 0";
    return false;
  }
  if (v.isMember("step") && !is_nonneg_int(v["step"])) {
    if (out_err) *out_err = "step must be integer >= 0";
    return false;
  }
  if (v.isMember("epoch") && !is_nonneg_int(v["epoch"])) {
    if (out_err) *out_err = "epoch must be integer >= 0";
    return false;
  }
  if (v.isMember("source") && !v["source"].isString()) {
    if (out_err) *out_err = "source must be string";
    return false;
  }
  if (v.isMember("run_id") && !is_nonneg_int(v["run_id"])) {
    if (out_err) *out_err = "run_id must be integer >= 0";
    return false;
  }
  if (v.isMember("workflow_id") && !v["workflow_id"].isString()) {
    if (out_err) *out_err = "workflow_id must be string";
    return false;
  }
  if (v.isMember("session_id") && !v["session_id"].isString()) {
    if (out_err) *out_err = "session_id must be string";
    return false;
  }
  return true;
}

static bool get_string_field(const Json::Value& v, const char* key, std::string* out) {
  if (!key || !out) return false;
  if (!v.isMember(key) || !v[key].isString()) return false;
  *out = v[key].asString();
  return !out->empty();
}

static bool get_int_field(const Json::Value& v, const char* key, int64_t* out) {
  if (!key || !out) return false;
  if (!v.isMember(key) || !is_nonneg_int(v[key])) return false;
  *out = v[key].asInt64();
  return true;
}

static bool validate_payload_assistant_delta(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "assistant_delta data must be object";
    return false;
  }
  std::string delta;
  if (!get_string_field(data, "delta", &delta)) {
    if (out_err) *out_err = "assistant_delta.data.delta must be non-empty string";
    return false;
  }
  if (data.isMember("step") && !is_nonneg_int(data["step"])) {
    if (out_err) *out_err = "assistant_delta.data.step must be integer >= 0";
    return false;
  }
  if (data.isMember("epoch") && !is_nonneg_int(data["epoch"])) {
    if (out_err) *out_err = "assistant_delta.data.epoch must be integer >= 0";
    return false;
  }
  return true;
}

static bool validate_payload_assistant_message(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "assistant_message data must be object";
    return false;
  }
  std::string content;
  if (!get_string_field(data, "assistant_content", &content)) {
    if (out_err) *out_err = "assistant_message.data.assistant_content must be non-empty string";
    return false;
  }
  int64_t has_tool_calls = 0;
  if (!get_int_field(data, "has_tool_calls", &has_tool_calls)) {
    if (out_err) *out_err = "assistant_message.data.has_tool_calls must be integer >= 0";
    return false;
  }
  if (data.isMember("assistant_mm_json") && !data["assistant_mm_json"].isString()) {
    if (out_err) *out_err = "assistant_message.data.assistant_mm_json must be string";
    return false;
  }
  if (data.isMember("assistant_mm_bytes") && !is_nonneg_int(data["assistant_mm_bytes"])) {
    if (out_err) *out_err = "assistant_message.data.assistant_mm_bytes must be integer >= 0";
    return false;
  }
  if (data.isMember("assistant_mm_truncated") && !is_nonneg_int(data["assistant_mm_truncated"])) {
    if (out_err) *out_err = "assistant_message.data.assistant_mm_truncated must be integer >= 0";
    return false;
  }
  return true;
}

static bool validate_payload_tool_call(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "tool_call data must be object";
    return false;
  }
  std::string tool_name;
  if (!get_string_field(data, "tool_name", &tool_name)) {
    if (out_err) *out_err = "tool_call.data.tool_name must be non-empty string";
    return false;
  }
  std::string tool_call_id;
  if (!get_string_field(data, "tool_call_id", &tool_call_id)) {
    if (out_err) *out_err = "tool_call.data.tool_call_id must be non-empty string";
    return false;
  }
  std::string args_json;
  if (!get_string_field(data, "arguments_json", &args_json)) {
    if (out_err) *out_err = "tool_call.data.arguments_json must be non-empty string";
    return false;
  }
  return true;
}

static bool validate_payload_tool_result(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "tool_result data must be object";
    return false;
  }
  std::string tool_name;
  if (!get_string_field(data, "tool_name", &tool_name)) {
    if (out_err) *out_err = "tool_result.data.tool_name must be non-empty string";
    return false;
  }
  std::string tool_call_id;
  if (!get_string_field(data, "tool_call_id", &tool_call_id)) {
    if (out_err) *out_err = "tool_result.data.tool_call_id must be non-empty string";
    return false;
  }
  std::string content;
  if (!get_string_field(data, "content", &content)) {
    if (out_err) *out_err = "tool_result.data.content must be non-empty string";
    return false;
  }
  return true;
}

static bool validate_payload_llm_usage(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "llm_usage data must be object";
    return false;
  }
  int64_t prompt_tokens = 0;
  int64_t completion_tokens = 0;
  int64_t total_tokens = 0;
  if (!get_int_field(data, "prompt_tokens", &prompt_tokens)) {
    if (out_err) *out_err = "llm_usage.data.prompt_tokens must be integer >= 0";
    return false;
  }
  if (!get_int_field(data, "completion_tokens", &completion_tokens)) {
    if (out_err) *out_err = "llm_usage.data.completion_tokens must be integer >= 0";
    return false;
  }
  if (!get_int_field(data, "total_tokens", &total_tokens)) {
    if (out_err) *out_err = "llm_usage.data.total_tokens must be integer >= 0";
    return false;
  }
  return true;
}

static bool validate_payload_artifact(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "artifact data must be object";
    return false;
  }
  std::string path;
  if (!get_string_field(data, "path", &path)) {
    if (out_err) *out_err = "artifact.data.path must be non-empty string";
    return false;
  }
  std::string kind;
  if (!get_string_field(data, "kind", &kind)) {
    if (out_err) *out_err = "artifact.data.kind must be non-empty string";
    return false;
  }
  if (data.isMember("mime") && !data["mime"].isString()) {
    if (out_err) *out_err = "artifact.data.mime must be string";
    return false;
  }
  if (data.isMember("repeat") && !is_nonneg_int(data["repeat"])) {
    if (out_err) *out_err = "artifact.data.repeat must be integer >= 0";
    return false;
  }
  if (data.isMember("autoplay") && !data["autoplay"].isBool()) {
    if (out_err) *out_err = "artifact.data.autoplay must be boolean";
    return false;
  }
  return true;
}

static bool validate_payload_ui_action(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "ui_action data must be object";
    return false;
  }
  if (!data.isMember("action") || !data["action"].isObject()) {
    if (out_err) *out_err = "ui_action.data.action must be object";
    return false;
  }
  std::string action_type;
  if (!get_string_field(data["action"], "type", &action_type)) {
    if (out_err) *out_err = "ui_action.data.action.type must be non-empty string";
    return false;
  }
  return true;
}

static bool validate_payload_heartbeat(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "heartbeat data must be object";
    return false;
  }
  int64_t phase = 0;
  if (!get_int_field(data, "phase", &phase)) {
    if (out_err) *out_err = "heartbeat.data.phase must be integer >= 0";
    return false;
  }
  return true;
}

static bool validate_payload_error(const Json::Value& data, std::string* out_err) {
  if (!data.isObject()) {
    if (out_err) *out_err = "error data must be object";
    return false;
  }
  std::string reason;
  if (!get_string_field(data, "reason", &reason)) {
    if (out_err) *out_err = "error.data.reason must be non-empty string";
    return false;
  }
  if (data.isMember("error") && !data["error"].isString()) {
    if (out_err) *out_err = "error.data.error must be string";
    return false;
  }
  if (data.isMember("steps_executed") && !is_nonneg_int(data["steps_executed"])) {
    if (out_err) *out_err = "error.data.steps_executed must be integer >= 0";
    return false;
  }
  if (data.isMember("max_steps") && !is_nonneg_int(data["max_steps"])) {
    if (out_err) *out_err = "error.data.max_steps must be integer >= 0";
    return false;
  }
  return true;
}

static bool validate_event_payload(const Json::Value& v, std::string* out_err) {
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  const std::string type = v.isMember("type") && v["type"].isString() ? v["type"].asString() : "";
  if (type.empty()) return true;
  if (!v.isMember("data")) return true;
  const Json::Value& data = v["data"];

  if (type == "assistant_delta") return validate_payload_assistant_delta(data, out_err);
  if (type == "assistant_message") return validate_payload_assistant_message(data, out_err);
  if (type == "tool_call") return validate_payload_tool_call(data, out_err);
  if (type == "tool_result") return validate_payload_tool_result(data, out_err);
  if (type == "llm_usage") return validate_payload_llm_usage(data, out_err);
  if (type == "artifact") return validate_payload_artifact(data, out_err);
  if (type == "ui_action") return validate_payload_ui_action(data, out_err);
  if (type == "heartbeat") return validate_payload_heartbeat(data, out_err);
  if (type == "error") return validate_payload_error(data, out_err);
  return true;
}

static int schema_sanity(const std::string& schema_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(schema_dir)) {
    std::fprintf(stderr, "schema dir missing: %s\n", schema_dir.c_str());
    return 1;
  }

  int files = 0;
  std::unordered_set<std::string> seen;
  for (const auto& ent : fs::directory_iterator(schema_dir)) {
    if (!ent.is_regular_file()) continue;
    const std::string path = ent.path().string();
    if (!ends_with(path, ".json")) continue;
    files++;
    seen.insert(ent.path().filename().string());

    std::string s;
    if (!read_all(path, &s)) {
      std::fprintf(stderr, "failed to read schema: %s\n", path.c_str());
      return 1;
    }
    Json::Value v;
    std::string err;
    if (!parse_json_object(s, &v, &err)) {
      std::fprintf(stderr, "invalid schema JSON: %s: %s\n", path.c_str(), err.c_str());
      return 1;
    }
    if (!v.isMember("$schema") || !v["$schema"].isString() || v["$schema"].asString().empty()) {
      std::fprintf(stderr, "schema missing $schema: %s\n", path.c_str());
      return 1;
    }
    if (!v.isMember("title") || !v["title"].isString() || v["title"].asString().empty()) {
      std::fprintf(stderr, "schema missing title: %s\n", path.c_str());
      return 1;
    }
    if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
      std::fprintf(stderr, "schema missing type: %s\n", path.c_str());
      return 1;
    }
    if (v["type"].asString() != "object") {
      std::fprintf(stderr, "schema unexpected type (expected object): %s\n", path.c_str());
      return 1;
    }
  }

  if (files == 0) {
    std::fprintf(stderr, "no schema files found under: %s\n", schema_dir.c_str());
    return 1;
  }

  const char* required[] = {
    "run_event_v1.schema.json",
    "run_event_payload_assistant_delta_v1.schema.json",
    "run_event_payload_assistant_message_v1.schema.json",
    "run_event_payload_tool_call_v1.schema.json",
    "run_event_payload_tool_result_v1.schema.json",
    "run_event_payload_llm_usage_v1.schema.json",
    "run_event_payload_artifact_v1.schema.json",
    "run_event_payload_ui_action_v1.schema.json",
    "run_event_payload_heartbeat_v1.schema.json",
    "run_event_payload_error_v1.schema.json",
  };
  for (const char* name : required) {
    if (!name) continue;
    if (seen.find(name) == seen.end()) {
      std::fprintf(stderr, "missing required schema file: %s/%s\n", schema_dir.c_str(), name);
      return 1;
    }
  }
  return 0;
}

static int fixtures_sanity(const std::string& fixtures_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(fixtures_dir)) {
    std::fprintf(stderr, "fixtures dir missing: %s\n", fixtures_dir.c_str());
    return 1;
  }

  int files = 0;
  int events = 0;
  for (const auto& ent : fs::directory_iterator(fixtures_dir)) {
    if (!ent.is_regular_file()) continue;
    const std::string path = ent.path().string();
    if (!ends_with(path, ".jsonl")) continue;
    files++;

    std::ifstream f(path);
    if (!f) {
      std::fprintf(stderr, "failed to read fixtures: %s\n", path.c_str());
      return 1;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
      line_no++;
      const std::string trimmed = trim_ws(line);
      if (trimmed.empty()) continue;
      Json::Value v;
      std::string err;
      if (!parse_json_object(trimmed, &v, &err)) {
        std::fprintf(stderr, "invalid fixture json: %s:%d: %s\n", path.c_str(), line_no, err.c_str());
        return 1;
      }
      if (!validate_event_object(v, &err)) {
        std::fprintf(stderr, "invalid fixture event: %s:%d: %s\n", path.c_str(), line_no, err.c_str());
        return 1;
      }
      if (!validate_event_payload(v, &err)) {
        std::fprintf(stderr, "invalid fixture payload: %s:%d: %s\n", path.c_str(), line_no, err.c_str());
        return 1;
      }
      events++;
    }
  }

  if (files == 0) {
    std::fprintf(stderr, "no fixture files found under: %s\n", fixtures_dir.c_str());
    return 1;
  }
  if (events == 0) {
    std::fprintf(stderr, "no fixture events found under: %s\n", fixtures_dir.c_str());
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  const std::string root = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  if (root.empty()) {
    std::fprintf(stderr, "usage: test_run_events_spec_sanity <path-to-docs/spec/run-events>\n");
    return 2;
  }
  const std::string schema_dir = root + "/schema";
  const std::string fixtures_dir = root + "/fixtures";
  if (schema_sanity(schema_dir) != 0) return 1;
  return fixtures_sanity(fixtures_dir);
}
