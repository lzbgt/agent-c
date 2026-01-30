#include "tool_loop.h"

#include "agent/tool_loop.h"

#include "openai_client.h"
#include "openai_tool_provider.h"
#include "tool_loop_truncation.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <algorithm>
#include <cctype>
#include <ostream>
#include <sstream>

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

static bool parse_json_any(const std::string& s, Json::Value* out_v) {
  if (!out_v) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) return false;
  *out_v = v;
  return true;
}

static bool parse_json_object(const std::string& s, Json::Value* out_obj) {
  Json::Value v;
  if (!parse_json_any(s, &v)) return false;
  if (!v.isObject()) return false;
  *out_obj = v;
  return true;
}

static Json::Value summarize_tool_output_json(const std::string& tool_out) {
  Json::Value obj;
  if (!parse_json_object(tool_out, &obj)) {
    Json::Value s(Json::objectValue);
    s["kind"] = "text";
    s["bytes"] = (Json::UInt64)tool_out.size();
    return s;
  }
  Json::Value summary(Json::objectValue);
  summary["kind"] = "json_envelope";
  if (obj.isMember("ok")) summary["ok"] = obj["ok"];
  if (obj.isMember("error")) summary["error"] = obj["error"];
  const auto& data = obj["data"];
  if (data.isObject()) {
    if (data.isMember("exit_code")) summary["exit_code"] = data["exit_code"];
    if (data.isMember("timed_out")) summary["timed_out"] = data["timed_out"];
    if (data.isMember("truncated")) summary["truncated"] = data["truncated"];
    if (data.isMember("argv")) summary["argv"] = data["argv"];
    if (data.isMember("tool")) summary["tool"] = data["tool"];
    // Special-case: keep artifact metadata even when verbose events are disabled.
    if (data.isMember("tool") && data["tool"].isString() && data["tool"].asString() == "artifact_register") {
      if (data.isMember("artifact") && data["artifact"].isObject()) {
        summary["artifact"] = data["artifact"];
      }
    }
    if (data.isMember("check") && data["check"].isObject() && data["check"].isMember("exit_code")) {
      summary["check_exit_code"] = data["check"]["exit_code"];
    }
    if (data.isMember("apply") && data["apply"].isObject() && data["apply"].isMember("exit_code")) {
      summary["apply_exit_code"] = data["apply"]["exit_code"];
    }
  }
  return summary;
}

struct EventSink {
  Json::Value events = Json::Value(Json::arrayValue);
  bool events_truncated = false;

  size_t captured_bytes = 0;
  size_t max_total_capture_bytes = 2 * 1024 * 1024;

  size_t events_count = 0;
  size_t max_events = 4000;

  ToolLoopEventCallback forward_cb = nullptr;
  void* forward_ctx = nullptr;

  std::ostream* trace_stream = nullptr;
};

static void sink_note_capture(EventSink* sink, const std::string& s) {
  if (!sink) return;
  if (s.size() > sink->max_total_capture_bytes) {
    sink->captured_bytes += sink->max_total_capture_bytes;
  } else {
    sink->captured_bytes += s.size();
  }
}

