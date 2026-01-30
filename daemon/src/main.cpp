#include "http_server.h"

#include "agent/agent.h"
#include "agent/provider.h"
#include "agent/runner.h"

#include "http_util.h"
#include "string_util.h"

#include "default_system_prompt.h"
#include "file_persistor.h"
#include "openai_client.h"
#include "openai_provider.h"
#include "session_store.h"
#include "summary_compaction.h"
#include "summary_llm.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#include "json_util.h"
#include "job_manager.h"
#include "openrouter_util.h"
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include <cstring>
#include <chrono>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <functional>
#include <unistd.h>
#include <cerrno>
#include <signal.h>

using namespace agentd;

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
  return std::filesystem::current_path().string();
}

struct DaemonConfig {
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 8123;
  // Optional daemon auth (control-plane). When set, all endpoints (except /health) require
  // Authorization: Bearer <token>. This is distinct from provider API keys.
  std::string auth_token;
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string model = "gpt-4o-mini";
  std::string summary_model;  // optional: model used to summarize dropped messages during compaction (tools=none)
  size_t summary_max_chars = 1200;
  std::string proxy_url; // optional explicit proxy override (else env)
  long timeout_ms = 60000;
  std::string tools = "host";     // none|basic|host
  std::string tools_root = "";    // empty => CWD (unrestricted file edits)
  std::string host_scope_root;    // default: daemon process CWD (for "@host" tool root mode)
  bool yolo_default = true;       // default to unrestricted unless client requests scoped mode
  bool no_default_system = false; // when false, host tool runs insert a default system hint (one time)
  size_t max_chars_default = 20000;
  size_t keep_last_default = 16;

  // Async job GC (daemon longevity): finished jobs are kept only for a bounded time/count.
  int64_t job_ttl_ms = 30 * 60 * 1000; // 30 minutes
  size_t max_jobs = 256;
};

static bool daemon_auth_ok(const DaemonConfig& cfg, const HttpRequest& req) {
  if (cfg.auth_token.empty()) {
    return true;
  }
  const std::string auth = header_get_ci(req.headers, "authorization");
  const std::string got = bearer_token_from_auth_header(auth);
  return !got.empty() && got == cfg.auth_token;
}

static bool daemon_require_auth(const DaemonConfig& cfg, const HttpRequest& req, HttpResponse* resp) {
  if (daemon_auth_ok(cfg, req)) {
    return true;
  }
  if (resp) {
    resp->status = 401;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->headers["WWW-Authenticate"] = "Bearer";
    resp->body = R"({"ok":false,"error":"unauthorized"})";
  }
  return false;
}

