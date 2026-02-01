#include "run_endpoints.h"

#include "daemon_auth.h"
#include "client_profiles.h"
#include "default_system_prompt.h"
#include "file_persistor.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"
#include "openai_provider.h"
#include "provider_util.h"
#include "sandbox_policy.h"
#include "session_id_util.h"
#include "session_paths.h"
#include "session_store.h"
#include "scene_store.h"
#include "secrets_file.h"
#include "string_util.h"
#include "summary_compaction.h"
#include "summary_llm.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#include "openai_stream_decoder.h"

#include "agent/agent.h"
#include "agent/runner.h"

#include <json/json.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

struct ExtendedToolExecutorCtx {
  agent_tool_executor_t base{};
  ToolExtension ext{};
  std::unordered_set<std::string> ext_tool_names;
};

static agent_status_t extended_tool_execute(
  void* vctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  if (!vctx) return AGENT_ERR_INVALID_ARGUMENT;
  auto* ctx = static_cast<ExtendedToolExecutorCtx*>(vctx);
  const char* name = tool_name ? tool_name : "";
  if (ctx->ext.execute_tool && ctx->ext_tool_names.find(name) != ctx->ext_tool_names.end()) {
    return ctx->ext.execute_tool(ctx->ext.ctx, tool_name, arguments_json, out_result);
  }
  if (!ctx->base.execute) return AGENT_ERR_INVALID_ARGUMENT;
  return ctx->base.execute(ctx->base.ctx, tool_name, arguments_json, out_result);
}

static agent_status_t host_read_client_events_tail_from_db(
  void* vctx,
  const std::string& session_id,
  size_t max_bytes,
  size_t /*max_files*/,
  std::string* out_tail_jsonl
) {
  if (!out_tail_jsonl) return AGENT_ERR_INVALID_ARGUMENT;
  out_tail_jsonl->clear();
  auto* db = static_cast<AgentDb*>(vctx);
  if (!db || !db->is_open()) return AGENT_ERR_INVALID_ARGUMENT;
  std::string err;
  if (!db->read_client_events_tail_jsonl(session_id, max_bytes, /*max_events=*/0, out_tail_jsonl, &err)) {
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}

static bool load_session_from_db(
  AgentDb& db,
  const std::string& session_id,
  agent_session_t** out_session,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_session) return false;
  *out_session = nullptr;

  std::vector<std::pair<std::string, std::string>> msgs;
  std::string err;
  if (!db.load_session_messages(session_id, &msgs, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to load session messages" : err;
    return false;
  }

  agent_session_t* session = nullptr;
  if (agent_session_create(&session) != AGENT_OK || !session) {
    if (out_error) *out_error = "failed to create session";
    return false;
  }

  for (const auto& rc : msgs) {
    agent_role_t role = AGENT_ROLE_USER;
    if (!rc.first.empty()) {
      (void)agent_role_from_string(rc.first.c_str(), &role);
    }
    (void)agent_session_add_message(session, role, rc.second.c_str());
  }

  *out_session = session;
  return true;
}

static bool persist_session_to_db(
  AgentDb& db,
  const std::string& session_id,
  const agent_session_t* session,
  int64_t now_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!session) {
    if (out_error) *out_error = "missing session";
    return false;
  }
  std::vector<std::pair<std::string, std::string>> msgs;
  msgs.reserve(agent_session_message_count(session));
  for (size_t i = 0; i < agent_session_message_count(session); i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    msgs.emplace_back(agent_role_to_string(v.role), std::string(v.content ? v.content : "", v.content_len));
  }
  return db.replace_session_messages(session_id, msgs, now_unix_ms, out_error);
}

static bool session_leading_system_has_prefix(const agent_session_t* session, const char* prefix) {
  if (!session || !prefix || !prefix[0]) return false;
  const size_t prefix_len = std::strlen(prefix);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n && i < 8; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) return false;
    if (v.content_len >= prefix_len && std::memcmp(v.content, prefix, prefix_len) == 0) return true;
  }
  return false;
}

static bool session_leading_system_has_substring(const agent_session_t* session, const char* needle) {
  if (!session || !needle || !needle[0]) return false;
  const std::string n(needle);
  const size_t count = agent_session_message_count(session);
  for (size_t i = 0; i < count && i < 8; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) return false;
    const std::string s(v.content, v.content_len);
    if (s.find(n) != std::string::npos) return true;
  }
  return false;
}

[[maybe_unused]] static bool session_has_any_system_prefix(const agent_session_t* session, const char* prefix) {
  if (!session || !prefix || !prefix[0]) return false;
  const size_t prefix_len = std::strlen(prefix);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) continue;
    if (v.content_len >= prefix_len && std::memcmp(v.content, prefix, prefix_len) == 0) return true;
  }
  return false;
}

[[maybe_unused]] static bool session_has_any_system_substring(const agent_session_t* session, const char* needle) {
  if (!session || !needle || !needle[0]) return false;
  const std::string n(needle);
  const size_t count = agent_session_message_count(session);
  for (size_t i = 0; i < count; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM || !v.content) continue;
    const std::string s(v.content, v.content_len);
    if (s.find(n) != std::string::npos) return true;
  }
  return false;
}

static bool ensure_pinned_host_system_prompts(
  agent_session_t** session_io,
  const std::string& tools,
  bool no_default_system,
  const std::string& client_kind,
  bool allow_default_host_prompt
) {
  if (!session_io || !*session_io) return false;
  if (no_default_system) return false;
  if (tools != "host") return false;

  // These are intentionally substring/prefix checks, not exact-string matches:
  // - allows prompt evolution without breaking the "present in session" detection
  // - avoids repeated insertion across runs in a long-lived session.
  const char* kHostPrefix = "You are a host-side coding agent";
  const char* kClientProfilePrefix = "CLIENT_PROFILE=";

  agent_session_t* session = *session_io;

  const bool want_host = allow_default_host_prompt;
  const bool want_profile = !client_kind.empty();
  const std::string profile = want_profile ? client_profile_system_prompt(client_kind) : std::string();
  const std::string profile_marker = want_profile ? (std::string("CLIENT_PROFILE=") + client_kind) : std::string();

  const bool have_host_leading = session_leading_system_has_prefix(session, kHostPrefix);
  const bool have_profile_leading = want_profile ? session_leading_system_has_substring(session, profile_marker.c_str()) : true;

  if ((want_host && !have_host_leading) || (want_profile && !have_profile_leading)) {
    // Rebuild the session so the required system messages are *leading* (pinned by core compaction policy).
    // This is required because agent_session_add_message only appends; appended system messages are not pinned
    // and can be dropped during char-budget compaction (leading system messages are the pinned prefix).
    struct Msg {
      agent_role_t role;
      std::string content;
    };
    std::vector<Msg> msgs;
    msgs.reserve(agent_session_message_count(session));
    for (size_t i = 0; i < agent_session_message_count(session); i++) {
      agent_message_view_t v{};
      if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
      msgs.push_back(Msg{v.role, std::string(v.content ? v.content : "", v.content_len)});
    }

    agent_session_t* ns = nullptr;
    if (agent_session_create(&ns) != AGENT_OK || !ns) {
      return false;
    }

    if (want_host) {
      (void)agent_session_add_message(ns, AGENT_ROLE_SYSTEM, default_host_system_prompt());
    }
    if (want_profile && !profile.empty()) {
      // Always pin the active client's profile for this run. Any older profile strings (possibly for a different client)
      // are deduplicated below to avoid mixing semantics in a shared session.
      (void)agent_session_add_message(ns, AGENT_ROLE_SYSTEM, profile.c_str());
    }

    for (const auto& m : msgs) {
      // Deduplicate older/stray copies of these injected prompts to keep the session stable.
      if (m.role == AGENT_ROLE_SYSTEM) {
        if (!m.content.empty()) {
          if (m.content.rfind(kHostPrefix, 0) == 0) continue;
          if (m.content.rfind(kClientProfilePrefix, 0) == 0) continue;
        }
      }
      (void)agent_session_add_message(ns, m.role, m.content.c_str());
    }

    agent_session_destroy(session);
    *session_io = ns;
    return true;
  }
  return false;
}

