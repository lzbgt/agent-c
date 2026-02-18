#include "run_endpoints.h"

#include "daemon_auth.h"
#include "client_profiles.h"
#include "default_system_prompt.h"
#include "file_persistor.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"
#include "llm_usage.h"
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
#include "trace_id_util.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#include "base64.h"
#include "openai_client.h"
#include "openai_stream_adapter.h"

#include "agent/agent.h"
#include "agent/runner.h"
#include "agent/json_c14n.h"

#include "run_client_acks.h"
#include "run_memory_context.h"
#include "run_multimodal.h"
#include "run_endpoints_internal.h"

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

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
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

  std::string trace_id;
  if (args.isMember("trace_id") && args["trace_id"].isString()) trace_id = trim_copy(args["trace_id"].asString());
  if (!trace_id.empty() && !trace_id_is_safe(trace_id)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid trace_id";
    return o;
  }
  if (trace_id.empty()) trace_id = make_uuidish_trace_id();

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
  if (args.isMember("connect_timeout_ms") && args["connect_timeout_ms"].isInt64()) {
    const long t = (long)args["connect_timeout_ms"].asInt64();
    if (t >= 0) run_cfg.connect_timeout_ms = t;
  }
  if (args.isMember("stream_idle_timeout_ms") && args["stream_idle_timeout_ms"].isInt64()) {
    const long t = (long)args["stream_idle_timeout_ms"].asInt64();
    if (t >= 0) run_cfg.stream_idle_timeout_ms = t;
  }
  if (args.isMember("max_retries") && args["max_retries"].isInt()) {
    const int r = args["max_retries"].asInt();
    run_cfg.max_retries = std::max(0, std::min(r, 8));
  }
  if (args.isMember("retry_base_ms") && args["retry_base_ms"].isInt64()) {
    const long t = (long)args["retry_base_ms"].asInt64();
    if (t >= 0) run_cfg.retry_base_ms = std::min<long>(t, 60000L);
  }
  if (args.isMember("retry_max_ms") && args["retry_max_ms"].isInt64()) {
    const long t = (long)args["retry_max_ms"].asInt64();
    if (t >= 0) run_cfg.retry_max_ms = std::min<long>(t, 60000L);
  }
  if (args.isMember("retry_jitter") && (args["retry_jitter"].isDouble() || args["retry_jitter"].isInt() || args["retry_jitter"].isInt64())) {
    const double j = args["retry_jitter"].asDouble();
    run_cfg.retry_jitter = std::max(0.0, std::min(j, 1.0));
  }
  if (args.isMember("respect_retry_after") && args["respect_retry_after"].isBool()) {
    run_cfg.respect_retry_after = args["respect_retry_after"].asBool();
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
      } else if (run_provider == "moonshot") {
        // Moonshot/Kimi: commonly stored as KIMI_API_KEY_CN in ~/.env.
        if (const char* k = getenv_s("KIMI_API_KEY_CN")) key = k;
        else if (const char* k2 = getenv_s("MOONSHOT_API_KEY")) key = k2;
        else if (const char* k3 = getenv_s("MOONSHOT_API_KEY_CN")) key = k3;
        else if (const char* k4 = getenv_s("OPENAI_API_KEY")) key = k4;
        else if (const char* k5 = getenv_s("OPENROUTER_API_KEY")) key = k5;
        else if (const char* k6 = getenv_s("DEEPSEEK_API_KEY")) key = k6;
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
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 400;
      o["error"] = "input_files requires session persistence (no_session=false)";
      return o;
    }
    if (sessions_root_dir.empty()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "sessions_root_dir not configured";
      return o;
    }

    const std::filesystem::path sr = session_root_path(sessions_root_dir, session_id);
    if (sr.empty()) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 400;
      o["error"] = "invalid session_id";
      return o;
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
  uint64_t max_tool_call_args_chars_u64 = 0;
  const size_t max_tool_call_args_chars =
    json_get_u64_nonneg(args, "max_tool_call_args_chars", &max_tool_call_args_chars_u64)
      ? (size_t)max_tool_call_args_chars_u64
      : daemon_cfg.max_tool_call_args_chars_default;

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

  std::string job_id_local = (job_id_or_null && job_id_or_null[0]) ? std::string(job_id_or_null) : std::string();
  const int64_t run_ts_ms = now_unix_ms();
  if (!job_id_local.empty()) {
    job_set_trace_id(job_id_local, trace_id);
  }

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
        agent_session_add_message(session, AGENT_ROLE_SYSTEM, host_system_prompt_for_profile(system_profile.c_str()));
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
    const bool changed = ensure_pinned_host_system_prompts(
      &session, tools, no_default_system, system_profile, client_kind, /*allow_default_host_prompt=*/true);
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
  auto inject_trace_id_into_events = [&](Json::Value* arr) {
    if (!arr || !arr->isArray() || trace_id.empty()) return;
    for (Json::ArrayIndex i = 0; i < arr->size(); i++) {
      Json::Value& ev = (*arr)[i];
      if (!ev.isObject()) continue;
      if (!ev.isMember("trace_id")) ev["trace_id"] = trace_id;
    }
  };

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
    // Optional vision prefetch:
    // For Moonshot/Kimi, multimodal vision works in tools=none schema, but tool-loop requests can't include image parts.
    // To keep host tools available while still letting the model "see" the image, do a one-shot tools=none call to
    // produce a textual description, then inject it into the tool-loop prompt.
    Json::Value pre_events(Json::arrayValue);
    std::string prompt_for_tool_loop = prompt_for_llm;
    {
      const bool want_prefetch =
        !(args.isMember("vision_prefetch") && args["vision_prefetch"].isBool() && args["vision_prefetch"].asBool() == false);

      Json::Value mm(Json::nullValue);
      std::string user_text = prompt_for_llm;
      const bool has_mm = try_parse_multimodal_prefix(prompt_for_llm, &mm, &user_text) && mm.isObject();
      const bool has_images = has_mm && mm.isMember("images") && mm["images"].isArray() && !mm["images"].empty();
      const bool should_prefetch = want_prefetch && has_images && provider_requires_tools_none_for_vision(run_cfg.base_url, run_cfg.model);

      if (should_prefetch) {
        vision_prefetch_attempted = true;
        // Build a single-message vision request (tools=none) using OpenAI-compatible multimodal parts.
        // We do not persist this "internal" call into the session transcript.
        std::string vision_desc;
        std::string v_err;
        long v_http = 0;
        try {
          const std::string pre_text =
            std::string("Describe the attached image(s) in detail so I can answer the user's request.\n")
            + "User request:\n"
            + prompt;

          Json::Value root(Json::objectValue);
          root["model"] = run_cfg.model;
          root["stream"] = false;
          Json::Value messages(Json::arrayValue);

          {
            Json::Value sm(Json::objectValue);
            sm["role"] = "system";
            sm["content"] = "You are a vision captioning assistant. Output plain text.";
            messages.append(sm);
          }
          {
            Json::Value um(Json::objectValue);
            um["role"] = "user";
            um["content"] = multimodal_content_from_parts(pre_text, mm, /*allow_image_parts=*/true);
            messages.append(um);
          }

          root["messages"] = messages;
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          const std::string req_json = Json::writeString(wb, root);

          OpenAIRawResult raw = openai_chat_completions_raw(run_cfg, req_json);
          v_http = raw.http_status;
          if (raw.http_status < 200 || raw.http_status >= 300) {
            v_err = openai_format_http_error(raw.http_status, raw.response_body);
          } else {
            vision_desc = try_extract_assistant_text_from_response_json(raw.response_body);
            if (vision_desc.empty()) {
              v_err = "vision prefetch returned empty assistant text";
            }
          }
        } catch (const std::exception& e) {
          v_err = std::string("vision prefetch threw exception: ") + e.what();
        } catch (...) {
          v_err = "vision prefetch threw unknown exception";
        }

        Json::Value ev(Json::objectValue);
        ev["type"] = "vision_prefetch";
        if (!trace_id.empty()) ev["trace_id"] = trace_id;
        Json::Value d(Json::objectValue);
        d["ok"] = (bool)v_err.empty();
        d["provider"] = provider_from_base_url(run_cfg.base_url);
        d["model"] = run_cfg.model;
        if (v_http) d["http_status"] = (Json::Int64)v_http;
        if (!v_err.empty()) d["error"] = v_err;
        if (!vision_desc.empty()) {
          d["chars"] = (Json::UInt64)vision_desc.size();
          // Include a short preview for debugging/UI display (avoid large blobs).
          const size_t kPreview = 512;
          d["preview"] = vision_desc.size() <= kPreview ? vision_desc : (vision_desc.substr(0, kPreview) + "…");
        }
        ev["data"] = d;
        pre_events.append(ev);

        if (v_err.empty() && !vision_desc.empty()) {
          vision_prefetch_ok = true;
          // Strip images from the multimodal envelope before entering the tool loop
          // (otherwise the tool-loop provider may add an "image omitted" hint).
          Json::Value mm2 = mm;
          if (mm2.isObject() && mm2.isMember("images")) {
            mm2.removeMember("images");
          }
          // Re-wrap without images and append the vision description into the text prompt.
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          prompt_for_tool_loop = std::string(kMultimodalPrefix) + Json::writeString(wb, mm2) + "\n" + user_text;
          prompt_for_tool_loop += "\n\n[Image description]\n";
          prompt_for_tool_loop += vision_desc;
        }
      }
    }

    ToolLoopOptions opt;
    opt.max_steps = max_steps;
    opt.max_tool_calls_total = max_tool_calls_total;
    opt.max_tool_calls_per_tool = max_tool_calls_per_tool;
    opt.max_tool_call_args_chars = max_tool_call_args_chars;
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
	    }
    if (should_cancel_or_null) {
      opt.should_cancel = should_cancel_or_null;
      opt.should_cancel_ctx = should_cancel_ctx_or_null;
    } else if (!job_id_local.empty()) {
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
		      std::string effective_query = mem_query;
		      if (effective_query.empty() && mem_pol.mode == MemoryContextMode::Search) {
		        effective_query = prompt_for_llm;
		      }
		      if (build_memory_context_text(daemon_cfg.state_dir, session_id, mem_pol, effective_query, &mem_ctx)) {
		        if (agent_session_t* tmp = clone_session_with_memory_context(session, mem_ctx)) {
		          ephemeral_seed.reset(tmp);
		          seed_for_run = tmp;
		        }
		      }
		    }

	    try {
	      ok = run_tool_loop(
	        run_cfg, seed_for_run, prompt_for_tool_loop, registry, &executor, opt, trace_stream, &tool_loop_result, &err, &http_status, &http_body
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
        inject_trace_id_into_events(&events_out);
      }
    }
    if (!pre_events.empty()) {
      if (!events_out.isArray()) events_out = Json::Value(Json::arrayValue);
      for (const auto& pe : pre_events) {
        events_out.append(pe);
      }
      inject_trace_id_into_events(&events_out);
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
        inject_trace_id_into_events(&events_out);
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
  if (!ok) out["error"] = err;
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
  out["effective_host_policy"] = host_policy_to_string(effective_policy);
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
    // Mirror session messages (as of the end of this run).
    std::vector<std::pair<std::string, std::string>> msgs;
    msgs.reserve(agent_session_message_count(session));
    for (size_t i = 0; i < agent_session_message_count(session); i++) {
      agent_message_view_t v{};
      if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
      msgs.emplace_back(agent_role_to_string(v.role), std::string(v.content, v.content_len));
    }
    (void)db_or_null->replace_session_messages(session_id, msgs, run_ts_ms, nullptr);

    std::string replay_request_json;
    std::string replay_response_json;
    std::string replay_sha256;
    std::string replay_sha256_alg;
    std::string replay_sha256_schema;
    std::string replay_error;
    auto append_replay_error = [&](const std::string& e) {
      if (e.empty()) return;
      if (!replay_error.empty()) replay_error.append(";");
      replay_error.append(e);
    };

    Json::Value replay_req = args;
    if (!trace_id.empty()) replay_req["trace_id"] = trace_id;
    redact_replay_request(&replay_req);
    {
      std::string err_code;
      if (!json_stringify_capped(replay_req, kReplayRequestMaxBytes, &replay_request_json, &err_code, "request_json_too_large")) {
        append_replay_error(err_code.empty() ? "request_json_unavailable" : err_code);
      }
    }

    Json::Value replay_resp = out;
    redact_replay_response(&replay_resp);
    {
      std::string err_code;
      if (!json_stringify_capped(replay_resp, kReplayResponseMaxBytes, &replay_response_json, &err_code, "response_json_too_large")) {
        append_replay_error(err_code.empty() ? "response_json_unavailable" : err_code);
      }
    }

    Json::Value replay_tools(Json::arrayValue);
    if (use_tool_loop && !tool_loop_result.tool_records.empty()) {
      for (const auto& tr : tool_loop_result.tool_records) {
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

    if (!replay_request_json.empty() && !replay_response_json.empty()) {
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
        replay_sha256 = std::string(token);
        replay_sha256_alg = "agent_json_c14n_v1";
        replay_sha256_schema = "run_replay_bundle_v1";
      } else {
        append_replay_error(err_buf[0] ? std::string(err_buf) : "replay_hash_failed");
      }
    }

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
    rr.request_json = replay_request_json;
    rr.response_json = replay_response_json;
    rr.replay_sha256 = replay_sha256;
    rr.replay_sha256_alg = replay_sha256_alg;
    rr.replay_sha256_schema = replay_sha256_schema;
    rr.replay_error = replay_error;
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
          Json::Value d2 = d;
          if (!trace_id.empty() && !d2.isMember("trace_id")) d2["trace_id"] = trace_id;
          (void)db_or_null->insert_event(run_id, run_ts_ms, t.asString(), Json::writeString(wb, d2), nullptr);

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
                int64_t artifact_id = 0;
                (void)db_or_null->insert_artifact(ar, &artifact_id, nullptr);
                if (artifact_id > 0 && art.isMember("blob_id") && art["blob_id"].isString()) {
                  const std::string blob_id = art["blob_id"].asString();
                  if (!blob_id.empty()) {
                    (void)db_or_null->attach_blob_to_artifact(artifact_id, blob_id, nullptr);
                  }
                }
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
      record["trace_id"] = trace_id;
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