// Parses the daemon run request body and returns a response JSON object (HTTP-level errors are represented in JSON).
static Json::Value run_request_to_json(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
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
  if (args.isMember("model") && args["model"].isString()) run_cfg.model = args["model"].asString();
  if (args.isMember("base_url") && args["base_url"].isString()) run_cfg.base_url = args["base_url"].asString();
  if (args.isMember("api_key") && args["api_key"].isString()) run_cfg.api_key = args["api_key"].asString();
  if (args.isMember("proxy") && args["proxy"].isString()) run_cfg.proxy_url = args["proxy"].asString();
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const long t = (long)args["timeout_ms"].asInt64();
    if (t > 0) run_cfg.timeout_ms = t;
  }

  const std::string tools = args.isMember("tools") && args["tools"].isString() ? args["tools"].asString() : daemon_cfg.tools;
  const bool yolo = args.isMember("yolo") && args["yolo"].isBool() ? args["yolo"].asBool() : daemon_cfg.yolo_default;
  const bool no_default_system =
    args.isMember("no_default_system") && args["no_default_system"].isBool() ? args["no_default_system"].asBool() : daemon_cfg.no_default_system;
  const std::string system_msg = args.isMember("system") && args["system"].isString() ? args["system"].asString() : "";
  std::string tools_root = args.isMember("tools_root") && args["tools_root"].isString() ? args["tools_root"].asString() : daemon_cfg.tools_root;
  if (tools_root == "@host") {
    tools_root = daemon_cfg.host_scope_root;
  } else if (tools_root == "@cwd") {
    tools_root = "";
  }
  if (yolo) {
    tools_root.clear();
  }
  uint64_t max_steps_u64 = 0;
  const size_t max_steps = json_get_u64_nonneg(args, "max_steps", &max_steps_u64) ? (size_t)max_steps_u64 : 0;
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

  const std::string session_id = args.isMember("session_id") && args["session_id"].isString() ? args["session_id"].asString() : "default";
  const bool no_session = args.isMember("no_session") && args["no_session"].isBool() ? args["no_session"].asBool() : false;
  std::string job_id_local = (job_id_or_null && job_id_or_null[0]) ? std::string(job_id_or_null) : std::string();

  agent_session_t* session = nullptr;
  SessionStoreConfig store_cfg;
  store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();

  agent_persistor_t persistor{};
  struct PersistorGuard {
    agent_persistor_t* p;
    ~PersistorGuard() { agent_persistor_destroy(p); }
  } pers_guard{&persistor};

  {
    const agent_status_t st = agent_file_persistor_create(store_cfg.root_dir.c_str(), &persistor);
    if (st != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to init persistor";
      o["status"] = (Json::Int64)st;
      return o;
    }
  }

  if (!no_session) {
    const agent_status_t st = persistor.load(persistor.ctx, session_id.c_str(), &session);
    if (st != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to load session";
      o["status"] = (Json::Int64)st;
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
    } else if (!no_default_system && tools == "host") {
      agent_session_add_message(session, AGENT_ROLE_SYSTEM, default_host_system_prompt());
    }
  }

  agent_tool_registry_t* registry = nullptr;
  agent_tool_executor_t executor{};
  bool need_destroy_executor = false;
  const bool use_tool_loop = (tools != "none");

  if (tools == "basic") {
    if (toolset_basic_create(&registry, &executor) != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to init toolset_basic";
      agent_session_destroy(session);
      return o;
    }
  } else if (tools == "host") {
    HostToolsetConfig hcfg;
    hcfg.root_dir = tools_root;
    if (!job_id_local.empty()) {
      // Cooperative cancellation for long-running host tools (sleep/build/etc).
      hcfg.should_cancel = [](void* vctx) -> bool {
        if (!vctx) return false;
        const auto* jid = static_cast<const std::string*>(vctx);
        return jid && job_is_cancel_requested(*jid);
      };
      hcfg.should_cancel_ctx = (void*)&job_id_local;
    }
    if (toolset_host_create(hcfg, &registry, &executor) != AGENT_OK) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "failed to init toolset_host";
      agent_session_destroy(session);
      return o;
    }
    need_destroy_executor = true;
  } else if (tools != "none") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid tools (expected: none|basic|host)";
    agent_session_destroy(session);
    return o;
  }

  bool ok = false;
  std::string assistant_text;
  std::string err;
  long http_status = 0;
  std::string http_body;
  std::ostringstream trace_buf;
  std::ostream* trace_stream = trace ? &trace_buf : nullptr;
  Json::Value events_out;

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
    opt.verbose = verbose;
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

    ToolLoopResult r;
    try {
      ok = run_tool_loop(run_cfg, session, prompt, registry, &executor, opt, trace_stream, &r, &err, &http_status, &http_body);
    } catch (const std::exception& e) {
      ok = false;
      err = std::string("tool loop threw exception: ") + e.what();
    } catch (...) {
      ok = false;
      err = "tool loop threw unknown exception";
    }
    assistant_text = r.final_assistant_text;
    if (!r.events_json.empty()) {
      Json::CharReaderBuilder rb;
      std::string errs;
      std::istringstream iss(r.events_json);
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
          decltype(push_ev)* push = nullptr;
        } sctx;
        sctx.verbose = verbose;
        sctx.push = &push_ev;

        auto on_chunk = [](void* vctx, const char* chunk_json, size_t chunk_len) {
          auto* s = static_cast<StreamCtx*>(vctx);
          if (!s || !chunk_json || chunk_len == 0 || !s->push) return;

          s->chunks++;
          Json::Value root;
          std::string perr;
          if (!json_parse_any(std::string(chunk_json, chunk_len), &root, &perr)) {
            return;
          }
          const auto& choices = root["choices"];
          if (!choices.isArray() || choices.empty()) return;
          const auto& delta = choices[0]["delta"];
          if (!delta.isObject()) return;
          const auto& content = delta["content"];
          if (!content.isString()) return;
          const std::string dstr = content.asString();
          if (dstr.empty()) return;
          s->assistant += dstr;
          s->pending_delta += dstr;

          // Coalesce small deltas to avoid flooding the daemon/UI with thousands of events.
          if (s->pending_delta.size() >= 128) {
            Json::Value d(Json::objectValue);
            d["delta"] = s->pending_delta;
            d["total_len"] = (Json::UInt64)s->assistant.size();
            if (s->verbose) {
              const size_t n = s->assistant.size();
              const size_t start = (n > 200) ? (n - 200) : 0;
              d["assistant_tail"] = s->assistant.substr(start);
            }
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
          d["http_status"] = (Json::Int64)http_status;
          d["stream"] = true;
          d["chunks"] = (Json::Int64)sctx.chunks;
          if (verbose) {
            bool trunc = false;
            d["response_body_capture"] = truncate_for_event(http_body, 64 * 1024, &trunc);
            d["response_truncated"] = trunc;
          }
          push_ev("llm_response", d);
        }

        if (http_status < 200 || http_status >= 300) {
          ok = false;
          err = sr.error_message.empty() ? openai_format_http_error(http_status, http_body) : sr.error_message;

          if (attempt < 2 && openai_is_context_too_long_error(http_status, http_body)) {
            const size_t next = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
            Json::Value d(Json::objectValue);
            d["attempt"] = attempt;
            d["http_status"] = (Json::Int64)http_status;
            d["reason"] = "context_too_long_retry";
            d["max_chars_before"] = (Json::UInt64)attempt_max_chars;
            d["max_chars_after"] = (Json::UInt64)next;
            push_ev("retry", d);
            attempt_max_chars = next;
            continue;
          }

          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["http_status"] = (Json::Int64)http_status;
          d["error"] = err;
          push_ev("error", d);
          break;
        }

        assistant_text = sctx.assistant;
        if (assistant_text.empty() && !http_body.empty() && http_body.size() > 0 && http_body[0] == '{') {
          // Provider may have ignored streaming and returned a normal JSON completion.
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(http_body, &parsed, &perr)) {
            assistant_text = json_try_extract_assistant_content_from_completion(parsed);
          }
        }
        ok = !assistant_text.empty();
        if (!ok) {
          err = "streamed completion returned no assistant content";
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["error"] = err;
          d["http_status"] = (Json::Int64)http_status;
          push_ev("error", d);
          break;
        }

        if (!sctx.pending_delta.empty()) {
          Json::Value d(Json::objectValue);
          d["delta"] = sctx.pending_delta;
          d["total_len"] = (Json::UInt64)sctx.assistant.size();
          if (verbose) {
            const size_t n = sctx.assistant.size();
            const size_t start = (n > 200) ? (n - 200) : 0;
            d["assistant_tail"] = sctx.assistant.substr(start);
          }
          push_ev("assistant_delta", d);
          sctx.pending_delta.clear();
        }

        agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
        break;
      }
    } else {
      OpenAIProviderCtx pctx;
      pctx.cfg = run_cfg;
      const agent_provider_t provider = openai_make_provider(&pctx);

      agent_run_options_t run_opt{};
      run_opt.model = run_cfg.model.c_str();
      run_opt.keep_last_messages = keep_last;

      size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
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
            const size_t max_out = summary_max_chars == 0 ? 1200 : summary_max_chars;
            CompactionSummaryResult sr = generate_compaction_summary_via_llm(run_cfg, summary_model, input, max_out);

            Json::Value d(Json::objectValue);
            d["attempt"] = attempt;
            d["summary_model"] = summary_model;
            d["dropped_messages"] = (Json::UInt64)input.dropped_messages;
            d["excerpt_truncated"] = input.truncated;
            d["ok"] = sr.ok;
            d["http_status"] = (Json::Int64)sr.http_status;
            if (!sr.ok && !sr.error.empty()) {
              d["error"] = sr.error;
            }
            if (verbose && sr.ok) {
              bool trunc = false;
              d["summary_text"] = truncate_for_event(sr.summary_text, 2048, &trunc);
              d["summary_text_truncated"] = trunc;
            }
            push_ev("summary", d);

            if (sr.ok && !sr.summary_text.empty()) {
              summary_buf = std::string(AGENT_SESSION_SUMMARY_PREFIX) + "\n" + sr.summary_text;
              run_opt.summary_or_null = summary_buf.c_str();
            }
          }
        }

        agent_run_report_t rep{};
        const agent_status_t st = agent_run_once(session, &provider, &run_opt, &rep);
        ok = (st == AGENT_OK);
        assistant_text = ok ? std::string(rep.assistant_view.content, rep.assistant_view.content_len) : "";
        if (!ok) {
          err = pctx.last_error.empty() ? "agent_run_once failed" : pctx.last_error;
          http_status = pctx.last_http_status;
          http_body = pctx.last_body;
        }
        {
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["max_chars"] = (Json::UInt64)attempt_max_chars;
          d["before_chars"] = (Json::UInt64)rep.compact.before_chars;
          d["after_chars"] = (Json::UInt64)rep.compact.after_chars;
          d["dropped_messages"] = (Json::UInt64)rep.compact.dropped_messages;
          d["inserted_summary"] = (bool)rep.compact.inserted_summary;
          push_ev("compaction", d);
        }
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
        if (trace_stream) {
          *trace_stream << "=== REQUEST (attempt=" << attempt << ") ===\n";
          *trace_stream << (pctx.last_request_body.empty() ? "(request body unavailable)\n" : (pctx.last_request_body + "\n"));
          *trace_stream << "=== RESPONSE ===\n";
          *trace_stream << (pctx.last_body.empty() ? "" : (pctx.last_body + "\n"));
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
          push_ev("llm_response", d);
        }

        if (ok) {
          break;
        }

        if (attempt < 2 && st == AGENT_ERR_CONTEXT_TOO_LONG) {
          const size_t next = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
          Json::Value d(Json::objectValue);
          d["attempt"] = attempt;
          d["http_status"] = (Json::Int64)pctx.last_http_status;
          d["reason"] = "context_too_long_retry";
          d["max_chars_before"] = (Json::UInt64)attempt_max_chars;
          d["max_chars_after"] = (Json::UInt64)next;
          push_ev("retry", d);
          attempt_max_chars = next;
          continue;
        }
        break;
      }
    }

    // Final assistant message (if any).
    {
      Json::Value d(Json::objectValue);
      d["assistant_content"] = assistant_text;
      d["has_tool_calls"] = false;
      push_ev("assistant_message", d);
    }
    {
      Json::Value d(Json::objectValue);
      d["truncated"] = false;
      push_ev("end", d);
    }
  }

  heartbeat_stop.store(true);
  if (heartbeat_thread.joinable()) {
    heartbeat_thread.join();
  }

  if (ok && !no_session) {
    (void)persistor.save(persistor.ctx, session_id.c_str(), session);
  }

  if (registry) {
    agent_tool_registry_destroy(registry);
  }
  if (need_destroy_executor) {
    toolset_host_destroy(&executor);
  }
  agent_session_destroy(session);

  Json::Value out(Json::objectValue);
  out["ok"] = ok;
  out["assistant_text"] = assistant_text;
  if (!ok && !err.empty()) out["error"] = err;
  out["http_status"] = (Json::Int64)http_status;
  out["http_body"] = http_body;
  out["trace_text"] = trace_buf.str();
  out["effective_tools_root"] = tools_root;
  out["effective_yolo"] = yolo;
  out["effective_timeout_ms"] = (Json::Int64)run_cfg.timeout_ms;
  out["effective_stream_assistant"] = stream_assistant;
  out["verbose"] = verbose;
  if (events_out.isArray()) {
    out["events"] = events_out;
  }

  if (!no_session && !session_id.empty()) {
    Json::Value record(Json::objectValue);
    record["ts_unix_ms"] = (Json::Int64)now_unix_ms();
    record["session_id"] = session_id;
    record["ok"] = ok;
    record["model"] = run_cfg.model;
    record["base_url"] = run_cfg.base_url;
    record["tools"] = tools;
    record["yolo"] = yolo;
    record["tools_root"] = tools_root;
    record["prompt"] = prompt;
    record["assistant_text"] = assistant_text;
    record["http_status"] = (Json::Int64)http_status;
    record["error"] = err;
    if (events_out.isArray()) {
      record["events"] = events_out;
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    (void)session_store_append_audit_jsonl(store_cfg, session_id, Json::writeString(wb, record));
  }

  return out;
}