static void sink_on_event(void* vctx, const char* type, const char* data_json) {
  auto* sink = static_cast<EventSink*>(vctx);
  if (!sink || !type) return;

  if (sink->events_count >= sink->max_events) {
    sink->events_truncated = true;
    return;
  }

  Json::Value data;
  std::string perr;
  if (data_json && data_json[0]) {
    if (!parse_json_any(data_json, &data)) {
      data = std::string(data_json);
    }
  } else {
    data = Json::Value(Json::objectValue);
  }

  Json::Value e(Json::objectValue);
  e["type"] = type;
  e["data"] = data;
  sink->events.append(e);
  sink->events_count++;

  if (data_json) sink_note_capture(sink, data_json);

  // Derived UI event: artifact
  if (data.isObject() && std::string(type) == "tool_result") {
    const std::string tool_name = data.isMember("tool_name") && data["tool_name"].isString() ? data["tool_name"].asString() : "";
    if (tool_name == "artifact_register") {
      Json::Value artifact;
      if (data.isMember("summary") && data["summary"].isObject()) {
        const auto& s = data["summary"];
        if (s.isMember("artifact") && s["artifact"].isObject()) {
          artifact = s["artifact"];
        }
      }
      if (artifact.isNull() && data.isMember("content") && data["content"].isString()) {
        Json::Value content_obj;
        if (parse_json_object(data["content"].asString(), &content_obj)) {
          if (content_obj.isMember("data") && content_obj["data"].isObject()) {
            const auto& cd = content_obj["data"];
            if (cd.isMember("tool") && cd["tool"].isString() && cd["tool"].asString() == "artifact_register") {
              if (cd.isMember("artifact") && cd["artifact"].isObject()) {
                artifact = cd["artifact"];
              }
            }
          }
        }
      }

      if (artifact.isObject() && sink->events_count < sink->max_events) {
        Json::Value ad(Json::objectValue);
        if (data.isMember("step")) ad["step"] = data["step"];
        if (data.isMember("tool_call_id")) ad["tool_call_id"] = data["tool_call_id"];
        ad["tool_name"] = tool_name;
        ad["artifact"] = artifact;

        Json::Value ae(Json::objectValue);
        ae["type"] = "artifact";
        ae["data"] = ad;
        sink->events.append(ae);
        sink->events_count++;

        const std::string aj = json_stringify(ad);
        if (sink->forward_cb) {
          sink->forward_cb(sink->forward_ctx, "artifact", aj.c_str());
        }
      }
    }
  }

  // Optional trace output (best-effort).
  if (sink->trace_stream) {
    const std::string t(type);
    if (t == "llm_request") {
      sink->trace_stream->flush();
      *sink->trace_stream << "=== TOOL LOOP LLM REQUEST ===\n";
      if (data.isObject() && data.isMember("request_json") && data["request_json"].isString()) {
        *sink->trace_stream << data["request_json"].asString() << "\n";
      }
    } else if (t == "llm_response") {
      sink->trace_stream->flush();
      *sink->trace_stream << "=== TOOL LOOP LLM RESPONSE ===\n";
      if (data.isObject() && data.isMember("http_status")) {
        *sink->trace_stream << "HTTP " << data["http_status"].asInt64() << "\n";
      }
      if (data.isObject() && data.isMember("response_body") && data["response_body"].isString()) {
        *sink->trace_stream << data["response_body"].asString() << "\n";
      }
    }
  }

  if (sink->forward_cb) {
    sink->forward_cb(sink->forward_ctx, type, data_json ? data_json : "");
  }
}

static uint8_t sink_should_cancel(void* vctx) {
  auto* opt = static_cast<ToolLoopOptions*>(vctx);
  if (!opt || !opt->should_cancel) return 0;
  return opt->should_cancel(opt->should_cancel_ctx) ? 1 : 0;
}

static agent_status_t cap_tool_output_cb(
  void* /*ctx*/,
  const char* tool_out,
  size_t tool_out_len,
  size_t max_chars,
  agent_string_t* out_capped,
  uint8_t* out_truncated
) {
  if (!out_capped) return AGENT_ERR_INVALID_ARGUMENT;
  if (out_truncated) *out_truncated = 0;
  const std::string in(tool_out ? tool_out : "", tool_out_len);
  bool trunc = false;
  const std::string capped = tool_loop_cap_tool_output_for_prompt(in, max_chars, &trunc);
  if (out_truncated) *out_truncated = trunc ? 1 : 0;
  return agent_string_set_copy(out_capped, capped.data(), capped.size());
}

static agent_status_t summarize_tool_output_cb(
  void* /*ctx*/,
  const char* tool_out,
  size_t tool_out_len,
  agent_string_t* out_summary_json
) {
  if (!out_summary_json) return AGENT_ERR_INVALID_ARGUMENT;
  const std::string in(tool_out ? tool_out : "", tool_out_len);
  const Json::Value summary = summarize_tool_output_json(in);
  const std::string s = json_stringify(summary);
  return agent_string_set_copy(out_summary_json, s.data(), s.size());
}