static bool is_safe_filename_component_ascii(const std::string& s) {
  if (s.empty() || s.size() > 160) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string local_date_ymd_days_ago(int days_ago) {
  const auto now = std::chrono::system_clock::now() - std::chrono::hours(24 * std::max(0, days_ago));
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

static std::string read_file_capped(const std::filesystem::path& p, size_t max_bytes) {
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return "";
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  if (s.size() > max_bytes) s.resize(max_bytes);
  return s;
}

static bool build_memory_context_text(
  const std::string& state_dir,
  const std::string& session_id,
  std::string* out_text
) {
  if (out_text) out_text->clear();
  if (!out_text) return false;
  if (state_dir.empty()) return false;

  const std::filesystem::path mem_root = std::filesystem::path(state_dir) / "memory";

  std::error_code ec;
  if (!std::filesystem::exists(mem_root, ec) || !std::filesystem::is_directory(mem_root, ec)) {
    return false;
  }

  struct Candidate {
    std::string label;
    std::filesystem::path path;
    size_t cap;
  };
  std::vector<Candidate> cands;
  cands.push_back(Candidate{"MEMORY.md", mem_root / "MEMORY.md", 6000});
  cands.push_back(Candidate{local_date_ymd_days_ago(0) + ".md", mem_root / (local_date_ymd_days_ago(0) + ".md"), 2200});
  cands.push_back(Candidate{local_date_ymd_days_ago(1) + ".md", mem_root / (local_date_ymd_days_ago(1) + ".md"), 2200});
  if (is_safe_filename_component_ascii(session_id)) {
    cands.push_back(Candidate{"sessions/" + session_id + ".md", mem_root / "sessions" / (session_id + ".md"), 2400});
  }

  std::ostringstream oss;
  oss
    << "DURABLE_MEMORY_CONTEXT\n"
    << "- The following notes are loaded from durable Markdown memory files on disk.\n"
    << "- Treat these as authoritative *if still current*.\n"
    << "- If you discover a requirement has changed (\"used to be true\" -> \"no longer true\"), update memory to remove contradictions.\n"
    << "- Use memory_get/memory_search to inspect and memory_write/memory_put to persist/consolidate.\n";

  int included = 0;
  for (const auto& c : cands) {
    ec.clear();
    if (!std::filesystem::exists(c.path, ec) || !std::filesystem::is_regular_file(c.path, ec)) continue;
    std::string content = read_file_capped(c.path, c.cap);
    if (trim_copy(content).empty()) continue;
    included++;
    oss << "\n[" << c.label << "]\n";
    oss << content;
    if (!content.empty() && content.back() != '\n') oss << "\n";
  }
  if (included == 0) return false;

  std::string s = oss.str();
  const size_t kTotalCap = 12000;
  if (s.size() > kTotalCap) s.resize(kTotalCap);
  *out_text = std::move(s);
  return true;
}

static agent_session_t* clone_session_with_memory_context(const agent_session_t* src, const std::string& memory_context) {
  if (!src) return nullptr;
  if (memory_context.empty()) return nullptr;

  agent_session_t* ns = nullptr;
  if (agent_session_create(&ns) != AGENT_OK || !ns) return nullptr;

  const size_t n = agent_session_message_count(src);
  size_t pinned_system = 0;
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(src, i, &v) != AGENT_OK) continue;
    if (v.role != AGENT_ROLE_SYSTEM) break;
    pinned_system++;
  }

  auto add_msg = [&](agent_role_t role, const agent_message_view_t& v) {
    const std::string content(v.content ? v.content : "", v.content_len);
    if (role == AGENT_ROLE_SYSTEM && !content.empty() && content.rfind("DURABLE_MEMORY_CONTEXT", 0) == 0) return;
    (void)agent_session_add_message(ns, role, content.c_str());
  };

  for (size_t i = 0; i < pinned_system; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(src, i, &v) != AGENT_OK) continue;
    add_msg(v.role, v);
  }

  (void)agent_session_add_message(ns, AGENT_ROLE_SYSTEM, memory_context.c_str());

  for (size_t i = pinned_system; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(src, i, &v) != AGENT_OK) continue;
    add_msg(v.role, v);
  }

  return ns;
}

struct ExpectedClientAck {
  // category: "artifact" | "client_rpc" | "client_probe" | "ui_action"
  std::string category;
  std::string tool_call_id;
  std::string rpc_id;
  std::string rpc_kind;
};