int main(int argc, char** argv) {
  // On macOS (and many POSIX systems), writing to a closed socket can raise SIGPIPE,
  // which terminates the process by default. Our HTTP server uses plain ::write(),
  // so we must ignore SIGPIPE to avoid daemon exits that look like "hangs" to clients.
  (void)::signal(SIGPIPE, SIG_IGN);

  DaemonConfig cfg;
  cfg.host_scope_root = std::filesystem::current_path().string();
  // Minimal flag parsing (daemon is host-only; core remains argv/env-free).
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    auto take = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (a == "--host") {
      if (!take(&cfg.listen_host)) {
        std::cerr << "Missing value for --host\n";
        return 2;
      }
    } else if (a == "--auth-token") {
      if (!take(&cfg.auth_token)) {
        std::cerr << "Missing value for --auth-token\n";
        return 2;
      }
    } else if (a == "--port") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --port\n";
        return 2;
      }
      try {
        const unsigned long p = std::stoul(v);
        cfg.listen_port = (uint16_t)p;
      } catch (...) {
        std::cerr << "Invalid --port\n";
        return 2;
      }
    } else if (a == "--model") {
      if (!take(&cfg.model)) {
        std::cerr << "Missing value for --model\n";
        return 2;
      }
    } else if (a == "--summary-model") {
      if (!take(&cfg.summary_model)) {
        std::cerr << "Missing value for --summary-model\n";
        return 2;
      }
    } else if (a == "--summary-max-chars") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --summary-max-chars\n";
        return 2;
      }
      try {
        cfg.summary_max_chars = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --summary-max-chars\n";
        return 2;
      }
    } else if (a == "--base-url") {
      if (!take(&cfg.base_url)) {
        std::cerr << "Missing value for --base-url\n";
        return 2;
      }
    } else if (a == "--api-key") {
      if (!take(&cfg.api_key)) {
        std::cerr << "Missing value for --api-key\n";
        return 2;
      }
    } else if (a == "--proxy") {
      if (!take(&cfg.proxy_url)) {
        std::cerr << "Missing value for --proxy\n";
        return 2;
      }
    } else if (a == "--timeout-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --timeout-ms\n";
        return 2;
      }
      try {
        cfg.timeout_ms = (long)std::stoll(v);
      } catch (...) {
        std::cerr << "Invalid --timeout-ms\n";
        return 2;
      }
    } else if (a == "--job-ttl-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --job-ttl-ms\n";
        return 2;
      }
      try {
        cfg.job_ttl_ms = (int64_t)std::stoll(v);
        if (cfg.job_ttl_ms < 0) cfg.job_ttl_ms = 0;
      } catch (...) {
        std::cerr << "Invalid --job-ttl-ms\n";
        return 2;
      }
    } else if (a == "--max-jobs") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-jobs\n";
        return 2;
      }
      try {
        cfg.max_jobs = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-jobs\n";
        return 2;
      }
    } else if (a == "--tools") {
      if (!take(&cfg.tools)) {
        std::cerr << "Missing value for --tools\n";
        return 2;
      }
    } else if (a == "--tools-root") {
      if (!take(&cfg.tools_root)) {
        std::cerr << "Missing value for --tools-root\n";
        return 2;
      }
    } else if (a == "--host-scope") {
      if (!take(&cfg.host_scope_root)) {
        std::cerr << "Missing value for --host-scope\n";
        return 2;
      }
    } else if (a == "--yolo") {
      cfg.yolo_default = true;
    } else if (a == "--no-yolo") {
      cfg.yolo_default = false;
    } else if (a == "--no-default-system") {
      cfg.no_default_system = true;
    } else if (a == "--help" || a == "-h") {
      std::cerr
        << "Usage: agentd [options]\n"
        << "  --host <ip>          Listen host (default: 127.0.0.1)\n"
        << "  --auth-token <tok>   Require Authorization: Bearer <tok> (default: disabled)\n"
        << "  --port <n>           Listen port (default: 8123)\n"
        << "  --model <name>       Default model\n"
        << "  --summary-model <name>   Optional model for compaction summaries (tools=none)\n"
        << "  --summary-max-chars <n>  Max chars for inserted summary (default: 1200)\n"
        << "  --base-url <url>     Default base url\n"
        << "  --api-key <key>      Default API key (else env)\n"
        << "  --proxy <url>        Optional HTTP proxy override (else env HTTPS_PROXY/http_proxy)\n"
        << "  --timeout-ms <n>     Provider HTTP timeout in ms (default: 60000)\n"
        << "  --job-ttl-ms <n>     GC finished jobs older than n ms (default: 1800000)\n"
        << "  --max-jobs <n>       Keep at most n jobs in memory (default: 256)\n"
        << "  --tools host|basic|none   Default toolset (default: host)\n"
        << "  --tools-root <path>  Root/working dir for file edits (default: unrestricted)\n"
        << "  --host-scope <path>  Host scope root for tools_root=\"@host\" (default: current dir)\n"
        << "  --yolo / --no-yolo   Default unrestricted mode (default: yolo)\n"
        << "  --no-default-system  Disable default host system hint (host tools only)\n";
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 2;
    }
  }

  // Fill from env only when not provided by flags.
  // Important: pick the API key that matches the configured base URL.
  // Otherwise a host environment that exports multiple keys can accidentally send the wrong key to the wrong provider.
  if (cfg.base_url.empty()) {
    if (const char* b = getenv_s("OPENAI_API_BASE")) {
      cfg.base_url = b;
    } else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) {
      cfg.base_url = b2;
    } else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) {
      cfg.base_url = b3;
    } else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) {
      cfg.base_url = b4;
    }
  }
  if (cfg.api_key.empty()) {
    if (url_contains_ci(cfg.base_url, "deepseek")) {
      if (const char* k = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k3;
    } else if (url_contains_ci(cfg.base_url, "openrouter")) {
      if (const char* k = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k3;
    } else {
      if (const char* k = getenv_s("OPENAI_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k3;
    }
  }
  if (cfg.model.empty()) {
    if (const char* m = getenv_s("AGENT_MODEL")) {
      cfg.model = m;
    }
  }
  if (cfg.auth_token.empty()) {
    if (const char* t = getenv_s("AGENTD_AUTH_TOKEN")) {
      cfg.auth_token = t;
    }
  }

  OpenAIClientConfig ocfg;
  ocfg.base_url = cfg.base_url;
  ocfg.api_key = cfg.api_key;
  ocfg.model = cfg.model;
  ocfg.proxy_url = cfg.proxy_url;
  ocfg.timeout_ms = cfg.timeout_ms;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) {
    ocfg.openrouter_http_referer = r;
  }
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) {
    ocfg.openrouter_x_title = t;
  }

  HttpServer server;
  server.set_default_headers({
    {"Server", "agentd/0.1"},
    {"Access-Control-Allow-Origin", "*"},
    {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
  });

  // Background GC for finished jobs (keeps daemon memory bounded for long-running usage).
  if (cfg.job_ttl_ms > 0 || cfg.max_jobs > 0) {
    const int64_t ttl_ms = cfg.job_ttl_ms;
    const size_t max_jobs = cfg.max_jobs;
    std::thread([ttl_ms, max_jobs]() {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        job_gc(ttl_ms, max_jobs);
      }
    }).detach();
  }

  server.handle("GET", "/api/v1/health", [&](const HttpRequest&, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":true,"service":"agentd","version":"0.1"})";
  });

  server.handle("GET", "/api/v1/tools", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    std::string tools = cfg.tools;
    if (const auto q = query_get(req.query, "tools"); q && !q->empty()) {
      tools = *q;
    }
    // Allow callers to request the daemon's host scope via tools_root=@host.
    std::string tools_root = cfg.tools_root;
    if (const auto q = query_get(req.query, "tools_root"); q) {
      tools_root = *q;
    }
    bool yolo = cfg.yolo_default;
    if (const auto q = query_get(req.query, "yolo"); q) {
      yolo = string_to_bool(*q);
    }

    if (tools_root == "@host") {
      tools_root = cfg.host_scope_root;
    } else if (tools_root == "@cwd") {
      tools_root = "";
    }
    if (yolo) {
      tools_root.clear();
    }

    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["tools"] = tools;
    out["effective_tools_root"] = tools_root;
    out["effective_yolo"] = yolo;

    agent_tool_registry_t* registry = nullptr;
    agent_tool_executor_t executor{};
    bool need_destroy_executor = false;

    if (tools == "none") {
      out["count"] = 0;
      out["defs"] = Json::Value(Json::arrayValue);
      resp->body = json_stringify(out);
      return;
    }
    if (tools == "basic") {
      if (toolset_basic_create(&registry, &executor) != AGENT_OK) {
        resp->status = 500;
        resp->body = R"({"ok":false,"error":"failed to init toolset_basic"})";
        return;
      }
    } else if (tools == "host") {
      HostToolsetConfig hcfg;
      hcfg.root_dir = tools_root;
      if (toolset_host_create(hcfg, &registry, &executor) != AGENT_OK) {
        resp->status = 500;
        resp->body = R"({"ok":false,"error":"failed to init toolset_host"})";
        return;
      }
      need_destroy_executor = true;
    } else {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid tools (expected: none|basic|host)\"}";
      return;
    }

    Json::Value arr(Json::arrayValue);
    const size_t n = agent_tool_registry_count(registry);
    for (size_t i = 0; i < n; i++) {
      agent_tool_def_view_t v{};
      if (agent_tool_registry_get(registry, i, &v) != AGENT_OK) continue;
      Json::Value d(Json::objectValue);
      d["name"] = v.name ? v.name : "";
      d["description"] = v.description ? v.description : "";
      d["parameters_json"] = v.parameters_json ? v.parameters_json : "";
      arr.append(d);
    }
    out["count"] = (Json::UInt64)arr.size();
    out["defs"] = arr;

    agent_tool_registry_destroy(registry);
    if (need_destroy_executor) {
      toolset_host_destroy(&executor);
    }

    resp->body = json_stringify(out);
    return;
#endif
  });

  server.handle("GET", "/api/v1/openrouter/models", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    // Query params:
    // - min_total/max_total: total $/1M tokens (prompt+completion)
    // - require_multimodal_input: 1/0
    // - require_tools: 1/0
    // - include_free: 1/0
    // - limit: max rows
    // - refresh: bypass cache
    const double min_total = [&]() -> double {
      const auto q = query_get(req.query, "min_total");
      if (!q || q->empty()) return 0.01;
      try { return std::stod(*q); } catch (...) { return 0.01; }
    }();
    const double max_total = [&]() -> double {
      const auto q = query_get(req.query, "max_total");
      if (!q || q->empty()) return 0.50;
      try { return std::stod(*q); } catch (...) { return 0.50; }
    }();
    const bool require_multimodal_input = [&]() -> bool {
      const auto q = query_get(req.query, "require_multimodal_input");
      if (!q) return true;
      return string_to_bool(*q);
    }();
    const bool require_tools = [&]() -> bool {
      const auto q = query_get(req.query, "require_tools");
      if (!q) return true;
      return string_to_bool(*q);
    }();
    const bool include_free = [&]() -> bool {
      const auto q = query_get(req.query, "include_free");
      if (!q) return false;
      return string_to_bool(*q);
    }();
    const int limit = [&]() -> int {
      const auto q = query_get(req.query, "limit");
      if (!q || q->empty()) return 50;
      try { return std::max(1, std::min(200, std::stoi(*q))); } catch (...) { return 50; }
    }();
    const bool refresh = [&]() -> bool {
      const auto q = query_get(req.query, "refresh");
      if (!q) return false;
      return string_to_bool(*q);
    }();

    // Base url for OpenRouter models endpoint.
    const std::string base_url = [&]() -> std::string {
      const auto q = query_get(req.query, "base_url");
      if (q && !q->empty()) return *q;
      const char* b = getenv_s("OPENROUTER_API_BASE");
      return b && b[0] ? std::string(b) : std::string("https://openrouter.ai/api/v1");
    }();
    const std::string models_url = trim_slashes(base_url) + "/models";

    // API key precedence (provider key; distinct from daemon auth):
    // 1) X-OpenRouter-Key header
    // 2) Authorization header (Bearer ...) ONLY when daemon auth is disabled
    // 3) env OPENROUTER_API_KEY
    // 4) env OPENAI_API_KEY (fallback)
    std::string key;
    {
      const std::string xk = header_get_ci(req.headers, "x-openrouter-key");
      if (!xk.empty()) {
        key = xk;
      } else if (cfg.auth_token.empty()) {
        const std::string auth = header_get_ci(req.headers, "authorization");
        key = bearer_token_from_auth_header(auth);
      }
      if (key.empty()) {
        if (const char* k = getenv_s("OPENROUTER_API_KEY")) key = k;
        else if (const char* k2 = getenv_s("OPENAI_API_KEY")) key = k2;
      }
    }
    if (key.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"missing OpenRouter key (set OPENROUTER_API_KEY or send X-OpenRouter-Key)\"}";
      return;
    }

    struct Cache {
      std::mutex mu;
      int64_t fetched_unix_ms = 0;
      std::string cache_key;
      Json::Value payload;
    };
    static Cache cache;

    const auto cache_key = [&]() -> std::string {
      const size_t kh = std::hash<std::string>{}(key);
      return models_url + "|" + std::to_string((unsigned long long)kh);
    }();
    const int64_t now = now_unix_ms();
    const int64_t ttl_ms = 10 * 60 * 1000;

    Json::Value payload;
    bool cached = false;
    {
      std::lock_guard<std::mutex> lk(cache.mu);
      if (!refresh && cache.cache_key == cache_key && cache.payload.isObject() && (now - cache.fetched_unix_ms) < ttl_ms) {
        payload = cache.payload;
        cached = true;
      }
    }

    long http_status = 0;
    std::string raw_body;
    if (!cached) {
      OpenAIClientConfig cfg2 = ocfg;
      cfg2.base_url = models_url; // unused by GET
      cfg2.api_key = key;

      OpenAIRawResult r = openai_http_get_raw(cfg2, models_url, {});
      http_status = r.http_status;
      raw_body = r.response_body;
      if (http_status < 200 || http_status >= 300) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to fetch OpenRouter models";
        o["http_status"] = (Json::Int64)http_status;
        o["http_body"] = raw_body;
        resp->status = 502;
        resp->body = json_stringify(o);
        return;
      }
      std::string perr;
      if (!json_parse_any(raw_body, &payload, &perr) || !payload.isObject()) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "failed to parse OpenRouter models JSON";
        o["parse_error"] = perr;
        o["http_status"] = (Json::Int64)http_status;
        resp->status = 502;
        resp->body = json_stringify(o);
        return;
      }
      {
        std::lock_guard<std::mutex> lk(cache.mu);
        cache.cache_key = cache_key;
        cache.payload = payload;
        cache.fetched_unix_ms = now;
      }
    }

    const auto& data = payload["data"];
    if (!data.isArray()) {
      resp->status = 502;
      resp->body = "{\"ok\":false,\"error\":\"unexpected OpenRouter models response (missing data array)\"}";
      return;
    }

    struct Row {
      double total = 0.0;
      double prompt = 0.0;
      double completion = 0.0;
      Json::Value model;
    };
    std::vector<Row> rows;
    rows.reserve((size_t)data.size());
    for (const auto& m : data) {
      if (!m.isObject()) continue;
      const auto& pricing = m["pricing"];
      const double prompt_pm = pricing_to_per_million(pricing["prompt"]);
      const double completion_pm = pricing_to_per_million(pricing["completion"]);
      const double total = prompt_pm + completion_pm;
      if (!include_free && total <= 0.0) continue;
      if (total < min_total || total > max_total) continue;
      if (require_tools && !model_supports_tools(m)) continue;
      if (require_multimodal_input && !model_has_multimodal_input(m)) continue;
      Row r;
      r.total = total;
      r.prompt = prompt_pm;
      r.completion = completion_pm;
      r.model = m;
      rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      if (a.total != b.total) return a.total < b.total;
      if (a.prompt != b.prompt) return a.prompt < b.prompt;
      if (a.completion != b.completion) return a.completion < b.completion;
      const std::string ida = a.model.isMember("id") && a.model["id"].isString() ? a.model["id"].asString() : "";
      const std::string idb = b.model.isMember("id") && b.model["id"].isString() ? b.model["id"].asString() : "";
      return ida < idb;
    });

    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["source"] = "openrouter";
    out["base_url"] = base_url;
    out["models_url"] = models_url;
    out["cached"] = cached;
    out["fetched_unix_ms"] = (Json::Int64)(cached ? cache.fetched_unix_ms : now);
    out["min_total"] = min_total;
    out["max_total"] = max_total;
    out["require_multimodal_input"] = require_multimodal_input;
    out["require_tools"] = require_tools;
    out["include_free"] = include_free;
    out["limit"] = limit;
    out["total_models"] = (Json::UInt64)data.size();

    Json::Value arr(Json::arrayValue);
    for (int i = 0; i < limit && i < (int)rows.size(); i++) {
      const auto& r = rows[(size_t)i];
      const auto& m = r.model;
      Json::Value e(Json::objectValue);
      e["id"] = m.isMember("id") && m["id"].isString() ? m["id"].asString() : "";
      e["name"] = m.isMember("name") && m["name"].isString() ? m["name"].asString() : "";
      e["context_length"] = m.isMember("context_length") ? m["context_length"] : Json::Value(0);
      e["total_usd_per_million"] = r.total;
      e["prompt_usd_per_million"] = r.prompt;
      e["completion_usd_per_million"] = r.completion;
      e["supports_tools"] = model_supports_tools(m);
      e["supports_multimodal_input"] = model_has_multimodal_input(m);
      const auto& arch = m["architecture"];
      if (arch.isObject()) {
        e["input_modalities"] = arch["input_modalities"];
        e["output_modalities"] = arch["output_modalities"];
      }
      arr.append(e);
    }
    out["count"] = (Json::UInt64)arr.size();
    out["models"] = arr;
    out["recommended_model"] = arr.size() > 0 ? arr[0]["id"] : Json::Value("");

    resp->body = json_stringify(out);
    return;
