#include "run_client_acks.h"

#include "job_manager.h"

#include <json/json.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace agentd {
namespace {

static bool client_event_matches_tool_call_id(const Json::Value& ev, const char* field, const std::string& tool_call_id) {
  if (!ev.isObject() || tool_call_id.empty() || !field) return false;
  const auto& d = ev["data"];
  if (!d.isObject()) return false;
  const auto& f = d[field];
  return f.isString() && f.asString() == tool_call_id;
}

}  // namespace

std::vector<ExpectedClientAck> collect_expected_client_acks(const Json::Value& events_out) {
  std::vector<ExpectedClientAck> out;
  if (!events_out.isArray()) return out;

  for (Json::ArrayIndex i = 0; i < events_out.size(); i++) {
    const auto& ev = events_out[i];
    if (!ev.isObject()) continue;
    const auto& t = ev["type"];
    const auto& d = ev["data"];
    if (!t.isString() || !d.isObject()) continue;

    const std::string type = t.asString();
    if (type == "artifact") {
      ExpectedClientAck ex;
      ex.category = "artifact";
      if (d.isMember("tool_call_id") && d["tool_call_id"].isString()) ex.tool_call_id = d["tool_call_id"].asString();
      if (!ex.tool_call_id.empty()) out.push_back(std::move(ex));
      continue;
    }

    if (type == "ui_action") {
      const auto& act = d["action"];
      if (!act.isObject()) continue;
      const std::string atype = act.isMember("type") && act["type"].isString() ? act["type"].asString() : "";
      const std::string tool_call_id = d.isMember("tool_call_id") && d["tool_call_id"].isString() ? d["tool_call_id"].asString() : "";
      if (tool_call_id.empty()) continue;

      if (atype == "client_rpc" || atype == "collab_rpc") {
        ExpectedClientAck ex;
        ex.category = "client_rpc";
        ex.tool_call_id = tool_call_id;
        if (act.isMember("rpc_id") && act["rpc_id"].isString()) ex.rpc_id = act["rpc_id"].asString();
        if (act.isMember("rpc") && act["rpc"].isObject() && act["rpc"].isMember("kind") && act["rpc"]["kind"].isString()) {
          ex.rpc_kind = act["rpc"]["kind"].asString();
        }
        out.push_back(std::move(ex));
        continue;
      }

      if (atype == "client_probe") {
        ExpectedClientAck ex;
        ex.category = "client_probe";
        ex.tool_call_id = tool_call_id;
        if (act.isMember("probe_id") && act["probe_id"].isString()) ex.rpc_id = act["probe_id"].asString();
        if (act.isMember("probe") && act["probe"].isObject() && act["probe"].isMember("kind") && act["probe"]["kind"].isString()) {
          ex.rpc_kind = act["probe"]["kind"].asString();
        }
        out.push_back(std::move(ex));
        continue;
      }

      ExpectedClientAck ex;
      ex.category = "ui_action";
      ex.tool_call_id = tool_call_id;
      out.push_back(std::move(ex));
      continue;
    }
  }

  // Deduplicate by category+tool_call_id.
  std::unordered_set<std::string> seen;
  std::vector<ExpectedClientAck> dedup;
  dedup.reserve(out.size());
  for (const auto& ex : out) {
    const std::string k = ex.category + ":" + ex.tool_call_id;
    if (k.empty() || seen.find(k) != seen.end()) continue;
    seen.insert(k);
    dedup.push_back(ex);
  }
  return dedup;
}

