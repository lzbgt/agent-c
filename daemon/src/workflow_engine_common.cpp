#include "workflow_engine_common.h"

#include "json_schema_subset.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentd {
namespace workflow_engine_internal {

int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

bool json_parse_any_value(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = Json::Value(Json::nullValue);
  std::string perr;
  if (!json_parse_any(s, out, &perr)) {
    if (out_err) *out_err = perr;
    return false;
  }
  return true;
}

std::vector<std::string> parse_dep_ids(const std::string& deps_json) {
  std::vector<std::string> out;
  if (deps_json.empty()) return out;
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(deps_json, &v, &err) || !v.isArray()) return out;
  out.reserve(v.size());
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (v[i].isString()) out.push_back(v[i].asString());
  }
  return out;
}

std::string json_get_string(const Json::Value& obj, const char* k) {
  if (!k) return "";
  if (!obj.isObject()) return "";
  const auto& v = obj[k];
  return v.isString() ? v.asString() : "";
}

std::string json_stringify_compact_local(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

bool workflow_is_terminal_status(const std::string& s) {
  return (s == "done" || s == "error" || s == "cancelled");
}

std::string workflow_task_kind_best_effort(const AgentDb::WorkflowTaskRow& t) {
  if (t.request_json.empty()) return "";
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(t.request_json, &v, &err) || !v.isObject()) return "";
  return json_get_string(v, "kind");
}

int64_t workflow_deadline_unix_ms_best_effort(const AgentDb::WorkflowRow& wf) {
  if (wf.deadline_unix_ms > 0) return wf.deadline_unix_ms;
  if (wf.spec_json.empty()) return 0;
  if (wf.spec_json.find("deadline_unix_ms") == std::string::npos) return 0;
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(wf.spec_json, &v, &err) || !v.isObject()) return 0;
  if (!v.isMember("deadline_unix_ms")) return 0;
  const Json::Value& d = v["deadline_unix_ms"];
  if (!(d.isInt64() || d.isUInt64() || d.isInt() || d.isUInt())) return 0;
  const int64_t ms = d.asInt64();
  return ms > 0 ? ms : 0;
}

int workflow_session_weight_best_effort(const AgentDb::WorkflowRow& wf, int max_weight) {
  if (max_weight < 1) max_weight = 1;
  // Session weights only make sense when the workflow is actually session-scoped.
  if (wf.session_id.empty()) return 1;
  if (wf.spec_json.empty()) return 1;
  if (wf.spec_json.find("session_weight") == std::string::npos) return 1;
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(wf.spec_json, &v, &err) || !v.isObject()) return 1;
  if (!v.isMember("session_weight")) return 1;
  const Json::Value& w = v["session_weight"];
  if (!(w.isInt64() || w.isUInt64() || w.isInt() || w.isUInt())) return 1;
  const int64_t n = w.asInt64();
  if (n <= 0) return 1;
  const int clamped = (int)std::min<int64_t>((int64_t)max_weight, n);
  return std::max(1, clamped);
}

WorkflowLimits workflow_limits_best_effort(const AgentDb::WorkflowRow& wf) {
  WorkflowLimits out;
  if (wf.spec_json.empty()) return out;
  if (wf.spec_json.find("workflow_limits") == std::string::npos) return out;
  Json::Value v;
  std::string err;
  if (!json_parse_any_value(wf.spec_json, &v, &err) || !v.isObject()) return out;
  if (!v.isMember("workflow_limits") || !v["workflow_limits"].isObject()) return out;
  const Json::Value lim = v["workflow_limits"];
  auto clamp_i64 = [](int64_t n, int64_t maxv) -> int64_t {
    if (n < 0) return 0;
    if (maxv > 0) return std::min<int64_t>(maxv, n);
    return n;
  };
  if (lim.isMember("max_tool_calls_total") && (lim["max_tool_calls_total"].isInt64() || lim["max_tool_calls_total"].isUInt64() || lim["max_tool_calls_total"].isInt() || lim["max_tool_calls_total"].isUInt())) {
    const int64_t n = lim["max_tool_calls_total"].asInt64();
    out.max_tool_calls_total = clamp_i64(n, 1000000000LL);
  }
  if (lim.isMember("max_steps_total") && (lim["max_steps_total"].isInt64() || lim["max_steps_total"].isUInt64() || lim["max_steps_total"].isInt() || lim["max_steps_total"].isUInt())) {
    const int64_t n = lim["max_steps_total"].asInt64();
    out.max_steps_total = clamp_i64(n, 1000000000LL);
  }
  if (lim.isMember("max_elapsed_ms_total") && (lim["max_elapsed_ms_total"].isInt64() || lim["max_elapsed_ms_total"].isUInt64() || lim["max_elapsed_ms_total"].isInt() || lim["max_elapsed_ms_total"].isUInt())) {
    const int64_t n = lim["max_elapsed_ms_total"].asInt64();
    // clamp to 1 year worth of ms to avoid silly overflows in stats
    out.max_elapsed_ms_total = clamp_i64(n, 365LL * 24LL * 60LL * 60LL * 1000LL);
  }
  if (lim.isMember("max_total_tokens") && (lim["max_total_tokens"].isInt64() || lim["max_total_tokens"].isUInt64() || lim["max_total_tokens"].isInt() || lim["max_total_tokens"].isUInt())) {
    const int64_t n = lim["max_total_tokens"].asInt64();
    // clamp to avoid silly overflows; 1e12 tokens is far beyond any realistic budget.
    out.max_total_tokens = clamp_i64(n, 1000000000000LL);
  }
  return out;
}