#endif
  });

  server.handle("GET", "/api/v1/file", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    if (!daemon_require_auth(cfg, req, resp)) return;
    const auto path_q = query_get(req.query, "path");
    const auto yolo_q = query_get(req.query, "yolo");
    const bool yolo = yolo_q && (*yolo_q == "1" || *yolo_q == "true");
    if (!path_q || path_q->empty()) {
      resp->status = 400;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"missing path"})";
      return;
    }

    const std::filesystem::path user_path(*path_q);
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path scope_root = std::filesystem::path(cfg.host_scope_root);

    std::filesystem::path resolved;
    if (yolo) {
      resolved = user_path.is_absolute() ? user_path : (cwd / user_path);
    } else {
      if (user_path.is_absolute()) {
        resp->status = 400;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"absolute path requires yolo=1"})";
        return;
      }
      resolved = (scope_root / user_path);
    }
    resolved = resolved.lexically_normal();

    // Containment check when not yolo.
    if (!yolo) {
      std::error_code ec;
      const auto canon_root = std::filesystem::weakly_canonical(scope_root, ec);
      if (ec) {
        resp->status = 500;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"failed to canonicalize host scope root"})";
        return;
      }
      ec.clear();
      const auto canon_file = std::filesystem::weakly_canonical(resolved, ec);
      if (ec) {
        resp->status = 404;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"file not found"})";
        return;
      }
      const auto root_s = canon_root.native();
      const auto file_s = canon_file.native();
      if (file_s.size() < root_s.size() || file_s.compare(0, root_s.size(), root_s) != 0) {
        resp->status = 403;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"path escapes host scope"})";
        return;
      }
    }

    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) || !std::filesystem::is_regular_file(resolved, ec)) {
      resp->status = 404;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"file not found"})";
      return;
    }

    const uintmax_t max_bytes = 10ULL * 1024ULL * 1024ULL;
    const uintmax_t sz = std::filesystem::file_size(resolved, ec);
    if (ec || sz > max_bytes) {
      resp->status = 400;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"file too large"})";
      return;
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in.is_open()) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to open file"})";
      return;
    }
    std::string bytes;
    bytes.resize((size_t)sz);
    in.read(bytes.data(), (std::streamsize)bytes.size());
    if (!in) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to read file"})";
      return;
    }

    resp->status = 200;
    resp->headers["Content-Type"] = content_type_from_path(resolved);
    resp->body = std::move(bytes);
  });

  server.handle("GET", "/api/v1/sessions", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    SessionStoreConfig store_cfg;
    store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();

    agent_persistor_t p{};
    const agent_status_t pst = agent_file_persistor_create(store_cfg.root_dir.c_str(), &p);
    std::vector<std::string> ids;
    agent_status_t st = pst;
    if (pst == AGENT_OK && p.list) {
      auto sink = [](void* vctx, const char* id) {
        auto* vec = static_cast<std::vector<std::string>*>(vctx);
        vec->push_back(id ? id : "");
      };
      st = p.list(p.ctx, sink, &ids);
    }
    agent_persistor_destroy(&p);

    Json::Value out(Json::objectValue);
    out["ok"] = (st == AGENT_OK);
    if (st != AGENT_OK) {
      out["error"] = "failed to list sessions";
      out["status"] = (Json::Int64)st;
    }
    Json::Value arr(Json::arrayValue);
    for (const auto& s : ids) {
      arr.append(s);
    }
    out["sessions"] = arr;
    resp->body = json_stringify(out);
    return;
#endif
  });

  server.handle("GET", "/api/v1/session", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto sid = query_get(req.query, "session_id");
    if (!sid || sid->empty()) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"missing session_id"})";
      return;
    }

    SessionStoreConfig store_cfg;
    store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();
    agent_session_t* session = nullptr;
    agent_persistor_t p{};
    const agent_status_t pst = agent_file_persistor_create(store_cfg.root_dir.c_str(), &p);
    const agent_status_t st = (pst == AGENT_OK) ? p.load(p.ctx, sid->c_str(), &session) : pst;
    agent_persistor_destroy(&p);
    if (st != AGENT_OK || !session) {
      resp->status = 500;
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["error"] = "failed to load session";
      out["status"] = (Json::Int64)st;
      resp->body = json_stringify(out);
      return;
    }

    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["session_id"] = *sid;
    Json::Value msgs(Json::arrayValue);
    const size_t n = agent_session_message_count(session);
    for (size_t i = 0; i < n; i++) {
      agent_message_view_t v{};
      if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
      Json::Value m(Json::objectValue);
      m["role"] = agent_role_to_string(v.role);
      m["content"] = std::string(v.content, v.content_len);
      msgs.append(m);
    }
    out["messages"] = msgs;
    agent_session_destroy(session);
    resp->body = json_stringify(out);
    return;