static std::vector<ExpectedClientAck> collect_expected_client_acks(const Json::Value& events_out) {
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

static bool client_event_matches_tool_call_id(const Json::Value& ev, const char* field, const std::string& tool_call_id) {
  if (!ev.isObject() || tool_call_id.empty() || !field) return false;
  const auto& d = ev["data"];
  if (!d.isObject()) return false;
  const auto& f = d[field];
  return f.isString() && f.asString() == tool_call_id;
}

static Json::Value verify_expected_client_acks(
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

// Parses the daemon run request body and returns a response JSON object (HTTP-level errors are represented in JSON).
static Json::Value run_request_to_json(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const std::string& request_body,
  const char* job_id_or_null
) {
  Json::Value args;
  std::string perr;
  if (!json_parse_object(request_body, &args, &perr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = std::string("invalid JSON: ") + perr;
    return o;
  }

  const std::string prompt = args.isMember("prompt") && args["prompt"].isString() ? args["prompt"].asString() : "";
  if (prompt.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing prompt";
    return o;
  }

  OpenAIClientConfig run_cfg = ocfg;
  const bool base_url_explicit = args.isMember("base_url") && args["base_url"].isString();
  const bool api_key_explicit = args.isMember("api_key") && args["api_key"].isString();
  if (args.isMember("model") && args["model"].isString()) run_cfg.model = args["model"].asString();
  if (args.isMember("base_url") && args["base_url"].isString()) run_cfg.base_url = args["base_url"].asString();
  if (api_key_explicit) run_cfg.api_key = args["api_key"].asString();
  if (args.isMember("proxy") && args["proxy"].isString()) run_cfg.proxy_url = args["proxy"].asString();
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const long t = (long)args["timeout_ms"].asInt64();
    if (t > 0) run_cfg.timeout_ms = t;
  }

  // Provider key fallback (framework responsibility):
  // If the request omitted api_key, load a provider-matching key based on run_cfg.base_url.
  //
  // Important: ocfg.api_key may be set at daemon startup; if the UI changes base_url per run, that key may be wrong.
  if (!api_key_explicit) {
    const std::string run_provider = provider_from_base_url(run_cfg.base_url);
    const std::string daemon_provider = provider_from_base_url(ocfg.base_url);
    const bool provider_mismatch = base_url_explicit && (run_provider != daemon_provider);
    // If a daemon-side provider-specific key exists, prefer it over a single global api_key.
    {
      const auto it = daemon_cfg.provider_keys.find(run_provider);
      if (it != daemon_cfg.provider_keys.end() && !it->second.empty()) {
        run_cfg.api_key = it->second;
      }
    }

    if (run_cfg.api_key.empty() || provider_mismatch) {
      std::string key;
      // Environment variable fallback (common deployment style).
      if (run_provider == "deepseek") {
        if (const char* k = getenv_s("DEEPSEEK_API_KEY")) key = k;
        else if (const char* k2 = getenv_s("OPENAI_API_KEY")) key = k2;
        else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) key = k3;
      } else if (run_provider == "openrouter") {
        if (const char* k = getenv_s("OPENROUTER_API_KEY")) key = k;
        else if (const char* k2 = getenv_s("OPENAI_API_KEY")) key = k2;
        else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) key = k3;
      } else {
        if (const char* k = getenv_s("OPENAI_API_KEY")) key = k;
        else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) key = k2;
        else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) key = k3;
      }
      // Repo-local secrets discovery (gitignored .not_in_repo / project.local.md).
      if (key.empty()) {
        if (auto k = load_provider_key_best_effort(run_provider)) {
          key = *k;
        }
      }
      if (!key.empty()) {
        run_cfg.api_key = key;
      }
    }
  }

  const std::string tools = args.isMember("tools") && args["tools"].isString() ? args["tools"].asString() : daemon_cfg.tools;
  const bool requested_yolo_set = args.isMember("yolo") && args["yolo"].isBool();
  const bool requested_yolo = requested_yolo_set ? args["yolo"].asBool() : daemon_cfg.yolo_default;
  const bool yolo = sandbox_tighten_yolo(daemon_cfg.yolo_default, requested_yolo, requested_yolo_set);
  const bool no_default_system =
    args.isMember("no_default_system") && args["no_default_system"].isBool() ? args["no_default_system"].asBool() : daemon_cfg.no_default_system;
  const std::string system_msg = args.isMember("system") && args["system"].isString() ? args["system"].asString() : "";
  std::string client_kind;
  if (args.isMember("client") && args["client"].isObject()) {
    const Json::Value& c = args["client"];
    if (c.isMember("kind") && c["kind"].isString()) client_kind = c["kind"].asString();
  }
  const bool require_client_acks =
    args.isMember("require_client_acks") && args["require_client_acks"].isBool()
      ? args["require_client_acks"].asBool()
      : false;

  const std::string session_id = args.isMember("session_id") && args["session_id"].isString() ? args["session_id"].asString() : "default";
  const bool no_session = args.isMember("no_session") && args["no_session"].isBool() ? args["no_session"].asBool() : false;
  if (!session_id_is_safe(session_id)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid session_id";
    return o;
  }
  if (args.isMember("tools_root")) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "tools_root was removed; omit it and use explicit paths or session_id";
    return o;
  }
  HostToolsetPolicyMode requested_policy = daemon_cfg.host_policy;
  if (args.isMember("host_policy") && args["host_policy"].isString()) {
    HostToolsetPolicyMode p{};
    const std::string s = args["host_policy"].asString();
    if (!host_policy_from_string(s, &p)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 400;
      o["error"] = "invalid host_policy (expected: full|readonly)";
      return o;
    }
    requested_policy = p;
  }
  const HostToolsetPolicyMode effective_policy = tighten_host_policy(daemon_cfg.host_policy, requested_policy);
  if (!no_session && !sessions_root_dir.empty()) {
    const std::filesystem::path sr = session_root_path(sessions_root_dir, session_id);
    if (sr.empty()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 400;
      o["error"] = "invalid session_id";
      return o;
    }
    std::error_code ec;
    (void)std::filesystem::create_directories(sr / "work", ec);
    ec.clear();
    (void)std::filesystem::create_directories(sr / "out", ec);
  }
  uint64_t max_steps_u64 = 0;
  const size_t max_steps =
    json_get_u64_nonneg(args, "max_steps", &max_steps_u64) ? (size_t)max_steps_u64 : daemon_cfg.max_steps_default;
  uint64_t max_tool_calls_total_u64 = 0;
  const size_t max_tool_calls_total =
    json_get_u64_nonneg(args, "max_tool_calls_total", &max_tool_calls_total_u64)
      ? (size_t)max_tool_calls_total_u64
      : daemon_cfg.max_tool_calls_total_default;
  uint64_t max_tool_calls_per_tool_u64 = 0;
  const size_t max_tool_calls_per_tool =
    json_get_u64_nonneg(args, "max_tool_calls_per_tool", &max_tool_calls_per_tool_u64)
      ? (size_t)max_tool_calls_per_tool_u64
      : daemon_cfg.max_tool_calls_per_tool_default;

  // Explicit per-tool call limits (more precise than max_tool_calls_per_tool).
  std::vector<ToolCallLimit> tool_call_limits;
  auto upsert_limit = [&](std::string tool, size_t max_calls) {
    if (tool.empty()) return;
    for (auto& x : tool_call_limits) {
      if (x.tool == tool) {
        x.max_calls = max_calls;
        return;
      }
    }
    ToolCallLimit x;
    x.tool = std::move(tool);
    x.max_calls = max_calls;
    tool_call_limits.push_back(std::move(x));
  };
  // Start from daemon defaults and apply per-run overrides (if any).
  // This keeps operators safe by default while still allowing per-run tightening/loosening.
  for (const auto& p : daemon_cfg.tool_call_limits_default) {
    upsert_limit(p.first, p.second);
  }
  if (args.isMember("tool_call_limits") && args["tool_call_limits"].isArray()) {
    const Json::Value arr = args["tool_call_limits"];
    for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
      const Json::Value item = arr[i];
      if (!item.isObject()) continue;
      if (!item.isMember("tool") || !item["tool"].isString()) continue;
      const std::string tool = item["tool"].asString();
      uint64_t n_u64 = 0;
      if (!json_get_u64_nonneg(item, "max_calls", &n_u64)) continue;
      upsert_limit(tool, (size_t)n_u64);
    }
  }
  uint64_t max_chars_u64 = 0;
  const size_t max_chars = json_get_u64_nonneg(args, "max_chars", &max_chars_u64)
                             ? (size_t)max_chars_u64
                             : daemon_cfg.max_chars_default;
  uint64_t keep_last_u64 = 0;
  const size_t keep_last = json_get_u64_nonneg(args, "keep_last", &keep_last_u64)
                             ? (size_t)keep_last_u64
                             : daemon_cfg.keep_last_default;
  const std::string summary_model =
    args.isMember("summary_model") && args["summary_model"].isString() ? args["summary_model"].asString() : daemon_cfg.summary_model;
  uint64_t summary_max_chars_u64 = 0;
  const size_t summary_max_chars =
    json_get_u64_nonneg(args, "summary_max_chars", &summary_max_chars_u64) ? (size_t)summary_max_chars_u64 : daemon_cfg.summary_max_chars;
  const bool trace = !(args.isMember("trace") && args["trace"].isBool() && args["trace"].asBool() == false);
  const bool verbose = args.isMember("verbose") && args["verbose"].isBool() ? args["verbose"].asBool() : false;
  const bool stream_assistant =
    args.isMember("stream_assistant") && args["stream_assistant"].isBool() ? args["stream_assistant"].asBool() : false;
  uint64_t max_capture_bytes_u64 = 0;
  const size_t max_capture_bytes =
    json_get_u64_nonneg(args, "max_capture_bytes", &max_capture_bytes_u64) ? (size_t)max_capture_bytes_u64 : (size_t)256 * 1024;

  std::string job_id_local = (job_id_or_null && job_id_or_null[0]) ? std::string(job_id_or_null) : std::string();
  const int64_t run_ts_ms = now_unix_ms();

  agent_session_t* session = nullptr;
  if (!no_session) {
    if (!db_or_null || !db_or_null->is_open()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "db not available (session persistence required)";
      return o;
    }
    std::string load_err;
    if (!load_session_from_db(*db_or_null, session_id, &session, &load_err)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = load_err.empty() ? "failed to load session from db" : load_err;
      return o;
    }
  } else {
    const agent_status_t st = agent_session_create(&session);
    if (st != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to create session";
      o["status"] = (Json::Int64)st;
      return o;
    }
  }

  // One-time system message insertion for host tools:
  // - If `system` is provided in the request, it wins (inserted only when the session is empty).
  // - Otherwise, when using host tools, insert a default host system hint unless disabled.
  if (agent_session_message_count(session) == 0) {
    if (!system_msg.empty()) {
      agent_session_add_message(session, AGENT_ROLE_SYSTEM, system_msg.c_str());
      // Even when the caller provides a custom system message, still inject the client profile
      // (DoD semantics / UI-specific RPC guidance) if enabled.
      if (!no_default_system && tools == "host" && !client_kind.empty()) {
        const std::string profile = client_profile_system_prompt(client_kind);
        if (!profile.empty()) {
          agent_session_add_message(session, AGENT_ROLE_SYSTEM, profile.c_str());
        }
      }
    } else {
      if (!no_default_system && tools == "host") {
        agent_session_add_message(session, AGENT_ROLE_SYSTEM, default_host_system_prompt());
        if (!client_kind.empty()) {
          const std::string profile = client_profile_system_prompt(client_kind);
          if (!profile.empty()) {
            agent_session_add_message(session, AGENT_ROLE_SYSTEM, profile.c_str());
          }
        }
      }
    }
  } else {
    // For long-lived sessions (e.g. Web UI "default"), do not rely on "empty session" to ensure
    // essential host/tool + DoD guidance is present. If the session was created via tools=none or
    // imported from an older version, it may have no system prompt at all.
    const bool changed = ensure_pinned_host_system_prompts(&session, tools, no_default_system, client_kind, /*allow_default_host_prompt=*/true);
    if (changed && !no_session) {
      // Persist the prefix change even if the run later fails, so subsequent runs don't regress.
      if (db_or_null && db_or_null->is_open()) {
        (void)persist_session_to_db(*db_or_null, session_id, session, run_ts_ms, nullptr);
      }
    }
  }

  agent_tool_registry_t* registry = nullptr;
  agent_tool_executor_t base_executor{};
  agent_tool_executor_t executor{};
  std::unique_ptr<ExtendedToolExecutorCtx> extended_executor;
  bool need_destroy_host_executor = false;
  const bool use_tool_loop = (tools != "none");

  if (tools == "basic") {
    if (toolset_basic_create(&registry, &base_executor) != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to init toolset_basic";
      agent_session_destroy(session);
      return o;
    }
    executor = base_executor;
  } else if (tools == "host") {
    HostToolsetConfig hcfg;
    hcfg.policy = effective_policy;
    // In scoped mode (yolo=false), omit process exec tools so "scoped filesystem" doesn't still mean
    // arbitrary host command execution.
    hcfg.enable_process_exec = yolo;
    hcfg.allow_symlinks = yolo;
    if (!job_id_local.empty()) {
      // Cooperative cancellation for long-running host tools (sleep/build/etc).
      hcfg.should_cancel = [](void* vctx) -> bool {
        if (!vctx) return false;
        const auto* jid = static_cast<const std::string*>(vctx);
        return jid && job_is_cancel_requested(*jid);
      };
      hcfg.should_cancel_ctx = (void*)&job_id_local;
    }
    // Session context for UI coordination tools (e.g. ui_wait_event). Only valid for session-backed runs.
    if (!no_session) {
      hcfg.sessions_root_dir = sessions_root_dir;
      hcfg.session_id = session_id;
      if (db_or_null && db_or_null->is_open()) {
        hcfg.read_client_events_tail = host_read_client_events_tail_from_db;
        hcfg.read_client_events_tail_ctx = (void*)db_or_null;
      }
    }
    if (toolset_host_create(hcfg, &registry, &base_executor) != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to init toolset_host";
      agent_session_destroy(session);
      return o;
    }
    need_destroy_host_executor = true;
    executor = base_executor;
  } else if (tools != "none") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid tools (expected: none|basic|host)";
    agent_session_destroy(session);
    return o;
  }

  // Optional tool extension: allow embedding hosts to append extra tools and execute them.
  // We only dispatch names added by the extension.
  if (registry && tool_ext_or_null && tool_ext_or_null->register_tools) {
    const size_t before = agent_tool_registry_count(registry);
    const agent_status_t st = tool_ext_or_null->register_tools(tool_ext_or_null->ctx, registry);
    if (st != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "tool extension register_tools failed";
      agent_tool_registry_destroy(registry);
      if (need_destroy_host_executor) {
        toolset_host_destroy(&base_executor);
      }
      agent_session_destroy(session);
      return o;
    }
    const size_t after = agent_tool_registry_count(registry);
    if (after > before && tool_ext_or_null->execute_tool) {
      extended_executor = std::make_unique<ExtendedToolExecutorCtx>();
      extended_executor->base = base_executor;
      extended_executor->ext = *tool_ext_or_null;
      for (size_t i = before; i < after; i++) {
        agent_tool_def_view_t v{};
        if (agent_tool_registry_get(registry, i, &v) != AGENT_OK) continue;
        if (!v.name || !v.name[0]) continue;
        extended_executor->ext_tool_names.insert(v.name);
      }
      if (!extended_executor->ext_tool_names.empty()) {
        executor.ctx = extended_executor.get();
        executor.execute = extended_tool_execute;
      }
    }
  }

  bool ok = false;
  std::string assistant_text;
  std::string err;
  long http_status = 0;
  std::string http_body;
  std::ostringstream trace_buf;
  std::ostream* trace_stream = trace ? &trace_buf : nullptr;
  Json::Value events_out;
  ToolLoopResult tool_loop_result;

  std::atomic<bool> heartbeat_stop{false};
  std::atomic<int64_t> heartbeat_last_any_event_ms{now_unix_ms()};
  std::atomic<int64_t> heartbeat_last_non_ms{now_unix_ms()};
  std::atomic<int> heartbeat_phase{kPhaseIdle};
  std::thread heartbeat_thread;
  if (!job_id_local.empty()) {
    heartbeat_thread = std::thread([&]() {
      // Emit a best-effort heartbeat while a job is running to avoid the appearance of "hangs"
      // during long tool exec (sleep/build) or slow LLM responses.
      for (;;) {
        if (heartbeat_stop.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (heartbeat_stop.load()) return;
        if (job_is_cancel_requested(job_id_local)) return;

        const int64_t now = now_unix_ms();
        const int64_t since_non = now - heartbeat_last_non_ms.load();
        const int64_t since_any = now - heartbeat_last_any_event_ms.load();
        // Only emit when we've been quiet for a while.
        if (since_non >= 1200 && since_any >= 900) {
          daemon_job_emit_heartbeat(job_id_local, heartbeat_phase.load(), since_non, since_any);
          heartbeat_last_any_event_ms.store(now);
        }
      }
    });
  }

  if (use_tool_loop) {
    ToolLoopOptions opt;
    opt.max_steps = max_steps;
    opt.max_tool_calls_total = max_tool_calls_total;
    opt.max_tool_calls_per_tool = max_tool_calls_per_tool;
    opt.tool_call_limits = std::move(tool_call_limits);
    opt.verbose = verbose;
    opt.stream_assistant = stream_assistant;
    if (args.isMember("max_repeated_tool_calls") && args["max_repeated_tool_calls"].isInt()) {
      const int v = args["max_repeated_tool_calls"].asInt();
      if (v >= 0) opt.max_repeated_tool_calls = (size_t)v;
    }
    // Avoid UI freezes when verbose tracing captures huge request/response/tool blobs.
    // Full fidelity remains available in `trace_text`.
    opt.max_capture_bytes = max_capture_bytes == 0 ? (size_t)64 * 1024 : std::min<size_t>(max_capture_bytes, (size_t)1024 * 1024);
    opt.max_chars = max_chars;
    opt.keep_last_messages = keep_last;
    if (args.isMember("force_tool") && args["force_tool"].isString()) opt.force_tool = args["force_tool"].asString();
    opt.require_tool_call = args.isMember("require_tool_call") && args["require_tool_call"].isBool() ? args["require_tool_call"].asBool() : false;

    DaemonJobEventHookCtx hook;
	    if (!job_id_local.empty()) {
	      hook.job_id = job_id_local;
      hook.last_any_event_ms = &heartbeat_last_any_event_ms;
      hook.last_non_heartbeat_ms = &heartbeat_last_non_ms;
      hook.phase = &heartbeat_phase;
      opt.on_event = daemon_job_on_tool_loop_event;
      opt.on_event_ctx = &hook;
      opt.should_cancel = [](void* vctx) -> bool {
        if (!vctx) return false;
        const auto* jid = static_cast<const std::string*>(vctx);
        return jid && job_is_cancel_requested(*jid);
      };
	      opt.should_cancel_ctx = (void*)&job_id_local;
	    }

	    struct SessionDel {
	      void operator()(agent_session_t* s) const {
	        if (s) agent_session_destroy(s);
	      }
	    };
	    std::unique_ptr<agent_session_t, SessionDel> ephemeral_seed;
		    const agent_session_t* seed_for_run = session;
		    if (!no_default_system && tools == "host" && !no_session) {
		      std::string mem_ctx;
		      if (build_memory_context_text(daemon_cfg.state_dir, session_id, &mem_ctx)) {
		        if (agent_session_t* tmp = clone_session_with_memory_context(session, mem_ctx)) {
		          ephemeral_seed.reset(tmp);
		          seed_for_run = tmp;
		        }
		      }
		    }

	    try {
	      ok = run_tool_loop(
	        run_cfg, seed_for_run, prompt, registry, &executor, opt, trace_stream, &tool_loop_result, &err, &http_status, &http_body
	      );
	    } catch (const std::exception& e) {
	      ok = false;
	      err = std::string("tool loop threw exception: ") + e.what();
    } catch (...) {
      ok = false;
      err = "tool loop threw unknown exception";
    }
    assistant_text = tool_loop_result.final_assistant_text;
    if (!tool_loop_result.events_json.empty()) {
      Json::CharReaderBuilder rb;
      std::string errs;
      std::istringstream iss(tool_loop_result.events_json);
      Json::Value ev;
      if (Json::parseFromStream(rb, iss, &ev, &errs) && ev.isArray()) {
        events_out = ev;
      }
    }

    if (ok) {
      // Persist the conversational session:
      // - user prompt
      // - final assistant message
      //
      // Tool calls/results are stored in the session audit JSONL (host-only) and returned via `events`.
      agent_session_add_message(session, AGENT_ROLE_USER, prompt.c_str());
      agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
    }
  } else {
    agent_session_add_message(session, AGENT_ROLE_USER, prompt.c_str());

    events_out = Json::Value(Json::arrayValue);
    auto push_ev = [&](const std::string& type, const Json::Value& data) {
      Json::Value e(Json::objectValue);
      e["type"] = type;
      e["data"] = data;
      events_out.append(e);
      heartbeat_last_any_event_ms.store(now_unix_ms());
      // Heartbeats are emitted as job events only; do not treat them as "non-heartbeat" updates.
      if (type != "heartbeat") {
        heartbeat_last_non_ms.store(now_unix_ms());
      }
      if (type == "llm_request") heartbeat_phase.store(kPhaseWaitingLlm);
      if (type == "llm_response") heartbeat_phase.store(kPhaseIdle);
      if (job_id_or_null && job_id_or_null[0]) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        job_append_event(job_id_or_null, type, Json::writeString(wb, data));
      }
    };
    {
      Json::Value d(Json::objectValue);
      d["model"] = run_cfg.model;
      d["tools"] = "none";
      d["verbose"] = verbose;
      d["stream_assistant"] = stream_assistant;
      push_ev("start", d);
    }

    auto is_cancelled_job = [&]() -> bool {
      return !job_id_local.empty() && job_is_cancel_requested(job_id_local);
    };

    if (stream_assistant) {
      // Retry loop for providers that reject over-long contexts.
      // For stateless providers, retrying with a tighter compaction budget is equivalent to "spawning a new session".
      size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
      const size_t keep = (keep_last == 0 ? 16 : keep_last);

      for (int attempt = 0; attempt < 3; attempt++) {
        if (is_cancelled_job()) {
          ok = false;
          err = "cancelled";
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["reason"] = "cancel_requested";
          push_ev("cancelled", d);
          break;
        }
        // Apply core compaction policy (same as agent_run_once) and surface a compaction event.
        agent_compact_report_t compact{};
        const agent_status_t cst = agent_session_compact_char_budget(session, attempt_max_chars, keep, nullptr, &compact);
        if (cst != AGENT_OK) {
          ok = false;
          err = "session compaction failed";
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["error"] = err;
          push_ev("error", d);
          break;
        }
        {
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["max_chars"] = (Json::UInt64)attempt_max_chars;
          d["keep_last"] = (Json::UInt64)keep;
          d["before_chars"] = (Json::UInt64)compact.before_chars;
          d["after_chars"] = (Json::UInt64)compact.after_chars;
          d["dropped_messages"] = (Json::UInt64)compact.dropped_messages;
          d["inserted_summary"] = (bool)compact.inserted_summary;
          push_ev("compaction", d);
        }

        // Build the provider request JSON from the compacted session messages.
        std::string request_json;
        {
          Json::Value root(Json::objectValue);
          root["model"] = run_cfg.model;
          root["stream"] = true;
          Json::Value messages(Json::arrayValue);
          const size_t n = agent_session_message_count(session);
          for (size_t i = 0; i < n; i++) {
            agent_message_view_t v{};
            if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
            Json::Value m(Json::objectValue);
            m["role"] = agent_role_to_string(v.role);
            m["content"] = std::string(v.content, v.content_len);
            messages.append(m);
          }
          root["messages"] = messages;
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          request_json = Json::writeString(wb, root);
        }
        {
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          if (verbose) {
            bool trunc = false;
            d["request_json"] = truncate_for_event(request_json, 64 * 1024, &trunc);
            d["request_truncated"] = trunc;
          }
          push_ev("llm_request", d);
        }

        struct StreamCtx {
          std::string assistant;
          std::string pending_delta;
          bool verbose = false;
          int chunks = 0;
          uint64_t step = 0;
          uint64_t epoch = 0;
          decltype(push_ev)* push = nullptr;
        } sctx;
        sctx.verbose = verbose;
        sctx.push = &push_ev;
        sctx.step = 0;
        sctx.epoch = (uint64_t)attempt;

        auto on_chunk = [](void* vctx, const char* chunk_json, size_t chunk_len) {
          auto* s = static_cast<StreamCtx*>(vctx);
          if (!s || !chunk_json || chunk_len == 0 || !s->push) return;

          s->chunks++;
          OpenAIStreamChunk chunk;
          if (!openai_stream_parse_chunk_json(chunk_json, chunk_len, &chunk)) return;
          const std::string dstr = chunk.content_delta;
          if (dstr.empty()) return;
          s->assistant += dstr;
          s->pending_delta += dstr;

          // Coalesce small deltas to avoid flooding the daemon/UI with thousands of events.
          if (s->pending_delta.size() >= 128) {
            Json::Value d(Json::objectValue);
            d["step"] = (Json::UInt64)s->step;
            d["epoch"] = (Json::UInt64)s->epoch;
            d["delta"] = s->pending_delta;
            (*s->push)("assistant_delta", d);
            s->pending_delta.clear();
          }
        };

        OpenAIStreamResult sr = openai_chat_completions_raw_stream(run_cfg, request_json, on_chunk, &sctx, max_capture_bytes);
        http_status = sr.http_status;
        http_body = sr.response_body;
        if (trace_stream) {
          *trace_stream << "=== REQUEST (stream=true attempt=" << attempt << ") ===\n";
          *trace_stream << request_json << "\n";
          *trace_stream << "=== RESPONSE (stream capture) ===\n";
          *trace_stream << (sr.response_body.empty() ? "" : (sr.response_body + "\n"));
        }

        {
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["http_status"] = (Json::Int64)sr.http_status;
          if (verbose) {
            bool trunc = false;
            d["response_body"] = truncate_for_event(sr.response_body, 64 * 1024, &trunc);
            d["response_truncated"] = trunc;
          }
          d["stream"] = true;
          push_ev("llm_response", d);
        }

        // Flush any pending deltas.
        if (!sctx.pending_delta.empty()) {
          Json::Value d(Json::objectValue);
          d["step"] = (Json::UInt64)sctx.step;
          d["epoch"] = (Json::UInt64)sctx.epoch;
          d["delta"] = sctx.pending_delta;
          push_ev("assistant_delta", d);
          sctx.pending_delta.clear();
        }

        // Provider may have ignored streaming and returned a normal JSON completion.
        if (sr.http_status < 200 || sr.http_status >= 300) {
          ok = false;
          err = !sr.error_message.empty() ? sr.error_message : openai_format_http_error(sr.http_status, sr.response_body);
        } else {
          const std::string final_text = sctx.assistant.empty()
            ? json_try_extract_assistant_content_from_completion([&]() -> Json::Value {
                Json::Value v;
                std::string pe;
                if (!json_parse_any(sr.response_body, &v, &pe)) return Json::Value(Json::nullValue);
                return v;
              }())
            : sctx.assistant;
          if (final_text.empty()) {
            ok = false;
            err = "streamed completion returned no assistant content";
          } else {
            assistant_text = final_text;
            ok = true;
          }
        }

        if (ok) {
          agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
          break;
        }

        if (openai_is_context_too_long_error(sr.http_status, sr.response_body)) {
          attempt_max_chars = std::max<size_t>(256, attempt_max_chars / 2);
          continue;
        }
        break;
      }
    } else {
      // Non-stream request path (tools=none): use core runner + OpenAI provider adapter (same as CLI).
      OpenAIProviderCtx pctx;
      pctx.cfg = run_cfg;
      const agent_provider_t provider = openai_make_provider(&pctx);

      agent_run_options_t run_opt{};
      run_opt.model = run_cfg.model.c_str();
      run_opt.keep_last_messages = keep_last;
      run_opt.summary_or_null = nullptr;

      size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
      agent_status_t last_st = AGENT_ERR_INTERNAL;

      for (int attempt = 0; attempt < 3; attempt++) {
        if (is_cancelled_job()) {
          ok = false;
          err = "cancelled";
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["reason"] = "cancel_requested";
          push_ev("cancelled", d);
          break;
        }

        run_opt.max_chars = attempt_max_chars;
        run_opt.summary_or_null = nullptr;

        std::string summary_buf;
        if (attempt == 0 && !summary_model.empty() && agent_session_estimated_chars(session) > attempt_max_chars) {
          SummaryCompactionInput input = build_summary_compaction_input(session, keep_last);
          if (input.dropped_messages > 0 && !input.excerpt.empty()) {
            const size_t max_out = (summary_max_chars == 0 ? (size_t)1200 : summary_max_chars);
            CompactionSummaryResult sr = generate_compaction_summary_via_llm(run_cfg, summary_model, input, max_out);
            if (sr.ok && !sr.summary_text.empty()) {
              summary_buf = std::string(AGENT_SESSION_SUMMARY_PREFIX) + "\n" + sr.summary_text;
              run_opt.summary_or_null = summary_buf.c_str();
            }

            if (trace_stream) {
              *trace_stream << "=== SUMMARY MODEL ===\n";
              *trace_stream << "model=" << summary_model
                            << " ok=" << (sr.ok ? "true" : "false")
                            << " http_status=" << sr.http_status
                            << " dropped_messages=" << input.dropped_messages
                            << " excerpt_truncated=" << (input.truncated ? "true" : "false")
                            << "\n";
              if (!sr.error.empty()) {
                *trace_stream << sr.error << "\n";
              }
            }

            // Surface summary metadata to UIs (avoid leaking the full excerpt).
            Json::Value d(Json::objectValue);
            d["attempt"] = attempt;
            d["summary_model"] = summary_model;
            d["dropped_messages"] = (Json::UInt64)input.dropped_messages;
            d["excerpt_truncated"] = (bool)input.truncated;
            d["ok"] = (bool)sr.ok;
            d["http_status"] = (Json::Int64)sr.http_status;
            push_ev("summary", d);
          }
        }

        agent_run_report_t rep{};
        last_st = agent_run_once(session, &provider, &run_opt, &rep);
        http_status = pctx.last_http_status;
        http_body = pctx.last_body;

        {
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          if (verbose) {
            bool trunc = false;
            d["request_json"] = truncate_for_event(pctx.last_request_body, 64 * 1024, &trunc);
            d["request_truncated"] = trunc;
          }
          push_ev("llm_request", d);
        }
        {
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["http_status"] = (Json::Int64)pctx.last_http_status;
          if (verbose) {
            bool trunc = false;
            d["response_body"] = truncate_for_event(pctx.last_body, 64 * 1024, &trunc);
            d["response_truncated"] = trunc;
          }
          d["stream"] = false;
          push_ev("llm_response", d);
        }

        if (last_st == AGENT_OK) {
          ok = true;
          assistant_text = std::string(rep.assistant_view.content, rep.assistant_view.content_len);
          break;
        }

        if (last_st == AGENT_ERR_CANCELLED) {
          ok = false;
          err = "cancelled";
          break;
        }

        if (!pctx.last_error.empty()) {
          err = pctx.last_error;
        } else {
          err = std::string("agent_run_once failed: ") + std::to_string((int)last_st);
        }

        if (attempt < 2 && last_st == AGENT_ERR_CONTEXT_TOO_LONG) {
          const size_t next = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["http_status"] = (Json::Int64)pctx.last_http_status;
          d["max_chars_before"] = (Json::UInt64)attempt_max_chars;
          d["max_chars_after"] = (Json::UInt64)next;
          push_ev("retry", d);
          attempt_max_chars = next;
          continue;
        }
        break;
      }
    }
  }

  // Stop heartbeat thread and join.
  if (!job_id_local.empty()) {
    heartbeat_stop.store(true);
    if (heartbeat_thread.joinable()) heartbeat_thread.join();
  }

  if (registry) {
    agent_tool_registry_destroy(registry);
  }
  if (need_destroy_host_executor) {
    toolset_host_destroy(&base_executor);
  }

  // Client acknowledgement verification (prevents "false done" reports in interactive UIs).
  //
  // IMPORTANT: only enforce for async jobs (job_id present). For sync `/api/v1/run`, the client receives events
  // only at the end of the request, so blocking here would deadlock the UI (it can't ack what it can't render yet).
  if (ok && require_client_acks && !job_id_local.empty() && !no_session && db_or_null && db_or_null->is_open()) {
    const std::vector<ExpectedClientAck> expected = collect_expected_client_acks(events_out);
    if (!expected.empty()) {
      Json::Value report = verify_expected_client_acks(*db_or_null, session_id, run_ts_ms, expected, /*timeout_ms=*/5000);
      {
        Json::Value d(Json::objectValue);
        d["require_client_acks"] = true;
        d["report"] = report;
        if (!events_out.isArray()) events_out = Json::Value(Json::arrayValue);
        Json::Value e(Json::objectValue);
        e["type"] = "client_ack_verify";
        e["data"] = d;
        events_out.append(e);
      }
      if (report.isObject() && report.isMember("ok") && report["ok"].isBool() && report["ok"].asBool() == false) {
        ok = false;
        err = report.isMember("error") && report["error"].isString() ? report["error"].asString() : "client acknowledgement verification failed";
      }
    }
  }

  Json::Value out(Json::objectValue);
  out["ok"] = ok;
  out["assistant_text"] = assistant_text;
  if (!ok) out["error"] = err;
  if (http_status) out["http_status"] = (Json::Int64)http_status;
  if (!http_body.empty()) out["http_body"] = http_body;
  if (trace_stream) out["trace_text"] = trace_buf.str();
  out["effective_yolo"] = yolo;
  out["effective_host_policy"] = host_policy_to_string(effective_policy);
  out["effective_timeout_ms"] = (Json::Int64)run_cfg.timeout_ms;
  out["effective_stream_assistant"] = stream_assistant;
  out["effective_require_client_acks"] = require_client_acks;
  out["verbose"] = verbose;
  out["events"] = events_out;

  // Canonical persistence: sessions + audit + telemetry in SQLite.
  //
  // Respect `no_session`: "ephemeral" runs should not persist anything to disk.
  if (!no_session && db_or_null && db_or_null->is_open()) {
    // Mirror session messages (as of the end of this run).
    std::vector<std::pair<std::string, std::string>> msgs;
    msgs.reserve(agent_session_message_count(session));
    for (size_t i = 0; i < agent_session_message_count(session); i++) {
      agent_message_view_t v{};
      if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
      msgs.emplace_back(agent_role_to_string(v.role), std::string(v.content, v.content_len));
    }
    (void)db_or_null->replace_session_messages(session_id, msgs, run_ts_ms, nullptr);

    AgentDb::RunRow rr;
    rr.session_id = session_id;
    rr.job_id = job_id_local;
    rr.ts_unix_ms = run_ts_ms;
    rr.prompt = prompt;
    rr.tools = tools;
    rr.model = run_cfg.model;
    rr.base_url = run_cfg.base_url;
    rr.stream_assistant = stream_assistant;
    rr.ok = ok;
    rr.steps_executed = use_tool_loop ? (int64_t)tool_loop_result.steps_executed : 0;
    rr.tool_calls_total = use_tool_loop ? (int64_t)tool_loop_result.tool_records.size() : 0;
    {
      // stop_reason: best-effort extracted from events.
      // - ok=true: "done"
      // - ok=false: error event's `reason` when present, else "error"
      std::string stop_reason = ok ? "done" : "error";
      std::string last_err_reason;
      if (events_out.isArray()) {
        for (Json::ArrayIndex i = 0; i < events_out.size(); i++) {
          const auto& ev = events_out[i];
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
            stop_reason = ok ? "done" : stop_reason;
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
      if (!ok && !last_err_reason.empty()) {
        stop_reason = last_err_reason;
      }
      rr.stop_reason = stop_reason;
      rr.last_error_reason = last_err_reason;
    }
    if (use_tool_loop) {
      // tool_calls_by_tool_json: compact map for troubleshooting.
      Json::Value m(Json::objectValue);
      for (const auto& tr : tool_loop_result.tool_records) {
        if (tr.tool_name.empty()) continue;
        const auto key = tr.tool_name;
        if (!m.isMember(key)) m[key] = (Json::UInt64)0;
        m[key] = (Json::UInt64)(m[key].asUInt64() + 1);
      }
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      rr.tool_calls_by_tool_json = Json::writeString(wb, m);
    }
    rr.error = err;
    rr.http_status = http_status;
    rr.http_body = http_body;

    int64_t run_id = 0;
    if (db_or_null->insert_run(rr, &run_id, nullptr) && run_id > 0) {
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      if (events_out.isArray()) {
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
        for (const auto& ev : events_out) {
          if (!ev.isObject()) continue;
          const auto& t = ev["type"];
          const auto& d = ev["data"];
          if (!t.isString() || !d.isObject()) continue;
          (void)db_or_null->insert_event(run_id, run_ts_ms, t.asString(), Json::writeString(wb, d), nullptr);

          if (t.asString() == "artifact") {
            const auto& art = d["artifact"];
            if (art.isObject()) {
              AgentDb::ArtifactRow ar;
              ar.run_id = run_id;
              ar.ts_unix_ms = run_ts_ms;
              ar.session_id = session_id;
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
                (void)db_or_null->insert_artifact(ar, nullptr);
                // High-leverage UX: mirror artifacts into the durable server-owned Scene so they're visible
                // in the collaboration surface even after refresh (and even if no client RPCs run).
                (void)scene_store_mirror_artifact(db_or_null, session_id, art, ar.tool_call_id, next_scene_ts_ms(), nullptr);
              }
            }
          }
          if (t.asString() == "scene_apply") {
            // Durable Scene update requested by the agent (server-side; refresh-proof).
            const auto& ops = d["ops"];
            if (ops.isArray()) {
              (void)scene_store_apply_ops(db_or_null, session_id, ops, next_scene_ts_ms(), nullptr, nullptr, nullptr);
            }
          }
          if (t.asString() == "ui_action") {
            const auto& act = d["action"];
            if (act.isObject()) {
              AgentDb::UiActionRow ur;
              ur.run_id = run_id;
              ur.ts_unix_ms = run_ts_ms;
              ur.session_id = session_id;
              if (d.isMember("tool_call_id") && d["tool_call_id"].isString()) ur.tool_call_id = d["tool_call_id"].asString();
              if (act.isMember("type") && act["type"].isString()) ur.type = act["type"].asString();
              if (act.isMember("title") && act["title"].isString()) ur.title = act["title"].asString();
              if (act.isMember("message") && act["message"].isString()) ur.message = act["message"].asString();
              if (act.isMember("path") && act["path"].isString()) ur.path = act["path"].asString();
              if (act.isMember("mime") && act["mime"].isString()) ur.mime = act["mime"].asString();
              if (act.isMember("autoplay") && act["autoplay"].isBool()) ur.autoplay = act["autoplay"].asBool();
              if (act.isMember("repeat") && act["repeat"].isInt()) ur.repeat = std::max(1, act["repeat"].asInt());
              ur.action_json = Json::writeString(wb, act);
              (void)db_or_null->insert_ui_action(ur, nullptr);
            }
          }
        }
      }
      if (use_tool_loop && !tool_loop_result.tool_records.empty()) {
        for (const auto& tr : tool_loop_result.tool_records) {
          AgentDb::ToolRecordRow trr;
          trr.run_id = run_id;
          trr.tool_name = tr.tool_name;
          trr.tool_call_id = tr.tool_call_id;
          trr.arguments_json = tr.arguments_json;
          trr.result_text = tr.result_string;
          trr.result_for_prompt_text = tr.result_string_for_prompt;
          trr.result_truncated_for_prompt = tr.result_truncated_for_prompt;
          (void)db_or_null->insert_tool_record(trr, nullptr);
        }
      }
    }

    // Append a per-run audit record (used by `/api/v1/session/audit`).
    if (!session_id.empty()) {
      Json::Value record(Json::objectValue);
      record["ts_unix_ms"] = (Json::Int64)run_ts_ms;
      record["session_id"] = session_id;
      record["ok"] = ok;
      record["model"] = run_cfg.model;
      record["base_url"] = run_cfg.base_url;
      record["tools"] = tools;
      record["yolo"] = yolo;
      record["host_policy"] = host_policy_to_string(effective_policy);
      record["prompt"] = prompt;
      record["assistant_text"] = assistant_text;
      if (http_status) record["http_status"] = (Json::Int64)http_status;
      if (!http_body.empty()) record["http_body"] = http_body;
      if (!ok) record["error"] = err;
      if (events_out.isArray()) {
        record["events"] = events_out;
      }
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      (void)db_or_null->insert_audit_record(session_id, run_ts_ms, run_id, Json::writeString(wb, record), nullptr);
    }
  }

  agent_session_destroy(session);
  return out;
}

}  // namespace