bool run_tool_loop(
  const OpenAIClientConfig& cfg,
  const agent_session_t* seed_session,
  const std::string& user_prompt,
  const agent_tool_registry_t* tools_registry,
  const agent_tool_executor_t* executor,
  const ToolLoopOptions& options,
  std::ostream* trace_stream,
  ToolLoopResult* out_result,
  std::string* out_error,
  long* out_http_status,
  std::string* out_http_body
) {
  if (out_error) out_error->clear();
  if (out_http_status) *out_http_status = 0;
  if (out_http_body) out_http_body->clear();
  if (!out_result) {
    if (out_error) *out_error = "invalid out_result";
    return false;
  }
  *out_result = ToolLoopResult{};

#if !defined(AGENT_HAVE_JSONCPP)
  if (out_error) *out_error = "tool loop requires jsoncpp (AGENT_HAVE_JSONCPP)";
  (void)cfg;
  (void)seed_session;
  (void)user_prompt;
  (void)tools_registry;
  (void)executor;
  (void)options;
  (void)trace_stream;
  return false;
#else
  if (!seed_session || !tools_registry || !executor || !executor->execute) {
    if (out_error) *out_error = "invalid inputs (seed_session/tools/executor)";
    return false;
  }

  EventSink sink;
  sink.forward_cb = options.on_event;
  sink.forward_ctx = options.on_event_ctx;
  sink.trace_stream = trace_stream;

  OpenAIToolProviderCtx pctx;
  pctx.cfg = cfg;
  pctx.on_event = sink_on_event;
  pctx.on_event_ctx = &sink;
  pctx.stream_assistant = options.stream_assistant;
  pctx.verbose_events = options.verbose;
  pctx.max_capture_bytes = options.max_capture_bytes;
  pctx.max_event_chars = options.max_capture_bytes;

  agent_tool_provider_t provider = openai_make_tool_provider(&pctx);

  agent_tool_loop_options_t opt{};
  opt.model = cfg.model.c_str();
  opt.force_tool_or_null = options.force_tool.empty() ? nullptr : options.force_tool.c_str();
  opt.require_tool_call = options.require_tool_call ? 1 : 0;
  opt.max_steps = options.max_steps;
  opt.max_repeated_tool_calls = options.max_repeated_tool_calls;
  opt.max_chars = options.max_chars;
  opt.keep_last_messages = options.keep_last_messages;
  opt.insert_compaction_summary = options.insert_compaction_summary ? 1 : 0;
  opt.summary_preview_items = options.summary_preview_items;
  opt.summary_snippet_chars = options.summary_snippet_chars;
  opt.summary_max_chars = options.summary_max_chars;
  opt.max_tool_result_chars = options.max_tool_result_chars;
  opt.max_context_too_long_retries = 2;
  opt.verbose_events = options.verbose ? 1 : 0;
  opt.max_capture_chars = options.max_capture_bytes;

  agent_tool_loop_hooks_t hooks{};
  hooks.on_event = sink_on_event;
  hooks.on_event_ctx = &sink;
  hooks.should_cancel = options.should_cancel ? sink_should_cancel : nullptr;
  hooks.should_cancel_ctx = const_cast<ToolLoopOptions*>(&options);
  hooks.cap_tool_output_for_prompt = cap_tool_output_cb;
  hooks.cap_tool_output_for_prompt_ctx = nullptr;
  hooks.cap_tool_output_for_event = cap_tool_output_cb;
  hooks.cap_tool_output_for_event_ctx = nullptr;
  hooks.summarize_tool_output = summarize_tool_output_cb;
  hooks.summarize_tool_output_ctx = nullptr;

  agent_tool_loop_result_t core_res{};
  const agent_status_t st = agent_tool_loop_run(
    &provider,
    tools_registry,
    executor,
    seed_session,
    user_prompt.c_str(),
    &opt,
    &hooks,
    &core_res
  );

  if (out_http_status) *out_http_status = pctx.last_http_status;
  if (out_http_body) *out_http_body = pctx.last_response_body;

  if (st != AGENT_OK) {
    if (out_error) {
      if (core_res.error_message.data && core_res.error_message.data[0]) {
        *out_error = std::string(core_res.error_message.data, core_res.error_message.len);
      } else if (pctx.last_http_status) {
        *out_error = openai_format_http_error(pctx.last_http_status, pctx.last_response_body);
      } else if (st == AGENT_ERR_CANCELLED) {
        *out_error = "cancelled";
      } else {
        *out_error = "tool loop failed";
      }
    }
    agent_tool_loop_result_free(&core_res);
    return false;
  }

  out_result->final_assistant_text = core_res.final_assistant_text.data ? core_res.final_assistant_text.data : "";
  out_result->saw_tool_call = core_res.saw_tool_call != 0;

  // Copy tool records for host audit logging.
  out_result->tool_records.reserve(core_res.tool_record_count);
  for (size_t i = 0; i < core_res.tool_record_count; i++) {
    const agent_tool_record_t& r = core_res.tool_records[i];
    ToolLoopToolRecord rec;
    rec.tool_name = r.tool_name.data ? r.tool_name.data : "";
    rec.tool_call_id = r.tool_call_id.data ? r.tool_call_id.data : "";
    rec.arguments_json = r.arguments_json.data ? r.arguments_json.data : "";
    rec.result_string = r.result_string.data ? r.result_string.data : "";
    rec.result_string_for_prompt = r.result_string_for_prompt.data ? r.result_string_for_prompt.data : "";
    rec.result_truncated_for_prompt = r.result_truncated_for_prompt != 0;
    out_result->tool_records.push_back(std::move(rec));
  }

  // End event (host-side envelope stats).
  {
    Json::Value d(Json::objectValue);
    d["truncated"] = sink.events_truncated;
    d["captured_bytes"] = (Json::UInt64)sink.captured_bytes;
    d["max_total_capture_bytes"] = (Json::UInt64)sink.max_total_capture_bytes;
    d["events_count"] = (Json::UInt64)sink.events_count;
    d["max_events"] = (Json::UInt64)sink.max_events;
    Json::Value e(Json::objectValue);
    e["type"] = "end";
    e["data"] = d;
    sink.events.append(e);
  }

  out_result->events_json = json_stringify(sink.events);
  agent_tool_loop_result_free(&core_res);
  return true;
#endif
}

#endif  // AGENT_HAVE_JSONCPP