#endif
  });

  server.handle("GET", "/api/v1/session/audit", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto sid = query_get(req.query, "session_id");
    if (!sid || sid->empty()) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"missing session_id"})";
      return;
    }
    size_t max_bytes = 1024 * 1024;
    if (const auto mb = query_get(req.query, "max_bytes")) {
      try {
        max_bytes = (size_t)std::stoull(*mb);
      } catch (...) {
      }
    }
    SessionStoreConfig store_cfg;
    store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();
    std::string tail;
    const agent_status_t st = session_store_read_audit_tail(store_cfg, *sid, max_bytes, &tail);
    Json::Value out(Json::objectValue);
    out["ok"] = (st == AGENT_OK);
    out["session_id"] = *sid;
    if (st != AGENT_OK) {
      out["error"] = "failed to read audit log";
      out["status"] = (Json::Int64)st;
    }
    // Parse JSONL into entries (best-effort).
    Json::Value entries(Json::arrayValue);
    std::istringstream iss(tail);
    std::string line;
    Json::CharReaderBuilder rb;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      std::string errs;
      std::istringstream lss(line);
      Json::Value v;
      if (Json::parseFromStream(rb, lss, &v, &errs) && v.isObject()) {
        entries.append(v);
      }
    }
    out["entries"] = entries;
    resp->body = json_stringify(out);
    return;