void handle_run_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto started = std::chrono::steady_clock::now();
  std::cerr << "agentd: /api/v1/run start bytes=" << req.body.size() << "\n";
  Json::Value out = run_request_to_json(cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir, req.body, nullptr);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  const bool ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
  std::cerr << "agentd: /api/v1/run done ok=" << (ok ? "true" : "false") << " ms=" << ms << "\n";
  if (out.isObject() && out.isMember("rpc_status") && out["rpc_status"].isInt()) {
    resp->status = out["rpc_status"].asInt();
  }
  resp->body = json_stringify(out);
}

void handle_run_async_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }
  const std::string prompt = args.isMember("prompt") && args["prompt"].isString() ? args["prompt"].asString() : "";
  if (prompt.empty()) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "missing prompt";
    resp->body = json_stringify(o);
    return;
  }

  const std::string job_id = args.isMember("job_id") && args["job_id"].isString() ? args["job_id"].asString() : new_job_id();
  if (job_id.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"empty job_id"})";
    return;
  }
  if (!job_create(job_id)) {
    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "job_id already exists";
    o["job_id"] = job_id;
    resp->body = json_stringify(o);
    return;
  }

  // Log immediately in the request handler (before the background thread starts).
  // This helps diagnose "hangs" where the UI is pointed at the wrong daemon base URL,
  // or where the request never reaches the daemon.
  std::cerr << "agentd: /api/v1/run_async accepted job=" << job_id << " bytes=" << req.body.size() << "\n";

  const std::string body_copy = req.body;
  std::thread([job_id, body_copy, cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir]() mutable {
    const auto started = std::chrono::steady_clock::now();
    std::cerr << "agentd: /api/v1/run_async job=" << job_id << " start bytes=" << body_copy.size() << "\n";
    job_set_status(job_id, "running", "");
    {
      // Emit an immediate event so UIs don't look "stuck" even if the first LLM request is slow
      // or if the run uses tools="none" (no tool-loop events until completion).
      Json::Value d(Json::objectValue);
      d["source"] = "daemon";
      d["job_id"] = job_id;
      d["status"] = "running";
      d["ts_unix_ms"] = (Json::Int64)now_unix_ms();
      job_append_event(job_id, "start", json_stringify(d));
    }
    try {
      Json::Value out = run_request_to_json(cfg, ocfg, db_or_null, tool_ext_or_null, sessions_root_dir, body_copy, job_id.c_str());
      job_set_result(job_id, out);
    } catch (const std::exception& e) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = std::string("uncaught exception: ") + e.what();
      job_set_result(job_id, o);
    } catch (...) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "uncaught unknown exception";
      job_set_result(job_id, o);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    JobState s;
    const bool got = job_get(job_id, &s);
    const bool ok = got && s.result.isObject() && s.result.isMember("ok") && s.result["ok"].isBool() && s.result["ok"].asBool();
    std::cerr << "agentd: /api/v1/run_async job=" << job_id << " done ok=" << (ok ? "true" : "false") << " ms=" << ms << "\n";
  }).detach();

  resp->status = 202;
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = job_id;
  resp->body = json_stringify(o);
}

}  // namespace agentd
