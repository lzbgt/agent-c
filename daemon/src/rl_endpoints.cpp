#include "rl_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace agentd {
namespace {

static std::string json_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool parse_i64(const std::string& s, int64_t* out) {
  if (!out) return false;
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || !end || *end != '\0') return false;
  *out = (int64_t)v;
  return true;
}

static bool parse_double(const std::string& s, double* out) {
  if (!out) return false;
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || !end || *end != '\0') return false;
  *out = v;
  return true;
}

static int64_t query_i64_clamped(
  const std::string& query,
  const char* key,
  int64_t def,
  int64_t minv,
  int64_t maxv
) {
  const auto q = query_get(query, key);
  int64_t v = def;
  if (q && !q->empty()) {
    int64_t parsed = 0;
    if (parse_i64(*q, &parsed)) v = parsed;
  }
  return std::max<int64_t>(minv, std::min<int64_t>(maxv, v));
}

static std::filesystem::path experience_path_for_state_dir(const std::string& state_dir) {
  const std::filesystem::path base =
    state_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(state_dir);
  return base / "rl" / "experience_records.jsonl";
}

static bool string_field_matches(const Json::Value& record, const char* key, const std::string& expected) {
  if (expected.empty()) return true;
  return record.isObject() && record.isMember(key) && record[key].isString() && record[key].asString() == expected;
}

static bool reward_matches(const Json::Value& record, bool has_min, double min_reward, bool has_max, double max_reward) {
  if (!has_min && !has_max) return true;
  if (!record.isObject() || !record.isMember("reward") || !record["reward"].isNumeric()) return false;
  const double reward = record["reward"].asDouble();
  if (has_min && reward < min_reward) return false;
  if (has_max && reward > max_reward) return false;
  return true;
}

}  // namespace

void handle_rl_experience_records_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const int64_t offset = query_i64_clamped(req.query, "offset", 0, 0, std::numeric_limits<int32_t>::max());
  const int64_t limit = query_i64_clamped(req.query, "limit", 50, 1, 500);
  const auto label_q = query_get(req.query, "label");
  const auto workflow_q = query_get(req.query, "workflow_id");
  const auto task_q = query_get(req.query, "task_id");
  const std::string label = label_q ? *label_q : "";
  const std::string workflow_id = workflow_q ? *workflow_q : "";
  const std::string task_id = task_q ? *task_q : "";

  double min_reward = 0.0;
  double max_reward = 0.0;
  bool has_min_reward = false;
  bool has_max_reward = false;
  if (const auto q = query_get(req.query, "min_reward"); q && !q->empty()) {
    if (!parse_double(*q, &min_reward)) {
      resp->status = 400;
      resp->body = json_error_body("min_reward must be numeric");
      return;
    }
    has_min_reward = true;
  }
  if (const auto q = query_get(req.query, "max_reward"); q && !q->empty()) {
    if (!parse_double(*q, &max_reward)) {
      resp->status = 400;
      resp->body = json_error_body("max_reward must be numeric");
      return;
    }
    has_max_reward = true;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["schema"] = "agentd_experience_record_v1";
  out["path"] = "rl/experience_records.jsonl";
  out["offset"] = (Json::Int64)offset;
  out["limit"] = (Json::Int64)limit;

  Json::Value filters(Json::objectValue);
  if (!label.empty()) filters["label"] = label;
  if (!workflow_id.empty()) filters["workflow_id"] = workflow_id;
  if (!task_id.empty()) filters["task_id"] = task_id;
  if (has_min_reward) filters["min_reward"] = min_reward;
  if (has_max_reward) filters["max_reward"] = max_reward;
  if (!filters.empty()) out["filters"] = filters;

  const std::filesystem::path path = experience_path_for_state_dir(cfg.state_dir);
  if (!std::filesystem::exists(path)) {
    out["records"] = Json::Value(Json::arrayValue);
    out["count"] = 0;
    out["next_offset"] = (Json::Int64)offset;
    out["eof"] = true;
    resp->body = json_compact(out);
    return;
  }

  std::ifstream f(path);
  if (!f) {
    resp->status = 500;
    resp->body = json_error_body("failed to open experience records");
    return;
  }

  Json::Value records(Json::arrayValue);
  int64_t line_index = 0;
  int64_t malformed = 0;
  int64_t matched_after_offset = 0;
  bool reached_limit = false;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) {
      line_index++;
      continue;
    }
    if (line_index++ < offset) continue;

    Json::Value record;
    std::string err;
    if (!json_parse_any(line, &record, &err) || !record.isObject()) {
      malformed++;
      continue;
    }
    if (!string_field_matches(record, "label", label)) continue;
    if (!string_field_matches(record, "workflow_id", workflow_id)) continue;
    if (!string_field_matches(record, "task_id", task_id)) continue;
    if (!reward_matches(record, has_min_reward, min_reward, has_max_reward, max_reward)) continue;

    if (matched_after_offset >= limit) {
      reached_limit = true;
      break;
    }
    records.append(record);
    matched_after_offset++;
  }

  out["records"] = records;
  out["count"] = records.size();
  out["next_offset"] = (Json::Int64)line_index;
  out["eof"] = !reached_limit && f.eof();
  if (malformed > 0) out["malformed_lines_skipped"] = (Json::Int64)malformed;
  resp->body = json_compact(out);
}

}  // namespace agentd