#endif
  });

  server.handle("DELETE", "/api/v1/session", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto sid = query_get(req.query, "session_id");
    if (!sid || sid->empty()) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"missing session_id"})";
      return;
    }
    SessionStoreConfig store_cfg;
    store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();
    agent_persistor_t p{};
    const agent_status_t pst = agent_file_persistor_create(store_cfg.root_dir.c_str(), &p);
    const agent_status_t st = (pst == AGENT_OK) ? p.del(p.ctx, sid->c_str()) : pst;
    agent_persistor_destroy(&p);
    Json::Value out(Json::objectValue);
    out["ok"] = (st == AGENT_OK);
    out["session_id"] = *sid;
    if (st != AGENT_OK) {
      out["error"] = "failed to delete session";
      out["status"] = (Json::Int64)st;
    }
    resp->body = json_stringify(out);
    return;
#endif
  });

  server.handle("POST", "/api/v1/run", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;

#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto started = std::chrono::steady_clock::now();
    std::cerr << "agentd: /api/v1/run start bytes=" << req.body.size() << "\n";
    Json::Value out = run_request_to_json(cfg, ocfg, req.body, nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    const bool ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
    std::cerr << "agentd: /api/v1/run done ok=" << (ok ? "true" : "false") << " ms=" << ms << "\n";
    if (out.isObject() && out.isMember("rpc_status") && out["rpc_status"].isInt()) {
      resp->status = out["rpc_status"].asInt();
    }
    resp->body = json_stringify(out);
#endif
  });

  // Async run: returns a job id immediately and completes in the background.
  server.handle("POST", "/api/v1/run_async", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;

#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
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
    std::thread([job_id, body_copy, cfg, ocfg]() mutable {
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
        Json::Value out = run_request_to_json(cfg, ocfg, body_copy, job_id.c_str());
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
    return;
#endif
  });

  server.handle("GET", "/api/v1/job", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto jid = query_get(req.query, "job_id");
    if (!jid || jid->empty()) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"missing job_id"})";
      return;
    }
    JobState s;
    if (!job_get(*jid, &s)) {
      resp->status = 404;
      resp->body = R"({"ok":false,"error":"job not found"})";
      return;
    }
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["job_id"] = s.id;
    o["status"] = s.status;
    o["error"] = s.error;
    o["created_unix_ms"] = (Json::Int64)s.created_unix_ms;
    o["updated_unix_ms"] = (Json::Int64)s.updated_unix_ms;
    // Optional live events for progress (polling UI).
    const auto include_ev = query_get(req.query, "include_events");
    const bool want_events = include_ev && (*include_ev == "1" || *include_ev == "true");
    const auto cursor_q = query_get(req.query, "cursor");
    const auto max_q = query_get(req.query, "max_events");
    uint64_t cursor = 0;
    if (cursor_q && !cursor_q->empty()) {
      try {
        cursor = (uint64_t)std::stoull(*cursor_q);
      } catch (...) {
        cursor = 0;
      }
    }
    size_t max_events = 256;
    if (max_q && !max_q->empty()) {
      try {
        max_events = (size_t)std::stoull(*max_q);
      } catch (...) {
        max_events = 256;
      }
    }
    if (max_events == 0) max_events = 256;
    if (max_events > 2048) max_events = 2048;

    o["events_cursor_base"] = (Json::UInt64)s.events_offset;
    o["events_cursor_end"] = (Json::UInt64)(s.events_offset + (uint64_t)s.events.size());
    if (want_events) {
      Json::Value slice(Json::arrayValue);
      const uint64_t base = s.events_offset;
      const uint64_t end = base + (uint64_t)s.events.size();
      bool reset = false;
      uint64_t cur = cursor;
      if (cur < base) {
        reset = true;
        cur = base;
      }
      if (cur > end) {
        cur = end;
      }
      const uint64_t start_idx = cur - base;
      const uint64_t avail = end - cur;
      const uint64_t take = std::min<uint64_t>((uint64_t)max_events, avail);
      for (uint64_t i = 0; i < take; i++) {
        slice.append(s.events[(Json::ArrayIndex)(start_idx + i)]);
      }
      o["events"] = slice;
      o["events_cursor_next"] = (Json::UInt64)(cur + take);
      o["events_reset"] = reset;
    }

    if (s.status == "done" || s.status == "error") {
      o["result"] = s.result;
    }
    resp->body = json_stringify(o);
    return;