Json::Value verify_expected_client_acks(
  AgentDb& db,
  const std::string& session_id,
  int64_t after_unix_ms,
  const std::vector<ExpectedClientAck>& expected,
  int timeout_ms
) {
  Json::Value report(Json::objectValue);
  report["ok"] = true;
  report["session_id"] = session_id;
  report["expected"] = (Json::UInt64)expected.size();
  report["timeout_ms"] = timeout_ms;

  if (expected.empty()) return report;

  const int64_t deadline = now_unix_ms() + std::max<int>(0, timeout_ms);
  Json::CharReaderBuilder rb;

  struct Found {
    bool ok = false;
    std::string type;
    std::string error;
  };
  std::unordered_map<std::string, Found> found;
  auto key_for = [&](const ExpectedClientAck& ex) -> std::string { return ex.category + ":" + ex.tool_call_id; };
  for (const auto& ex : expected) {
    found[key_for(ex)] = Found{};
  }

  auto all_satisfied = [&]() -> bool {
    for (const auto& kv : found) {
      if (!kv.second.ok && kv.second.type.empty() && kv.second.error.empty()) return false;
    }
    return true;
  };

  while (now_unix_ms() <= deadline) {
    std::string tail;
    std::string err;
    if (!db.read_client_events_tail_jsonl(session_id, /*max_bytes=*/1024 * 1024, /*max_events=*/0, &tail, &err)) {
      report["ok"] = false;
      report["error"] = err.empty() ? "failed to read client events" : err;
      return report;
    }

    if (!tail.empty()) {
      std::istringstream iss(tail);
      std::string line;
      while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::string perr;
        Json::Value ev;
        std::istringstream lss(line);
        if (!Json::parseFromStream(rb, lss, &ev, &perr) || !ev.isObject()) continue;

        // Ignore events from before this run (best-effort).
        if (after_unix_ms > 0 && ev.isMember("ts_unix_ms")) {
          const auto& ts = ev["ts_unix_ms"];
          int64_t tms = 0;
          if (ts.isInt64()) tms = ts.asInt64();
          else if (ts.isUInt64()) tms = (int64_t)ts.asUInt64();
          else if (ts.isInt()) tms = (int64_t)ts.asInt();
          if (tms > 0 && tms < after_unix_ms) continue;
        }

        const std::string type = ev.isMember("type") && ev["type"].isString() ? ev["type"].asString() : "";
        if (type.empty()) continue;

        for (const auto& ex : expected) {
          const std::string key = key_for(ex);
          auto it = found.find(key);
          if (it == found.end()) continue;
          if (it->second.ok || !it->second.type.empty() || !it->second.error.empty()) continue;

          if (ex.category == "artifact") {
            if (type == "artifact_rendered" && client_event_matches_tool_call_id(ev, "tool_call_id", ex.tool_call_id)) {
              it->second.ok = true;
              it->second.type = type;
            } else if (type == "artifact_render_failed" && client_event_matches_tool_call_id(ev, "tool_call_id", ex.tool_call_id)) {
              it->second.ok = false;
              it->second.type = type;
              const auto& d = ev["data"];
              it->second.error = (d.isObject() && d.isMember("error") && d["error"].isString()) ? d["error"].asString() : "artifact failed";
            }
            continue;
          }

          if (ex.category == "client_rpc") {
            if (type == "client_rpc_result" && client_event_matches_tool_call_id(ev, "request_tool_call_id", ex.tool_call_id)) {
              it->second.type = type;
              const auto& d = ev["data"];
              const bool ok = d.isObject() && d.isMember("ok") && d["ok"].isBool() ? d["ok"].asBool() : false;
              it->second.ok = ok;
              if (!ok) {
                it->second.error = (d.isObject() && d.isMember("error") && d["error"].isString()) ? d["error"].asString() : "client_rpc failed";
              }
            }
            continue;
          }

          if (ex.category == "client_probe") {
            if (type == "client_probe_result" && client_event_matches_tool_call_id(ev, "request_tool_call_id", ex.tool_call_id)) {
              it->second.type = type;
              const auto& d = ev["data"];
              const bool ok = d.isObject() && d.isMember("ok") && d["ok"].isBool() ? d["ok"].asBool() : false;
              it->second.ok = ok;
              if (!ok) {
                it->second.error = (d.isObject() && d.isMember("error") && d["error"].isString()) ? d["error"].asString() : "client_probe failed";
              }
            }
            continue;
          }

          if (ex.category == "ui_action") {
            if (type == "ui_action_shown" && client_event_matches_tool_call_id(ev, "tool_call_id", ex.tool_call_id)) {
              it->second.ok = true;
              it->second.type = type;
            }
            continue;
          }
        }
      }
    }

    if (all_satisfied()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Json::Value details(Json::arrayValue);
  bool ok_all = true;
  for (const auto& ex : expected) {
    const std::string key = key_for(ex);
    const auto it = found.find(key);
    Json::Value row(Json::objectValue);
    row["category"] = ex.category;
    row["tool_call_id"] = ex.tool_call_id;
    if (!ex.rpc_id.empty()) row["rpc_id"] = ex.rpc_id;
    if (!ex.rpc_kind.empty()) row["rpc_kind"] = ex.rpc_kind;
    if (it == found.end() || (it->second.type.empty() && it->second.error.empty())) {
      row["ok"] = false;
      row["status"] = "missing";
      ok_all = false;
    } else {
      row["ok"] = it->second.ok;
      row["status"] = it->second.type;
      if (!it->second.ok) {
        row["error"] = it->second.error;
        ok_all = false;
      }
    }
    details.append(row);
  }
  report["ok"] = ok_all;
  report["details"] = details;
  if (!ok_all) report["error"] = "client acknowledgement verification failed";
  return report;
}

}  // namespace agentd
