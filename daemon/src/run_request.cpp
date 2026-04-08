#include "run_endpoints.h"

#include "approval_queue.h"
#include "automation_profile.h"
#include "daemon_auth.h"
#include "client_profiles.h"
#include "default_system_prompt.h"
#include "file_persistor.h"
#include "host_session_prompts.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"
#include "llm_usage.h"
#include "openai_provider.h"
#include "policy_hooks.h"
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
#include "trace_id_util.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#include "base64.h"
#include "openai_client.h"
#include "openai_stream_adapter.h"

#include "agent/agent.h"
#include "agent/runner.h"
#include "run_request_config.h"
#include "run_request_parse.h"
#include "run_request_persist.h"
#include "run_request_tool_loop.h"

#include "run_client_acks.h"
#include "run_memory_context.h"
#include "run_multimodal.h"
#include "run_endpoints_internal.h"

#include "agent/multimodal_prefix.h"

#include <json/json.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace agentd {
namespace {

static void ensure_trace_id_in_events(Json::Value* arr, const std::string& trace_id) {
  if (!arr || !arr->isArray() || trace_id.empty()) return;
  for (Json::ArrayIndex i = 0; i < arr->size(); i++) {
    Json::Value& ev = (*arr)[i];
    if (!ev.isObject()) continue;
    if (!ev.isMember("trace_id")) ev["trace_id"] = trace_id;
  }
}

static void inject_schema_into_events(Json::Value* arr) {
  if (!arr || !arr->isArray()) return;
  for (Json::ArrayIndex i = 0; i < arr->size(); i++) {
    Json::Value& ev = (*arr)[i];
    if (!ev.isObject()) continue;
    if (ev.isMember("schema")) continue;
    if (!ev.isMember("type") || !ev["type"].isString()) continue;
    const std::string type = ev["type"].asString();
    const char* schema = nullptr;
    if (type == "assistant_delta") schema = "run_event_payload_assistant_delta_v1";
    else if (type == "assistant_message") schema = "run_event_payload_assistant_message_v1";
    else if (type == "user_message") schema = "run_event_payload_user_message_v1";
    else if (type == "tool_call") schema = "run_event_payload_tool_call_v1";
    else if (type == "tool_result") schema = "run_event_payload_tool_result_v1";
    else if (type == "llm_usage") schema = "run_event_payload_llm_usage_v1";
    else if (type == "artifact") schema = "run_event_payload_artifact_v1";
    else if (type == "ui_action") schema = "run_event_payload_ui_action_v1";
    else if (type == "heartbeat") schema = "run_event_payload_heartbeat_v1";
    else if (type == "error") schema = "run_event_payload_error_v1";
    else if (type == "policy_decision") schema = "run_event_payload_policy_decision_v1";
    else if (type == "approval_request") schema = "run_event_payload_approval_request_v1";
    else if (type == "approval_update") schema = "run_event_payload_approval_update_v1";
    else if (type == "approval_resolved") schema = "run_event_payload_approval_resolved_v1";
    else if (type == "team_handoff") schema = "run_event_payload_team_handoff_v1";
    else if (type == "team_quorum_request") schema = "run_event_payload_team_quorum_request_v1";
    else if (type == "team_quorum_result") schema = "run_event_payload_team_quorum_result_v1";
    else if (type == "team_member_result") schema = "run_event_payload_team_member_result_v1";
    if (schema) ev["schema"] = schema;
  }
}

static Json::Value run_request_error(int rpc_status, const std::string& msg) {
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["rpc_status"] = rpc_status;
  o["error"] = msg;
  o["err"] = msg;
  o["code"] = error_code_from_message(msg);
  return o;
}

static bool is_safe_memory_scope_id(const std::string& s) {
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

static bool is_safe_team_id(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
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

static std::string normalize_memory_scope_mode(const std::string& raw) {
  const std::string v = lower_copy(trim_copy(raw));
  if (v.empty()) return "";
  if (v == "read_only" || v == "readonly" || v == "ro") return "read_only";
  if (v == "read_write" || v == "readwrite" || v == "read-write" || v == "rw") return "read_write";
  return "";
}

static Json::Value run_request_to_json_impl(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const std::string& request_body,
  const char* job_id_or_null,
  RunCancelCallback should_cancel_or_null,
  void* should_cancel_ctx_or_null
) {
  const auto started_steady = std::chrono::steady_clock::now();
  RunRequestParseResult parsed;
  Json::Value parse_error;
  if (!parse_run_request_base(daemon_cfg, request_body, &parsed, &parse_error)) {
    return parse_error;
  }
  const Json::Value& args = parsed.args;
  const std::string& prompt = parsed.prompt;
  std::string trace_id = parsed.trace_id;
  std::string tools = parsed.tools;

  OpenAIClientConfig run_cfg = build_run_config_from_args(daemon_cfg, ocfg, args);
  std::string automation_profile;
  if (args.isMember("automation_profile") && args["automation_profile"].isString()) {
    automation_profile = args["automation_profile"].asString();
  }
  DaemonConfig effective_cfg = daemon_cfg;
  if (!automation_profile.empty()) {
    std::string aerr;
    if (!automation_profile_apply(automation_profile, &effective_cfg, &aerr)) {
      return run_request_error(400, aerr.empty() ? "invalid automation_profile" : aerr);
    }
  }
  const std::string effective_automation_profile = automation_profile_from_config(effective_cfg);
  const bool requested_yolo_set = args.isMember("yolo") && args["yolo"].isBool();
  const bool requested_yolo = requested_yolo_set ? args["yolo"].asBool() : effective_cfg.yolo_default;
  const bool yolo = sandbox_tighten_yolo(effective_cfg.yolo_default, requested_yolo, requested_yolo_set);
  const bool no_default_system =
    args.isMember("no_default_system") && args["no_default_system"].isBool() ? args["no_default_system"].asBool() : daemon_cfg.no_default_system;
  const std::string system_profile_raw =
    args.isMember("system_profile") && args["system_profile"].isString() ? args["system_profile"].asString() : daemon_cfg.system_profile;
  const std::string system_profile = trim_copy(system_profile_raw) == "jules_codex" ? "jules_codex" : "default";
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
    return run_request_error(400, "invalid session_id");
  }
  std::string team_id;
  if (args.isMember("team_id")) {
    if (!args["team_id"].isString()) {
      return run_request_error(400, "invalid team_id");
    }
    team_id = args["team_id"].asString();
    if (!team_id.empty() && !is_safe_team_id(team_id)) {
      return run_request_error(400, "invalid team_id");
    }
  }
  if (args.isMember("tools_root")) {
    return run_request_error(400, "tools_root was removed; omit it and use explicit paths or session_id");
  }
  HostToolsetPolicyMode requested_policy = effective_cfg.host_policy;
  if (args.isMember("host_policy") && args["host_policy"].isString()) {
    HostToolsetPolicyMode p{};
    const std::string s = args["host_policy"].asString();
    if (!host_policy_from_string(s, &p)) {
      return run_request_error(400, "invalid host_policy (expected: full|readonly)");
    }
    requested_policy = p;
  }
  const HostToolsetPolicyMode effective_policy = tighten_host_policy(effective_cfg.host_policy, requested_policy);
  if (!no_session && !sessions_root_dir.empty()) {
    const std::filesystem::path sr = session_root_path(sessions_root_dir, session_id);
    if (sr.empty()) {
      return run_request_error(400, "invalid session_id");
    }
    std::error_code ec;
    (void)std::filesystem::create_directories(sr / "work", ec);
    ec.clear();
    (void)std::filesystem::create_directories(sr / "out", ec);
  }

  // Optional multimodal inputs:
  // - UI uploads files into the session folder via POST /api/v1/session/upload
  // - Run requests then reference them by session-relative `path` entries in `input_files`
  //
  // For OpenAI-compatible providers that support multimodal messages, we translate these into a
  // `messages[].content` array containing text + image parts.
  std::string prompt_for_llm = prompt;
  // Effective stream_assistant may be tightened/overridden internally (e.g., to support multimodal content
  // for tools=none even when the caller did not request streaming).
  bool effective_stream_assistant = false;
  size_t input_image_count = 0;
  bool input_had_any_files = false;
  if (args.isMember("input_files") && args["input_files"].isArray() && !args["input_files"].empty()) {
    input_had_any_files = true;
    if (no_session) {
      return run_request_error(400, "input_files requires session persistence (no_session=false)");
    }
    if (sessions_root_dir.empty()) {
      return run_request_error(500, "sessions_root_dir not configured");
    }

    const std::filesystem::path sr = session_root_path(sessions_root_dir, session_id);
    if (sr.empty()) {
      return run_request_error(400, "invalid session_id");
    }

    Json::Value mm(Json::objectValue);
    Json::Value images(Json::arrayValue);
    Json::Value files(Json::arrayValue);

    const Json::Value arr = args["input_files"];
    for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
      const Json::Value& item = arr[i];
      std::string rel;
      std::string mime;
      std::string name;
      std::string kind;
      if (item.isString()) {
        rel = item.asString();
      } else if (item.isObject()) {
        if (item.isMember("path") && item["path"].isString()) rel = item["path"].asString();
        if (item.isMember("mime") && item["mime"].isString()) mime = item["mime"].asString();
        if (item.isMember("name") && item["name"].isString()) name = item["name"].asString();
        if (item.isMember("kind") && item["kind"].isString()) kind = item["kind"].asString();
      } else {
        continue;
      }
      if (!is_safe_relpath_ascii(rel)) continue;
      std::filesystem::path abs = (sr / std::filesystem::path(rel)).lexically_normal();
      if (!path_is_within_root(sr, abs)) continue;

      if (name.empty()) name = abs.filename().string();
      if (mime.empty()) mime = content_type_from_path(abs);
      if (kind.empty()) {
        const std::string m = lower_copy(mime);
        if (m.rfind("image/", 0) == 0) kind = "image";
        else kind = "file";
      }

      if (kind == "image") {
        std::string bytes;
        // Cap image sizes to avoid blowing up the prompt context.
        const size_t kMaxImageBytes = 6u * 1024u * 1024u;
        if (read_file_bytes_capped(abs, kMaxImageBytes, &bytes)) {
          const std::string b64 = base64_encode(bytes.data(), bytes.size());
          Json::Value im(Json::objectValue);
          im["name"] = name;
          im["mime"] = mime.empty() ? std::string("image/png") : mime;
          im["b64"] = b64;
          images.append(im);
          input_image_count++;
        } else {
          Json::Value f(Json::objectValue);
          f["name"] = name;
          f["mime"] = mime;
          f["text"] = std::string("Attachment stored at ") + rel + " (image too large to inline)";
          f["truncated"] = true;
          files.append(f);
        }
      } else {
        std::string bytes;
        const size_t kMaxFileBytes = 256u * 1024u;
        if (read_file_bytes_capped(abs, kMaxFileBytes, &bytes) && looks_texty(bytes)) {
          Json::Value f(Json::objectValue);
          f["name"] = name;
          f["mime"] = mime;
          f["text"] = bytes;
          f["truncated"] = false;
          files.append(f);
        } else {
          Json::Value f(Json::objectValue);
          f["name"] = name;
          f["mime"] = mime;
          f["text"] = std::string("Attachment stored at ") + rel + " (not inlined; use tools to inspect)";
          f["truncated"] = true;
          files.append(f);
        }
      }
    }

    if (!images.empty()) mm["images"] = images;
    if (!files.empty()) mm["files"] = files;
    if (!mm.empty()) {
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      prompt_for_llm = std::string(kMultimodalPrefix) + Json::writeString(wb, mm) + "\n" + prompt;
    }
  }

  // If tools are enabled and the provider requires tools=none for vision, we keep tools enabled and instead
  // (optionally) run a lightweight "vision prefetch" call to obtain an image description that can be injected
  // into the tool-loop prompt as plain text. This keeps tools available for the rest of the turn without
  // hardcoding tools=none as a startup gate.

  uint64_t max_steps_u64 = 0;
  size_t max_steps =
    json_get_u64_nonneg(args, "max_steps", &max_steps_u64) ? (size_t)max_steps_u64 : daemon_cfg.max_steps_default;
  uint64_t max_tool_calls_total_u64 = 0;
  size_t max_tool_calls_total =
    json_get_u64_nonneg(args, "max_tool_calls_total", &max_tool_calls_total_u64)
      ? (size_t)max_tool_calls_total_u64
      : daemon_cfg.max_tool_calls_total_default;
  uint64_t max_tool_calls_per_tool_u64 = 0;
  size_t max_tool_calls_per_tool =
    json_get_u64_nonneg(args, "max_tool_calls_per_tool", &max_tool_calls_per_tool_u64)
      ? (size_t)max_tool_calls_per_tool_u64
      : daemon_cfg.max_tool_calls_per_tool_default;
  uint64_t max_tool_call_args_chars_u64 = 0;
  size_t max_tool_call_args_chars =
    json_get_u64_nonneg(args, "max_tool_call_args_chars", &max_tool_call_args_chars_u64)
      ? (size_t)max_tool_call_args_chars_u64
      : daemon_cfg.max_tool_call_args_chars_default;
  uint64_t max_tool_result_chars_u64 = 0;
  size_t max_tool_result_chars =
    json_get_u64_nonneg(args, "max_tool_result_chars", &max_tool_result_chars_u64)
      ? (size_t)max_tool_result_chars_u64
      : daemon_cfg.max_tool_result_chars_default;

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
  effective_stream_assistant = stream_assistant;
  uint64_t max_capture_bytes_u64 = 0;
  const size_t max_capture_bytes =
    json_get_u64_nonneg(args, "max_capture_bytes", &max_capture_bytes_u64) ? (size_t)max_capture_bytes_u64 : (size_t)256 * 1024;

  // Memory retrieval policy (durable on-disk Markdown memory injection into the tool loop).
  MemoryContextPolicy mem_pol;
  mem_pol.salience_max_items = daemon_cfg.memory_salience_max_items;
  mem_pol.salience_structured_max_items = daemon_cfg.memory_salience_structured_max_items;
  mem_pol.salience_daily_max_items = daemon_cfg.memory_salience_daily_max_items;
  mem_pol.salience_half_life_days = daemon_cfg.memory_salience_half_life_days;
  mem_pol.salience_importance_weight = daemon_cfg.memory_salience_importance_weight;
  std::string mem_query;
  bool mem_daily_days_set = false;
  if (args.isMember("memory_context_mode") && args["memory_context_mode"].isString()) {
    const std::string mode = lower_copy(trim_copy(args["memory_context_mode"].asString()));
    if (mode == "search") mem_pol.mode = MemoryContextMode::Search;
    else if (mode == "index" || mode == "progressive") mem_pol.mode = MemoryContextMode::Index;
    else if (mode == "salience" || mode == "dynamic") mem_pol.mode = MemoryContextMode::Salience;
    else mem_pol.mode = MemoryContextMode::Files;
  }
  if (args.isMember("memory_include_structured") && args["memory_include_structured"].isBool()) {
    mem_pol.include_structured = args["memory_include_structured"].asBool();
  }
  if (args.isMember("memory_include_core") && args["memory_include_core"].isBool()) {
    mem_pol.include_core = args["memory_include_core"].asBool();
  }
  if (args.isMember("memory_include_daily") && args["memory_include_daily"].isBool()) {
    mem_pol.include_daily = args["memory_include_daily"].asBool();
  }
  if (args.isMember("memory_include_session") && args["memory_include_session"].isBool()) {
    mem_pol.include_session = args["memory_include_session"].asBool();
  }
  if (args.isMember("memory_daily_days") && args["memory_daily_days"].isInt()) {
    mem_pol.daily_days = std::max(0, std::min(args["memory_daily_days"].asInt(), 31));
    mem_daily_days_set = true;
  }
  if (mem_pol.mode == MemoryContextMode::Salience && !mem_daily_days_set) {
    mem_pol.daily_days = std::max(0, std::min(daemon_cfg.memory_salience_daily_days, 31));
  }
  if (args.isMember("memory_total_cap") && (args["memory_total_cap"].isInt64() || args["memory_total_cap"].isInt())) {
    const int64_t v = args["memory_total_cap"].asInt64();
    if (v >= 0) mem_pol.total_cap = (size_t)std::min<int64_t>(v, 40000);
  }
  if (args.isMember("memory_search_query") && args["memory_search_query"].isString()) {
    mem_query = trim_copy(args["memory_search_query"].asString());
  }
  if (args.isMember("memory_search_use_index") && args["memory_search_use_index"].isBool()) {
    mem_pol.search_use_index = args["memory_search_use_index"].asBool();
  }
  if (args.isMember("memory_search_case_sensitive") && args["memory_search_case_sensitive"].isBool()) {
    mem_pol.search_case_sensitive = args["memory_search_case_sensitive"].asBool();
  }
  if (args.isMember("memory_search_order") && args["memory_search_order"].isString()) {
    const std::string raw = trim_copy(args["memory_search_order"].asString());
    const std::string v = lower_copy(raw);
    if (v == "newest" || v == "newest_first" || v == "latest") {
      mem_pol.search_order = MemorySearchOrder::Newest;
    } else if (v == "oldest" || v == "oldest_first" || v == "earliest") {
      mem_pol.search_order = MemorySearchOrder::Oldest;
    } else {
      mem_pol.search_order = MemorySearchOrder::Ranked;
    }
  }
  if (args.isMember("memory_search_fallback_to_files") && args["memory_search_fallback_to_files"].isBool()) {
    mem_pol.search_fallback_to_files = args["memory_search_fallback_to_files"].asBool();
  }
  if (args.isMember("memory_search_max_results") && args["memory_search_max_results"].isInt()) {
    mem_pol.search_max_results = std::max(1, args["memory_search_max_results"].asInt());
  }
  if (args.isMember("memory_search_max_snippet_chars") && args["memory_search_max_snippet_chars"].isInt()) {
    mem_pol.search_max_snippet_chars = std::max(80, args["memory_search_max_snippet_chars"].asInt());
  }
  if (args.isMember("memory_search_context_lines") && args["memory_search_context_lines"].isInt()) {
    mem_pol.search_context_lines = std::max(0, args["memory_search_context_lines"].asInt());
  }
  if (args.isMember("memory_salience_max_items") && args["memory_salience_max_items"].isInt()) {
    mem_pol.salience_max_items = std::max(1, args["memory_salience_max_items"].asInt());
  }
  if (args.isMember("memory_salience_structured_max_items") && args["memory_salience_structured_max_items"].isInt()) {
    mem_pol.salience_structured_max_items = std::max(0, args["memory_salience_structured_max_items"].asInt());
  }
  if (args.isMember("memory_salience_daily_max_items") && args["memory_salience_daily_max_items"].isInt()) {
    mem_pol.salience_daily_max_items = std::max(0, args["memory_salience_daily_max_items"].asInt());
  }
  if (args.isMember("memory_salience_half_life_days")) {
    if (args["memory_salience_half_life_days"].isString()) {
      try { mem_pol.salience_half_life_days = std::stod(args["memory_salience_half_life_days"].asString()); } catch (...) {}
    } else if (args["memory_salience_half_life_days"].isDouble()) {
      mem_pol.salience_half_life_days = args["memory_salience_half_life_days"].asDouble();
    } else if (args["memory_salience_half_life_days"].isInt() || args["memory_salience_half_life_days"].isUInt()) {
      mem_pol.salience_half_life_days = args["memory_salience_half_life_days"].asDouble();
    }
    if (mem_pol.salience_half_life_days < 0) mem_pol.salience_half_life_days = 0;
  }
  if (args.isMember("memory_salience_importance_weight")) {
    if (args["memory_salience_importance_weight"].isString()) {
      try { mem_pol.salience_importance_weight = std::stod(args["memory_salience_importance_weight"].asString()); } catch (...) {}
    } else if (args["memory_salience_importance_weight"].isDouble()) {
      mem_pol.salience_importance_weight = args["memory_salience_importance_weight"].asDouble();
    } else if (args["memory_salience_importance_weight"].isInt() || args["memory_salience_importance_weight"].isUInt()) {
      mem_pol.salience_importance_weight = args["memory_salience_importance_weight"].asDouble();
    }
    if (mem_pol.salience_importance_weight < 0) mem_pol.salience_importance_weight = 0;
  }
  mem_pol.salience_max_items = std::max(1, std::min(200, mem_pol.salience_max_items));
  mem_pol.salience_structured_max_items = std::max(0, std::min(200, mem_pol.salience_structured_max_items));
  mem_pol.salience_daily_max_items = std::max(0, std::min(200, mem_pol.salience_daily_max_items));

  std::string memory_scope_id;
  std::string memory_scope_mode;
  if (args.isMember("memory_scope_id") && args["memory_scope_id"].isString()) {
    memory_scope_id = trim_copy(args["memory_scope_id"].asString());
    if (!memory_scope_id.empty() && !is_safe_memory_scope_id(memory_scope_id)) {
      return run_request_error(400, "invalid memory_scope_id (unsafe characters)");
    }
  }
  if (args.isMember("memory_scope_mode") && args["memory_scope_mode"].isString()) {
    memory_scope_mode = normalize_memory_scope_mode(args["memory_scope_mode"].asString());
    if (memory_scope_mode.empty()) {
      return run_request_error(400, "invalid memory_scope_mode (expected: read_only|read_write)");
    }
  }
  if (memory_scope_id.empty() && !memory_scope_mode.empty()) {
    return run_request_error(400, "memory_scope_mode requires memory_scope_id");
  }
  if (!memory_scope_id.empty() && memory_scope_mode.empty()) {
    memory_scope_mode = "read_write";
  }
  std::string memory_root_override;
  bool memory_write_allowed = true;
  if (!memory_scope_id.empty()) {
    if (effective_cfg.state_dir.empty()) {
      return run_request_error(400, "memory_scope_id requires daemon state_dir");
    }
    memory_root_override =
      (std::filesystem::path(effective_cfg.state_dir) / "memory_scopes" / memory_scope_id).lexically_normal().string();
    if (memory_scope_mode == "read_only") {
      memory_write_allowed = false;
    }
  }

  std::string job_id_local = (job_id_or_null && job_id_or_null[0]) ? std::string(job_id_or_null) : std::string();
  const int64_t run_ts_ms = now_unix_ms();
  if (!job_id_local.empty()) {
    job_set_trace_id(job_id_local, trace_id);
  }

  PolicyConfig policy_cfg = policy_config_from_daemon(effective_cfg);
  std::string policy_err;
  if (!policy_apply_overrides_from_json(args, &policy_cfg, &policy_err)) {
    return run_request_error(400, policy_err.empty() ? "invalid policy override" : policy_err);
  }
  PolicyHookCtx policy_ctx;
  const bool policy_active = (policy_cfg.mode != PolicyMode::Off);
  if (policy_active) {
    policy_prepare(&policy_ctx, policy_cfg, trace_id, job_id_local, session_id);
    policy_emit_start(&policy_ctx);
    policy_apply_budget_caps(
      &policy_ctx,
      &max_steps,
      &max_tool_calls_total,
      &max_tool_calls_per_tool,
      &max_tool_call_args_chars,
      &max_tool_result_chars
    );
  }

  agent_session_t* session = nullptr;
  if (!no_session) {
    if (!db_or_null || !db_or_null->is_open()) {
      return run_request_error(500, "db not available (session persistence required)");
    }
    std::string load_err;
    if (!load_session_from_db(*db_or_null, session_id, &session, &load_err)) {
      return run_request_error(500, load_err.empty() ? "failed to load session from db" : load_err);
    }
  } else {
    const agent_status_t st = agent_session_create(&session);
    if (st != AGENT_OK) {
      Json::Value o = run_request_error(500, "failed to create session");
      o["status"] = (Json::Int64)st;
      return o;
    }
  }

  std::string project_instructions_prompt;
  if (!no_default_system && tools == "host") {
    std::error_code cwd_ec;
    const std::filesystem::path project_instruction_root = std::filesystem::current_path(cwd_ec);
    if (!cwd_ec) {
      project_instructions_prompt = build_project_instructions_system_prompt(project_instruction_root);
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
      if (!no_default_system && tools == "host" && !project_instructions_prompt.empty()) {
        agent_session_add_message(session, AGENT_ROLE_SYSTEM, project_instructions_prompt.c_str());
      }
    } else {
      if (!no_default_system && tools == "host") {
        agent_session_add_message(session, AGENT_ROLE_SYSTEM, host_system_prompt_for_profile(system_profile.c_str()));
        if (!client_kind.empty()) {
          const std::string profile = client_profile_system_prompt(client_kind);
          if (!profile.empty()) {
            agent_session_add_message(session, AGENT_ROLE_SYSTEM, profile.c_str());
          }
        }
        if (!project_instructions_prompt.empty()) {
          agent_session_add_message(session, AGENT_ROLE_SYSTEM, project_instructions_prompt.c_str());
        }
      }
    }
  } else {
    // For long-lived sessions (e.g. Web UI "default"), do not rely on "empty session" to ensure
    // essential host/tool + DoD guidance is present. If the session was created via tools=none or
    // imported from an older version, it may have no system prompt at all.
    const std::string client_profile_prompt =
      client_kind.empty() ? std::string() : client_profile_system_prompt(client_kind);
    const std::string client_profile_marker =
      client_kind.empty() ? std::string() : (std::string("CLIENT_PROFILE=") + client_kind);
    const bool changed = (!no_default_system && tools == "host")
      ? ensure_pinned_host_session_prompts(
          &session, system_profile, client_profile_prompt, client_profile_marker, project_instructions_prompt)
      : false;
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
  bool vision_prefetch_attempted = false;
  bool vision_prefetch_ok = false;

  if (tools == "basic") {
    if (toolset_basic_create(&registry, &base_executor) != AGENT_OK) {
      Json::Value o = run_request_error(500, "failed to init toolset_basic");
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
    if (should_cancel_or_null) {
      hcfg.should_cancel = should_cancel_or_null;
      hcfg.should_cancel_ctx = should_cancel_ctx_or_null;
    } else if (!job_id_local.empty()) {
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
    if (!memory_root_override.empty()) {
      hcfg.memory_root_override = memory_root_override;
      hcfg.memory_write_allowed = memory_write_allowed;
    }
    if (toolset_host_create(hcfg, &registry, &base_executor) != AGENT_OK) {
      Json::Value o = run_request_error(500, "failed to init toolset_host");
      agent_session_destroy(session);
      return o;
    }
    need_destroy_host_executor = true;
    executor = base_executor;
  } else if (tools != "none") {
    Json::Value o = run_request_error(400, "invalid tools (expected: none|basic|host)");
    agent_session_destroy(session);
    return o;
  }

  // Optional tool extension: allow embedding hosts to append extra tools and execute them.
  // We only dispatch names added by the extension.
  if (registry && tool_ext_or_null && tool_ext_or_null->register_tools) {
    const size_t before = agent_tool_registry_count(registry);
    const agent_status_t st = tool_ext_or_null->register_tools(tool_ext_or_null->ctx, registry);
    if (st != AGENT_OK) {
      Json::Value o = run_request_error(500, "tool extension register_tools failed");
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

  ApprovalGateCtx approval_gate{};
  const bool approvals_enabled = policy_active && use_tool_loop &&
    (!policy_cfg.approval_rules.empty() || !policy_cfg.approval_tools.empty());
  if (approvals_enabled) {
    if (!policy_cfg.approval_rules.empty()) {
      for (const auto& rule : policy_cfg.approval_rules) {
        if (rule.tool_names.empty()) continue;
        ApprovalGateRule gate_rule;
        gate_rule.required = std::max(0, rule.required);
        gate_rule.roles = rule.roles;
        gate_rule.timeout_ms = std::max<int64_t>(0, rule.timeout_ms);
        gate_rule.require_distinct_roles = rule.require_distinct_roles;
        gate_rule.best_effort = rule.best_effort;
        for (const auto& tool : rule.tool_names) {
          const std::string name = trim_copy(tool);
          if (!name.empty()) approval_gate.tool_rules[name] = gate_rule;
        }
      }
    } else {
      for (const auto& tool : policy_cfg.approval_tools) {
        if (!tool.empty()) approval_gate.toolset.insert(tool);
      }
      approval_gate.required = std::max(0, policy_cfg.approval_required);
      approval_gate.roles = policy_cfg.approval_roles;
      approval_gate.timeout_ms = std::max<int64_t>(0, policy_cfg.approval_timeout_ms);
    }
    approval_gate.poll_ms = std::max<int64_t>(1, policy_cfg.approval_poll_ms);
    approval_gate.enforce = (policy_cfg.mode == PolicyMode::Enforce);
    approval_gate.audit = (policy_cfg.mode == PolicyMode::Audit);
    approval_gate.db = db_or_null;
    approval_gate.hook = &policy_ctx;
    approval_gate.trace_id = trace_id;
    approval_gate.session_id = session_id;
    approval_gate.job_id = job_id_local;
    approval_gate.team_id = team_id;
    if (approval_gate.enforce && (!db_or_null || !db_or_null->is_open())) {
      if (registry) agent_tool_registry_destroy(registry);
      if (need_destroy_host_executor) {
        toolset_host_destroy(&base_executor);
      }
      agent_session_destroy(session);
      return run_request_error(500, "db not available (approvals require db)");
    }
  }

  PolicyToolExecutorCtx policy_exec_ctx{};
  if (policy_active && use_tool_loop && executor.execute) {
    policy_exec_ctx.base = executor;
    policy_exec_ctx.hook = &policy_ctx;
    policy_exec_ctx.approval_gate = approvals_enabled ? (void*)&approval_gate : nullptr;
    executor.ctx = &policy_exec_ctx;
    executor.execute = policy_tool_execute;
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
  // tool-loop helpers moved into run_request_tool_loop.{h,cpp}

  std::atomic<bool> heartbeat_stop{false};
  std::atomic<int64_t> heartbeat_last_any_event_ms{now_unix_ms()};
  std::atomic<int64_t> heartbeat_last_non_ms{now_unix_ms()};
  std::atomic<int> heartbeat_phase{kPhaseIdle};
  std::thread heartbeat_thread;
  if (!job_id_local.empty()) {
    heartbeat_thread = std::thread([&]() {
      // Emit a best-effort heartbeat while a job is running to avoid the appearance of "hangs"
      // during long tool exec (sleep/build) or slow LLM responses.
      int64_t last_db_touch_ms = 0;
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
          if (job_is_cancel_requested(job_id_local)) return;
          daemon_job_emit_heartbeat(job_id_local, heartbeat_phase.load(), since_non, since_any);
          heartbeat_last_any_event_ms.store(now);

          // Best-effort: persist a heartbeat timestamp so polling UIs can distinguish "stalled" vs "restarted".
          // Throttle DB writes to keep overhead negligible.
          if (db_or_null && db_or_null->is_open() && (last_db_touch_ms == 0 || (now - last_db_touch_ms) >= 2000)) {
            AgentDb::JobRow jr;
            jr.job_id = job_id_local;
            jr.updated_unix_ms = now;
            jr.status = "running";
            jr.cancel_requested = job_is_cancel_requested(job_id_local);
            jr.last_heartbeat_unix_ms = now;
            std::string db_err;
            (void)db_or_null->upsert_job(jr, &db_err);
            last_db_touch_ms = now;
          }
        }
      }
    });
  }

  if (use_tool_loop) {
    RunRequestToolLoopInput tl_in;
    tl_in.daemon_cfg = &daemon_cfg;
    tl_in.args = &args;
    tl_in.run_cfg = &run_cfg;
    tl_in.prompt = &prompt;
    tl_in.prompt_for_llm = &prompt_for_llm;
    tl_in.trace_id = &trace_id;
    tl_in.session_id = &session_id;
    tl_in.tools = &tools;
    tl_in.no_session = no_session;
    tl_in.no_default_system = no_default_system;
    tl_in.mem_pol = &mem_pol;
    tl_in.mem_query = &mem_query;
    if (!memory_root_override.empty()) {
      tl_in.memory_root_override = &memory_root_override;
    }
    tl_in.max_steps = max_steps;
    tl_in.max_tool_calls_total = max_tool_calls_total;
    tl_in.max_tool_calls_per_tool = max_tool_calls_per_tool;
    tl_in.max_tool_call_args_chars = max_tool_call_args_chars;
    tl_in.max_tool_result_chars = max_tool_result_chars;
    tl_in.tool_call_limits = &tool_call_limits;
    tl_in.max_capture_bytes = max_capture_bytes;
    tl_in.max_chars = max_chars;
    tl_in.keep_last = keep_last;
    tl_in.verbose = verbose;
    tl_in.stream_assistant = stream_assistant;
    tl_in.session = session;
    tl_in.registry = registry;
    tl_in.executor = &executor;
    tl_in.trace_stream = trace_stream;
    tl_in.should_cancel_or_null = should_cancel_or_null;
    tl_in.should_cancel_ctx_or_null = should_cancel_ctx_or_null;
    tl_in.job_id = &job_id_local;
    tl_in.policy_hook = policy_active ? &policy_ctx : nullptr;
    tl_in.heartbeat_last_any_event_ms = &heartbeat_last_any_event_ms;
    tl_in.heartbeat_last_non_ms = &heartbeat_last_non_ms;
    tl_in.heartbeat_phase = &heartbeat_phase;

    RunRequestToolLoopResult tl_out = run_request_tool_loop(tl_in);
    ok = tl_out.ok;
    assistant_text = std::move(tl_out.assistant_text);
    err = std::move(tl_out.err);
    http_status = tl_out.http_status;
    http_body = std::move(tl_out.http_body);
    events_out = std::move(tl_out.events_out);
    tool_loop_result = std::move(tl_out.tool_loop_result);
    vision_prefetch_attempted = tl_out.vision_prefetch_attempted;
    vision_prefetch_ok = tl_out.vision_prefetch_ok;
  } else {
    agent_session_add_message(session, AGENT_ROLE_USER, prompt.c_str());

    const bool stream_assistant_none = stream_assistant || (prompt_for_llm != prompt);
    effective_stream_assistant = stream_assistant_none;

    events_out = Json::Value(Json::arrayValue);
    auto push_ev = [&](const std::string& type, const Json::Value& data) {
      Json::Value e(Json::objectValue);
      e["type"] = type;
      if (!trace_id.empty()) e["trace_id"] = trace_id;
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
      Json::Value mm(Json::nullValue);
      std::string user_text = prompt_for_llm;
      const bool has_mm = try_parse_multimodal_prefix(prompt_for_llm, &mm, &user_text) && mm.isObject();
      d["user_content"] = user_text;
      if (has_mm) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string mm_json = Json::writeString(wb, mm);
        const size_t mm_bytes = mm_json.size();
        if (max_capture_bytes > 0 && mm_bytes > max_capture_bytes) {
          d["user_mm_json"] = mm_json.substr(0, max_capture_bytes);
          d["user_mm_truncated"] = (Json::Int64)1;
        } else {
          d["user_mm_json"] = mm_json;
          d["user_mm_truncated"] = (Json::Int64)0;
        }
        d["user_mm_bytes"] = (Json::Int64)mm_bytes;
      }
      push_ev("user_message", d);
    }

    // Surface provider retries (429/5xx/timeouts) as structured events so async jobs can explain
    // "why it was slow" and DB telemetry has enough context for diagnosis.
    using PushEvFn = decltype(push_ev);
    struct RetryPushCtx {
      PushEvFn* push = nullptr;
    } retry_ctx;
    retry_ctx.push = &push_ev;
    run_cfg.on_retry = [](void* vctx, const char* data_json) {
      auto* c = static_cast<RetryPushCtx*>(vctx);
      if (!c || !c->push) return;
      Json::Value d(Json::objectValue);
      if (data_json && data_json[0]) {
        std::string perr;
        if (!json_parse_object(std::string(data_json), &d, &perr)) {
          d = std::string(data_json);
        }
      }
      if (d.isObject() && !d.isMember("scope")) d["scope"] = "provider";
      (*c->push)("retry", d);
    };
    run_cfg.on_retry_ctx = &retry_ctx;
    {
      Json::Value d(Json::objectValue);
      d["model"] = run_cfg.model;
      d["tools"] = "none";
      d["verbose"] = verbose;
      d["stream_assistant"] = stream_assistant_none;
      push_ev("start", d);
    }

    auto should_cancel_run = [&]() -> bool {
      if (should_cancel_or_null && should_cancel_or_null(should_cancel_ctx_or_null)) return true;
      return !job_id_local.empty() && job_is_cancel_requested(job_id_local);
    };

    if (stream_assistant_none) {
      // Retry loop for providers that reject over-long contexts.
      // For stateless providers, retrying with a tighter compaction budget is equivalent to "spawning a new session".
      size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
      const size_t keep = (keep_last == 0 ? 16 : keep_last);

      for (int attempt = 0; attempt < 3; attempt++) {
        if (should_cancel_run()) {
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
            std::string content(v.content, v.content_len);
            if (i + 1 == n && v.role == AGENT_ROLE_USER) {
              // Substitute multimodal-wrapped prompt for the provider call, while keeping
              // the persisted session prompt clean.
              content = prompt_for_llm;
            }
            Json::Value mm(Json::nullValue);
            std::string text = content;
            if (try_parse_multimodal_prefix(content, &mm, &text) && mm.isObject()) {
              const bool allow_image_parts = !provider_rejects_image_parts(run_cfg.base_url, run_cfg.model);
              m["content"] = multimodal_content_from_parts(text, mm, allow_image_parts);
            } else {
              m["content"] = text;
            }
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
          bool verbose = false;
          int chunks = 0;
          uint64_t step = 0;
          uint64_t epoch = 0;
          decltype(push_ev)* push = nullptr;
          OpenAIStreamCoreAdapter core;
        } sctx;
        sctx.verbose = verbose;
        sctx.push = &push_ev;
        sctx.step = 0;
        sctx.epoch = (uint64_t)attempt;

        OpenAIStreamCoreConfig scfg{};
        scfg.max_tool_calls_total = 0;
        scfg.max_tool_call_args_chars = 0;
        scfg.max_events_per_feed = 0;
        scfg.delta_flush_bytes = 128;
        scfg.step = sctx.step;
        scfg.epoch = sctx.epoch;

        auto on_delta = [](void* vctx, const char* delta, size_t delta_len, uint64_t step, uint64_t epoch) {
          auto* s = static_cast<StreamCtx*>(vctx);
          if (!s || !s->push || !delta || delta_len == 0) return;
          Json::Value d(Json::objectValue);
          d["step"] = (Json::UInt64)step;
          d["epoch"] = (Json::UInt64)epoch;
          d["delta"] = std::string(delta, delta_len);
          (*s->push)("assistant_delta", d);
        };

        openai_stream_core_init(&sctx.core, &scfg, on_delta, &sctx);

        auto on_chunk = [](void* vctx, const char* chunk_json, size_t chunk_len) {
          auto* s = static_cast<StreamCtx*>(vctx);
          if (!s || !chunk_json || chunk_len == 0) return;
          s->chunks++;
          (void)openai_stream_core_feed_chunk(&s->core, chunk_json, chunk_len);
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
        if (sctx.core.has_usage) {
          Json::Value usage(Json::objectValue);
          usage["prompt_tokens"] = (Json::Int64)sctx.core.prompt_tokens;
          usage["completion_tokens"] = (Json::Int64)sctx.core.completion_tokens;
          usage["total_tokens"] = (Json::Int64)sctx.core.total_tokens;
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["usage"] = usage;
          push_ev("llm_usage", d);
        }

        // Flush any pending deltas.
        openai_stream_core_flush(&sctx.core);

        // Provider may have ignored streaming and returned a normal JSON completion.
        if (sr.http_status < 200 || sr.http_status >= 300) {
          ok = false;
          err = !sr.error_message.empty() ? sr.error_message : openai_format_http_error(sr.http_status, sr.response_body);
        } else {
          const std::string final_text = (!sctx.core.assistant.data || sctx.core.assistant.len == 0)
            ? json_try_extract_assistant_content_from_completion([&]() -> Json::Value {
                Json::Value v;
                std::string pe;
                if (!json_parse_any(sr.response_body, &v, &pe)) return Json::Value(Json::nullValue);
                return v;
              }())
            : std::string(sctx.core.assistant.data, sctx.core.assistant.len);
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
          openai_stream_core_free(&sctx.core);
          break;
        }

        if (openai_is_context_too_long_error(sr.http_status, sr.response_body)) {
          attempt_max_chars = std::max<size_t>(256, attempt_max_chars / 2);
          openai_stream_core_free(&sctx.core);
          continue;
        }
        openai_stream_core_free(&sctx.core);
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
        if (should_cancel_run()) {
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
        {
          Json::Value usage(Json::nullValue);
          if (llm_try_extract_usage_tokens_from_openai_response_body(pctx.last_body, &usage) && usage.isObject()) {
            Json::Value d(Json::objectValue);
            d["attempt"] = attempt;
            d["usage"] = usage;
            push_ev("llm_usage", d);
          }
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

    if (ok) {
      Json::Value d(Json::objectValue);
      Json::Value mm(Json::nullValue);
      std::string assistant_text_clean = assistant_text;
      const bool has_mm = try_parse_multimodal_prefix(assistant_text, &mm, &assistant_text_clean) && mm.isObject();
      d["assistant_content"] = assistant_text_clean;
      d["has_tool_calls"] = (Json::Int64)0;
      if (has_mm) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string mm_json = Json::writeString(wb, mm);
        const size_t mm_bytes = mm_json.size();
        if (max_capture_bytes > 0 && mm_bytes > max_capture_bytes) {
          d["assistant_mm_json"] = mm_json.substr(0, max_capture_bytes);
          d["assistant_mm_truncated"] = (Json::Int64)1;
        } else {
          d["assistant_mm_json"] = mm_json;
          d["assistant_mm_truncated"] = (Json::Int64)0;
        }
        d["assistant_mm_bytes"] = (Json::Int64)mm_bytes;
      }
      push_ev("assistant_message", d);
      assistant_text = assistant_text_clean;
    }
  }

  if (policy_active) {
    if (!policy_ctx.last_error.empty() && !ok) {
      err = policy_ctx.last_error;
    }
    policy_emit_complete(&policy_ctx, ok);
    if (!policy_ctx.events.empty()) {
      if (!events_out.isArray()) events_out = Json::Value(Json::arrayValue);
      for (const auto& ev : policy_ctx.events) {
        events_out.append(ev);
      }
      ensure_trace_id_in_events(&events_out, trace_id);
      inject_schema_into_events(&events_out);
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
        if (!trace_id.empty()) e["trace_id"] = trace_id;
        e["data"] = d;
        events_out.append(e);
        ensure_trace_id_in_events(&events_out, trace_id);
      }
      if (report.isObject() && report.isMember("ok") && report["ok"].isBool() && report["ok"].asBool() == false) {
        ok = false;
        err = report.isMember("error") && report["error"].isString() ? report["error"].asString() : "client acknowledgement verification failed";
      }
    }
  }

  Json::Value out(Json::objectValue);
  out["ok"] = ok;
  out["trace_id"] = trace_id;
  out["assistant_text"] = assistant_text;
  {
    const auto elapsed_ms =
      (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_steady).count();
    out["elapsed_ms"] = (Json::Int64)std::max<int64_t>(0, elapsed_ms);
  }
  if (!ok) {
    out["error"] = err;
    out["err"] = err;
    if (!err.empty()) out["code"] = error_code_from_message(err);
  }
  // Cancellation is a first-class terminal state for async jobs. Surface an explicit flag so
  // job state can be `status=cancelled` (instead of overloading `error`).
  if (!ok && events_out.isArray()) {
    bool cancelled = false;
    for (Json::ArrayIndex i = 0; i < events_out.size(); i++) {
      const auto& ev = events_out[i];
      if (!ev.isObject()) continue;
      if (ev.isMember("type") && ev["type"].isString() && ev["type"].asString() == "cancelled") {
        cancelled = true;
        break;
      }
    }
    if (cancelled) out["cancelled"] = true;
  }
  if (http_status) out["http_status"] = (Json::Int64)http_status;
  if (!http_body.empty()) out["http_body"] = http_body;
  if (trace_stream) out["trace_text"] = trace_buf.str();
  out["effective_yolo"] = yolo;
  const std::string effective_host_policy = host_policy_to_string(effective_policy);
  out["effective_host_policy"] = effective_host_policy;
  out["effective_automation_profile"] = effective_automation_profile;
  out["effective_timeout_ms"] = (Json::Int64)run_cfg.timeout_ms;
  out["effective_connect_timeout_ms"] = (Json::Int64)run_cfg.connect_timeout_ms;
  out["effective_stream_idle_timeout_ms"] = (Json::Int64)run_cfg.stream_idle_timeout_ms;
  out["effective_max_retries"] = (Json::Int64)run_cfg.max_retries;
  out["effective_retry_base_ms"] = (Json::Int64)run_cfg.retry_base_ms;
  out["effective_retry_max_ms"] = (Json::Int64)run_cfg.retry_max_ms;
  out["effective_retry_jitter"] = run_cfg.retry_jitter;
  out["effective_respect_retry_after"] = run_cfg.respect_retry_after;
  out["effective_stream_assistant"] = effective_stream_assistant;
  out["effective_require_client_acks"] = require_client_acks;
  out["effective_tools"] = tools;
  // Node identity surface (non-secret): expose effective provider target metadata so durable workflows
  // can require cross-node/provider quorum via aggregation (e.g. require_distinct_nodes).
  out["effective_model"] = run_cfg.model;
  out["effective_base_url"] = run_cfg.base_url;
  out["effective_max_steps"] = (Json::UInt64)max_steps;
  out["effective_max_tool_calls_total"] = (Json::UInt64)max_tool_calls_total;
  out["effective_max_tool_calls_per_tool"] = (Json::UInt64)max_tool_calls_per_tool;
  out["effective_max_tool_call_args_chars"] = (Json::UInt64)max_tool_call_args_chars;
  out["effective_max_tool_result_chars"] = (Json::UInt64)max_tool_result_chars;
  out["steps_executed"] = use_tool_loop ? (Json::UInt64)tool_loop_result.steps_executed : (Json::UInt64)0;
  out["tool_calls_total"] = use_tool_loop ? (Json::UInt64)tool_loop_result.tool_records.size() : (Json::UInt64)0;
  if (use_tool_loop && !tool_loop_result.tool_records.empty()) {
    Json::Value by_tool(Json::objectValue);
    for (const auto& tr : tool_loop_result.tool_records) {
      if (tr.tool_name.empty()) continue;
      const Json::Value cur = by_tool[tr.tool_name];
      const uint64_t n = cur.isUInt64() ? cur.asUInt64() : (cur.isInt64() ? (uint64_t)std::max<int64_t>(0, cur.asInt64()) : 0);
      by_tool[tr.tool_name] = (Json::UInt64)(n + 1);
    }
    out["tool_calls_by_tool"] = by_tool;
  }
  {
    // Token usage is provider-reported and best-effort.
    // We aggregate from structured `llm_usage` events, which are emitted by:
    // - tool-loop providers (`openai_tool_provider`), when response JSON has a `usage` object
    // - tools=none run path, after parsing the raw response body
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    int64_t total_tokens = 0;
    llm_sum_usage_from_events(events_out, &prompt_tokens, &completion_tokens, &total_tokens);
    out["prompt_tokens"] = (Json::Int64)std::max<int64_t>(0, prompt_tokens);
    out["completion_tokens"] = (Json::Int64)std::max<int64_t>(0, completion_tokens);
    out["total_tokens"] = (Json::Int64)std::max<int64_t>(0, total_tokens);
  }
  {
    Json::Value mp(Json::objectValue);
    mp["include_structured"] = mem_pol.include_structured;
    mp["include_core"] = mem_pol.include_core;
    mp["include_daily"] = mem_pol.include_daily;
    mp["include_session"] = mem_pol.include_session;
    mp["daily_days"] = (Json::Int64)mem_pol.daily_days;
    mp["total_cap"] = (Json::UInt64)mem_pol.total_cap;
    out["effective_memory_policy"] = mp;
  }
  if (!memory_scope_id.empty()) {
    out["memory_scope_id"] = memory_scope_id;
    out["memory_scope_mode"] = memory_scope_mode;
  }
  out["effective_input_image_count"] = (Json::UInt64)input_image_count;
  out["effective_had_input_files"] = input_had_any_files;
  {
    std::string mm_mode = "none";
    if (input_image_count > 0) {
      const bool provider_rejects = provider_rejects_image_parts(run_cfg.base_url, run_cfg.model);
      if (!use_tool_loop) {
        mm_mode = provider_rejects ? "image_omitted" : "direct";
      } else {
        if (vision_prefetch_attempted) {
          mm_mode = vision_prefetch_ok ? "prefetch_ok" : "prefetch_failed";
        } else {
          mm_mode = provider_rejects ? "image_omitted" : "direct";
        }
      }
    }
    out["effective_multimodal"] = mm_mode;
  }
  out["verbose"] = verbose;
  out["events"] = events_out;

  // Canonical persistence: sessions + audit + telemetry in SQLite.
  //
  // Respect `no_session`: "ephemeral" runs should not persist anything to disk.
  if (!no_session && db_or_null && db_or_null->is_open()) {
    RunRequestPersistInput persist;
    persist.db = db_or_null;
    persist.session = session;
    persist.session_id = &session_id;
    persist.run_ts_ms = run_ts_ms;
    persist.args = &args;
    persist.response_json = &out;
    persist.use_tool_loop = use_tool_loop;
    persist.tool_loop_result = use_tool_loop ? &tool_loop_result : nullptr;
    persist.trace_id = &trace_id;
    persist.prompt = &prompt;
    persist.tools = &tools;
    persist.run_cfg = &run_cfg;
    persist.stream_assistant = stream_assistant;
    persist.ok = ok;
    persist.err = &err;
    persist.http_status = http_status;
    persist.http_body = &http_body;
    persist.job_id = &job_id_local;
    persist.yolo = yolo;
    persist.host_policy = &effective_host_policy;
    persist.effective_automation_profile = &effective_automation_profile;
    persist.assistant_text = &assistant_text;
    persist.events_out = &events_out;
    persist_run_request(persist);
  }

  agent_session_destroy(session);
  return out;
}

}  // namespace

Json::Value run_request_to_json_internal(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const std::string& request_body,
  const char* job_id_or_null
) {
  return run_request_to_json_impl(
    daemon_cfg,
    ocfg,
    db_or_null,
    tool_ext_or_null,
    sessions_root_dir,
    request_body,
    job_id_or_null,
    nullptr,
    nullptr
  );
}

Json::Value run_request_to_json_internal_cancellable(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const std::string& request_body,
  const char* job_id_or_null,
  RunCancelCallback should_cancel,
  void* should_cancel_ctx
) {
  return run_request_to_json_impl(
    daemon_cfg,
    ocfg,
    db_or_null,
    tool_ext_or_null,
    sessions_root_dir,
    request_body,
    job_id_or_null,
    should_cancel,
    should_cancel_ctx
  );
}

}  // namespace agentd
