#include "run_endpoints.h"

#include "daemon_auth.h"
#include "default_system_prompt.h"
#include "file_persistor.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"
#include "openai_provider.h"
#include "sandbox_policy.h"
#include "session_store.h"
#include "string_util.h"
#include "summary_compaction.h"
#include "summary_llm.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"

#include "agent/agent.h"
#include "agent/runner.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace agentd {
namespace {

// Parses the daemon run request body and returns a response JSON object (HTTP-level errors are represented in JSON).
static Json::Value run_request_to_json(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
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
  if (args.isMember("model") && args["model"].isString()) run_cfg.model = args["model"].asString();
  if (args.isMember("base_url") && args["base_url"].isString()) run_cfg.base_url = args["base_url"].asString();
  if (args.isMember("api_key") && args["api_key"].isString()) run_cfg.api_key = args["api_key"].asString();
  if (args.isMember("proxy") && args["proxy"].isString()) run_cfg.proxy_url = args["proxy"].asString();
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const long t = (long)args["timeout_ms"].asInt64();
    if (t > 0) run_cfg.timeout_ms = t;
  }

  const std::string tools = args.isMember("tools") && args["tools"].isString() ? args["tools"].asString() : daemon_cfg.tools;
  const bool requested_yolo_set = args.isMember("yolo") && args["yolo"].isBool();
  const bool requested_yolo = requested_yolo_set ? args["yolo"].asBool() : daemon_cfg.yolo_default;
  const bool yolo = sandbox_tighten_yolo(daemon_cfg.yolo_default, requested_yolo, requested_yolo_set);
  const bool no_default_system =
    args.isMember("no_default_system") && args["no_default_system"].isBool() ? args["no_default_system"].asBool() : daemon_cfg.no_default_system;
  const std::string system_msg = args.isMember("system") && args["system"].isString() ? args["system"].asString() : "";
  const bool requested_tools_root_set = args.isMember("tools_root") && args["tools_root"].isString();
  const std::string requested_tools_root = requested_tools_root_set ? args["tools_root"].asString() : "";
  std::string tools_root;
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
  {
    std::string root_err;
    if (!sandbox_resolve_tools_root(
          daemon_cfg.host_scope_root,
          yolo,
          daemon_cfg.tools_root,
          requested_tools_root,
          requested_tools_root_set,
          &tools_root,
          &root_err
        )) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 403;
      o["error"] = root_err.empty() ? "invalid tools_root" : root_err;
      return o;
    }
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
  store_cfg.root_dir = sessions_root_dir;

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
    opt.stream_assistant = stream_assistant;
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
  if (need_destroy_executor) {
    toolset_host_destroy(&executor);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = ok;
  out["assistant_text"] = assistant_text;
  if (!ok) out["error"] = err;
  if (http_status) out["http_status"] = (Json::Int64)http_status;
  if (!http_body.empty()) out["http_body"] = http_body;
  if (trace_stream) out["trace_text"] = trace_buf.str();
  out["effective_tools_root"] = tools_root;
  out["effective_yolo"] = yolo;
  out["effective_host_policy"] = host_policy_to_string(effective_policy);
  out["effective_timeout_ms"] = (Json::Int64)run_cfg.timeout_ms;
  out["effective_stream_assistant"] = stream_assistant;
  out["verbose"] = verbose;
  out["events"] = events_out;

  // Persist session if enabled.
  if (ok && !no_session) {
    // Best-effort save.
    (void)persistor.save(persistor.ctx, session_id.c_str(), session);
  }

  // Append a per-run audit record (host-only; used by `/api/v1/session/audit`).
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
    (void)session_store_append_audit_jsonl(store_cfg, session_id, Json::writeString(wb, record));
  }

  agent_session_destroy(session);
  return out;
}

}  // namespace

void handle_run_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto started = std::chrono::steady_clock::now();
  std::cerr << "agentd: /api/v1/run start bytes=" << req.body.size() << "\n";
  Json::Value out = run_request_to_json(cfg, ocfg, sessions_root_dir, req.body, nullptr);
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
  std::thread([job_id, body_copy, cfg, ocfg, sessions_root_dir]() mutable {
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
      Json::Value out = run_request_to_json(cfg, ocfg, sessions_root_dir, body_copy, job_id.c_str());
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