bool workflow_run_should_cancel(void* vctx) {
  auto* c = static_cast<WorkflowRunCancelCtx*>(vctx);
  if (!c) return false;

  if (c->reason != WorkflowCancelReason::None) return true;

  const int64_t now = unix_ms_now();
  if (c->deadline_unix_ms > 0 && now > c->deadline_unix_ms) {
    c->reason = WorkflowCancelReason::DeadlineExceeded;
    return true;
  }

  if (!c->db || c->workflow_id.empty()) return false;
  if (c->last_check_unix_ms > 0 && (now - c->last_check_unix_ms) < 50) {
    return false;
  }
  c->last_check_unix_ms = now;
  AgentDb::WorkflowRow wf;
  std::string err;
  if (!c->db->get_workflow(c->workflow_id, &wf, &err)) return false;
  if (wf.cancel_requested) {
    c->reason = WorkflowCancelReason::CancelRequested;
    return true;
  }
  return false;
}

void insert_workflow_event_best_effort(
  AgentDb* db,
  const std::string& workflow_id,
  const std::string& task_id,
  const std::string& type,
  int64_t ts_unix_ms,
  const Json::Value& data
) {
  if (!db) return;
  if (workflow_id.empty() || type.empty()) return;
  AgentDb::WorkflowEventRow ev;
  ev.workflow_id = workflow_id;
  ev.task_id = task_id;
  ev.ts_unix_ms = ts_unix_ms > 0 ? ts_unix_ms : unix_ms_now();
  ev.type = type;
  ev.data_json = json_stringify_compact_local(data.isNull() ? Json::Value(Json::objectValue) : data);
  (void)db->insert_workflow_event(ev, nullptr, nullptr);
}