#endif
  });

  server.handle("POST", "/api/v1/job/cancel", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto jid = query_get(req.query, "job_id");
    if (!jid || jid->empty()) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"missing job_id"})";
      return;
    }
    if (!job_request_cancel(*jid)) {
      resp->status = 409;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "cannot cancel job (not found or already finished)";
      o["job_id"] = *jid;
      resp->body = json_stringify(o);
      return;
    }
    // Best-effort: surface cancellation request to any connected UI.
    {
      Json::Value d(Json::objectValue);
      d["job_id"] = *jid;
      d["ts_unix_ms"] = (Json::Int64)now_unix_ms();
      job_append_event(*jid, "cancel_requested", json_stringify(d));
    }
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["job_id"] = *jid;
    resp->body = json_stringify(o);
    return;
#endif
  });

  // Server-Sent Events stream for job progress (preferred UI path vs polling).
  // This endpoint streams `agent_event` events (same object shape as entries in the `events` array) and ends with `job_done`.
  server.handle_stream("GET", "/api/v1/job/stream", [&](const HttpRequest& req, int client_fd) {
#if !defined(AGENT_HAVE_JSONCPP)
    (void)req;
    const std::string wire =
      "HTTP/1.1 500 Internal Server Error\r\n"
      "Content-Type: application/json; charset=utf-8\r\n"
      "Connection: close\r\n"
      "\r\n"
      "{\"ok\":false,\"error\":\"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)\"}";
    (void)write_all_fd(client_fd, wire);
    return;
#else
    if (!daemon_auth_ok(cfg, req)) {
      const std::string wire =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "WWW-Authenticate: Bearer\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":false,\"error\":\"unauthorized\"}";
      (void)write_all_fd(client_fd, wire);
      return;
    }
    const auto jid = query_get(req.query, "job_id");
    if (!jid || jid->empty()) {
      const std::string wire =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"ok\":false,\"error\":\"missing job_id\"}";
      (void)write_all_fd(client_fd, wire);
      return;
    }

    uint64_t cursor = 0;
    const auto cursor_q = query_get(req.query, "cursor");
    if (cursor_q && !cursor_q->empty()) {
      try {
        cursor = (uint64_t)std::stoull(*cursor_q);
      } catch (...) {
        cursor = 0;
      }
    }

    // SSE headers
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n";
    hdr << "Content-Type: text/event-stream\r\n";
    hdr << "Cache-Control: no-cache\r\n";
    hdr << "Connection: keep-alive\r\n";
    hdr << "Access-Control-Allow-Origin: *\r\n";
    hdr << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    hdr << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    hdr << "\r\n";
    if (!write_all_fd(client_fd, hdr.str())) {
      return;
    }

    auto last_send = std::chrono::steady_clock::now();
    for (;;) {
      JobState s;
      if (!job_get(*jid, &s)) {
        (void)sse_send(client_fd, "error", R"({"ok":false,"error":"job not found"})");
        return;
      }

      const uint64_t base = s.events_offset;
      const uint64_t end = base + (uint64_t)s.events.size();

      if (cursor < base) {
        Json::Value d(Json::objectValue);
        d["reason"] = "cursor_too_old";
        d["cursor_base"] = (Json::UInt64)base;
        d["cursor_end"] = (Json::UInt64)end;
        if (!sse_send(client_fd, "reset", json_stringify(d))) {
          return;
        }
        cursor = base;
        last_send = std::chrono::steady_clock::now();
      }

      bool sent_any = false;
      while (cursor < end) {
        const uint64_t idx = cursor - base;
        const Json::Value& ev = s.events[(Json::ArrayIndex)idx];
        if (!sse_send(client_fd, "agent_event", json_stringify(ev), std::to_string((unsigned long long)cursor))) {
          return;
        }
        cursor++;
        sent_any = true;
      }
      if (sent_any) {
        last_send = std::chrono::steady_clock::now();
      }

      if (s.status == "done" || s.status == "error") {
        Json::Value out(Json::objectValue);
        out["ok"] = (s.status == "done");
        out["job_id"] = s.id;
        out["status"] = s.status;
        out["error"] = s.error;
        out["result"] = s.result;
        out["events_cursor_next"] = (Json::UInt64)cursor;
        (void)sse_send(client_fd, "job_done", json_stringify(out));
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - last_send).count() >= 15) {
        if (!sse_ping(client_fd)) {
          return;
        }
        last_send = now;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
#endif
  });

  server.handle("DELETE", "/api/v1/job", [&](const HttpRequest& req, HttpResponse* resp) {
    add_cors(resp);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
#if !defined(AGENT_HAVE_JSONCPP)
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"agentd requires jsoncpp (AGENT_HAVE_JSONCPP)"})";
    return;
#else
    const auto jid = query_get(req.query, "job_id");
    if (!jid || jid->empty()) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"missing job_id"})";
      return;
    }
    if (!job_delete(*jid)) {
      resp->status = 409;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "cannot delete job (still running or not found)";
      o["job_id"] = *jid;
      resp->body = json_stringify(o);
      return;
    }
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["job_id"] = *jid;
    resp->body = json_stringify(o);
    return;
#endif
  });

  std::string err;
  std::cerr << "agentd listening on http://" << cfg.listen_host << ":" << cfg.listen_port << "\n";
  if (!server.serve(cfg.listen_host, cfg.listen_port, &err)) {
    std::cerr << "agentd failed: " << err << "\n";
    return 1;
  }
  return 0;
}