bool apply_expectations(const std::string& expect_json, const Json::Value& run_out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (expect_json.empty()) return true;
  Json::Value exp;
  std::string perr;
  if (!json_parse_any_value(expect_json, &exp, &perr) || !exp.isObject()) {
    if (out_err) *out_err = "invalid expect_json: " + perr;
    return false;
  }

  // Gather tool usage from run events (tool_call + tool_result are emitted by the core tool loop).
  // This is used for deterministic "correctness constraints" like "must call X" or "must not call Y".
  std::unordered_map<std::string, int64_t> tool_call_count_by_name;
  int64_t tool_calls_total = 0;
  if (run_out.isObject() && run_out.isMember("events") && run_out["events"].isArray()) {
    const auto& events = run_out["events"];
    for (Json::ArrayIndex i = 0; i < events.size(); i++) {
      const auto& ev = events[i];
      if (!ev.isObject()) continue;
      const std::string type = ev.isMember("type") && ev["type"].isString() ? ev["type"].asString() : "";
      if (type != "tool_call" && type != "tool_result") continue;
      const auto& data = ev.isMember("data") && ev["data"].isObject() ? ev["data"] : Json::Value(Json::nullValue);
      const std::string tool =
        data.isObject() && data.isMember("tool_name") && data["tool_name"].isString() ? data["tool_name"].asString()
        : "";
      if (tool.empty()) continue;
      if (type == "tool_call") {
        tool_calls_total++;
        tool_call_count_by_name[tool] += 1;
      }
    }
  }

  if (exp.isMember("ok") && exp["ok"].isBool()) {
    const bool want_ok = exp["ok"].asBool();
    const bool got_ok = run_out.isObject() && run_out.isMember("ok") && run_out["ok"].isBool() && run_out["ok"].asBool();
    if (want_ok != got_ok) {
      if (out_err) *out_err = std::string("expectation failed: ok=") + (want_ok ? "true" : "false");
      return false;
    }
  }

  const std::string assistant_text = json_get_string(run_out, "assistant_text");
  if (exp.isMember("assistant_text_contains")) {
    const auto& v = exp["assistant_text_contains"];
    if (v.isString()) {
      const std::string needle = v.asString();
      if (!needle.empty() && assistant_text.find(needle) == std::string::npos) {
        if (out_err) *out_err = "expectation failed: assistant_text does not contain required substring";
        return false;
      }
    } else if (v.isArray()) {
      for (Json::ArrayIndex i = 0; i < v.size(); i++) {
        if (!v[i].isString()) continue;
        const std::string needle = v[i].asString();
        if (!needle.empty() && assistant_text.find(needle) == std::string::npos) {
          if (out_err) *out_err = "expectation failed: assistant_text does not contain required substring";
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_equals")) {
    const auto& v = exp["json_pointer_equals"];
    Json::Value arr = v;
    if (v.isObject()) {
      arr = Json::Value(Json::arrayValue);
      arr.append(v);
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& row = arr[i];
        if (!row.isObject()) continue;
        const std::string ptr = row.isMember("pointer") && row["pointer"].isString() ? row["pointer"].asString() : "";
        if (ptr.empty()) continue;
        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json_pointer missing: " + ptr;
          return false;
        }
        if (!row.isMember("value")) {
          if (out_err) *out_err = "expectation failed: json_pointer_equals missing value";
          return false;
        }
        if (*got != row["value"]) {
          if (out_err) *out_err = "expectation failed: json_pointer_equals mismatch at " + ptr;
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_exists")) {
    Json::Value arr = exp["json_pointer_exists"];
    if (arr.isString()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        if (!arr[i].isString()) continue;
        const std::string ptr = arr[i].asString();
        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing";
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_regex")) {
    Json::Value arr = exp["json_pointer_regex"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        const std::string ptr =
          item.isMember("pointer") && item["pointer"].isString() ? item["pointer"].asString() : "";
        const std::string pat =
          item.isMember("regex") && item["regex"].isString() ? item["regex"].asString() : "";
        if (ptr.empty() || pat.empty()) continue;

        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing for regex";
          return false;
        }

        std::string s;
        if (got->isString()) s = got->asString();
        else s = json_stringify_compact_local(*got);

        try {
          const std::regex re(pat);
          if (!std::regex_search(s, re)) {
            if (out_err) *out_err = "expectation failed: regex did not match";
            return false;
          }
        } catch (const std::exception& e) {
          if (out_err) *out_err = std::string("invalid regex: ") + e.what();
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_number_between")) {
    Json::Value arr = exp["json_pointer_number_between"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        const std::string ptr =
          item.isMember("pointer") && item["pointer"].isString() ? item["pointer"].asString() : "";
        if (ptr.empty()) continue;

        bool has_min = false;
        bool has_max = false;
        double min_v = 0.0;
        double max_v = 0.0;
        if (item.isMember("min")) has_min = json_value_to_double_best_effort(item["min"], &min_v);
        if (item.isMember("max")) has_max = json_value_to_double_best_effort(item["max"], &max_v);

        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing for numeric bound";
          return false;
        }
        double x = 0.0;
        if (!json_value_to_double_best_effort(*got, &x)) {
          if (out_err) *out_err = "expectation failed: value is not numeric";
          return false;
        }
        if (has_min && x < min_v) {
          if (out_err) *out_err = "expectation failed: value below min";
          return false;
        }
        if (has_max && x > max_v) {
          if (out_err) *out_err = "expectation failed: value above max";
          return false;
        }
      }
    }
  }

  if (exp.isMember("json_pointer_schema")) {
    Json::Value arr = exp["json_pointer_schema"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        if (!item.isMember("pointer") || !item["pointer"].isString()) {
          if (out_err) *out_err = "expectation failed: json_pointer_schema missing pointer";
          return false;
        }
        const std::string ptr = item["pointer"].asString();
        if (!item.isMember("schema") || !item["schema"].isObject()) {
          if (out_err) *out_err = "expectation failed: json_pointer_schema missing schema";
          return false;
        }
        const Json::Value* got = nullptr;
        if (!json_pointer_get(run_out, ptr, &got) || !got) {
          if (out_err) *out_err = "expectation failed: json pointer missing for schema";
          return false;
        }
        std::string schema_err;
        if (!json_schema_subset_validate_best_effort(item["schema"], *got, ptr.empty() ? "$" : ptr, &schema_err)) {
          if (out_err) {
            *out_err = "expectation failed: schema mismatch";
            if (!schema_err.empty()) out_err->append(": ").append(schema_err);
          }
          return false;
        }
      }
    }
  }

  auto normalize_string_or_array = [](const Json::Value& v) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (v.isString()) {
      const std::string s = trim_copy(v.asString());
      if (!s.empty()) out.push_back(s);
    } else if (v.isArray()) {
      for (Json::ArrayIndex i = 0; i < v.size(); i++) {
        if (!v[i].isString()) continue;
        const std::string s = trim_copy(v[i].asString());
        if (!s.empty()) out.push_back(s);
      }
    }
    return out;
  };

  if (exp.isMember("tool_called")) {
    const auto want = normalize_string_or_array(exp["tool_called"]);
    for (const auto& tn : want) {
      if (tool_call_count_by_name[tn] <= 0) {
        if (out_err) *out_err = "expectation failed: required tool was not called: " + tn;
        return false;
      }
    }
  }

  if (exp.isMember("tool_not_called")) {
    const auto forbid = normalize_string_or_array(exp["tool_not_called"]);
    for (const auto& tn : forbid) {
      if (tool_call_count_by_name[tn] > 0) {
        if (out_err) *out_err = "expectation failed: forbidden tool was called: " + tn;
        return false;
      }
    }
  }

  if (exp.isMember("tool_calls_total_between") && exp["tool_calls_total_between"].isObject()) {
    const auto& v = exp["tool_calls_total_between"];
    bool has_min = false;
    bool has_max = false;
    int64_t min_v = 0;
    int64_t max_v = 0;
    if (v.isMember("min") && (v["min"].isInt64() || v["min"].isUInt64() || v["min"].isInt())) {
      has_min = true;
      min_v = v["min"].asInt64();
    }
    if (v.isMember("max") && (v["max"].isInt64() || v["max"].isUInt64() || v["max"].isInt())) {
      has_max = true;
      max_v = v["max"].asInt64();
    }
    if (has_min && tool_calls_total < min_v) {
      if (out_err) *out_err = "expectation failed: tool_calls_total below min";
      return false;
    }
    if (has_max && tool_calls_total > max_v) {
      if (out_err) *out_err = "expectation failed: tool_calls_total above max";
      return false;
    }
  }

  if (exp.isMember("tool_calls_for_tool_between")) {
    Json::Value arr = exp["tool_calls_for_tool_between"];
    if (arr.isObject()) {
      Json::Value one(Json::arrayValue);
      one.append(arr);
      arr = one;
    }
    if (arr.isArray()) {
      for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
        const auto& item = arr[i];
        if (!item.isObject()) continue;
        const std::string tool =
          item.isMember("tool") && item["tool"].isString() ? trim_copy(item["tool"].asString()) : "";
        if (tool.empty()) continue;
        const int64_t cnt = tool_call_count_by_name.count(tool) ? tool_call_count_by_name[tool] : 0;
        bool has_min = false;
        bool has_max = false;
        int64_t min_v = 0;
        int64_t max_v = 0;
        if (item.isMember("min") && (item["min"].isInt64() || item["min"].isUInt64() || item["min"].isInt())) {
          has_min = true;
          min_v = item["min"].asInt64();
        }
        if (item.isMember("max") && (item["max"].isInt64() || item["max"].isUInt64() || item["max"].isInt())) {
          has_max = true;
          max_v = item["max"].asInt64();
        }
        if (has_min && cnt < min_v) {
          if (out_err) *out_err = "expectation failed: tool call count below min for tool: " + tool;
          return false;
        }
        if (has_max && cnt > max_v) {
          if (out_err) *out_err = "expectation failed: tool call count above max for tool: " + tool;
          return false;
        }
      }
    }
  }

  return true;
}

int64_t retry_backoff_ms(int attempt) {
  if (attempt <= 0) return 0;
  // Quadratic-ish backoff with cap.
  int64_t ms = (int64_t)attempt * (int64_t)attempt * 250;
  if (ms < 250) ms = 250;
  if (ms > 60 * 1000) ms = 60 * 1000;
  return ms;
}

}  // namespace workflow_engine_internal
}  // namespace agentd
