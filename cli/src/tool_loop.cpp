#include "tool_loop.h"

#include "agent/agent.h"
#include "agent/tools.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <ostream>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "openai_client.h"
#include "tool_loop_compaction.h"
#include "tool_loop_truncation.h"

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

  static std::string truncate_str(const std::string& s, size_t max_bytes, bool* out_truncated = nullptr) {
    if (out_truncated) *out_truncated = false;
    if (max_bytes == 0 || s.size() <= max_bytes) {
      return s;
    }
    if (out_truncated) *out_truncated = true;
    return s.substr(0, max_bytes) + "...(truncated)";
  }

static Json::Value session_to_json_messages(const agent_session_t* session) {
  Json::Value messages(Json::arrayValue);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t view{};
    if (agent_session_get_message(session, i, &view) != AGENT_OK) {
      continue;
    }
    Json::Value m(Json::objectValue);
    m["role"] = agent_role_to_string(view.role);
    m["content"] = std::string(view.content, view.content_len);
    messages.append(m);
  }
  return messages;
}

static Json::Value session_to_compacted_json_messages(
  const agent_session_t* session,
  const std::string& user_prompt,
  const ToolLoopOptions& opt,
  Json::Value* out_compaction_event_data
) {
  Json::Value messages = session_to_json_messages(session);

  ToolLoopCompactionOptions copt;
  copt.max_chars = opt.max_chars;
  copt.keep_last_messages = opt.keep_last_messages;
  copt.insert_summary = opt.insert_compaction_summary;
  copt.summary_preview_items = opt.summary_preview_items;
  copt.summary_snippet_chars = opt.summary_snippet_chars;
  copt.summary_max_chars = opt.summary_max_chars;

  const size_t max_chars = copt.max_chars == 0 ? 20000 : copt.max_chars;
  // Reserve space for the new user prompt that will be appended after compaction.
  const size_t budget_for_history = max_chars > user_prompt.size() ? (max_chars - user_prompt.size()) : 1;

  ToolLoopCompactionReport rep;
  const bool did = tool_loop_compaction_maybe_compact_with_budget(&messages, budget_for_history, copt, &rep);
  if (out_compaction_event_data) {
    Json::Value d(Json::objectValue);
    d["before_chars"] = (Json::UInt64)(rep.before_chars + user_prompt.size());
    d["max_chars"] = (Json::UInt64)max_chars;
    d["keep_last_messages"] = (Json::UInt64)(copt.keep_last_messages == 0 ? 16 : copt.keep_last_messages);
    d["pinned_system_messages"] = (Json::UInt64)rep.pinned_system_messages;
    d["dropped_messages"] = (Json::UInt64)rep.dropped_messages;
    d["inserted_summary"] = did && rep.inserted_summary;
    if (opt.verbose && rep.inserted_summary && !rep.summary.empty()) {
      bool trunc = false;
      d["summary"] = truncate_str(rep.summary, opt.max_capture_bytes, &trunc);
      d["summary_truncated"] = trunc;
    }
    *out_compaction_event_data = d;
  }

  return messages;
}

  static bool extract_choice0_message(const Json::Value& root, Json::Value* out_message) {
    if (!out_message) {
      return false;
    }
  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) {
    return false;
  }
  const auto& msg = choices[0]["message"];
  if (!msg.isObject()) {
    return false;
  }
  *out_message = msg;
  return true;
}

static std::string extract_assistant_content(const Json::Value& message) {
  const auto& content = message["content"];
  if (content.isString()) {
    return content.asString();
  }
  return "";
}

static bool message_has_tool_calls(const Json::Value& message) {
  const auto& tc = message["tool_calls"];
  if (tc.isArray() && !tc.empty()) {
    return true;
  }
  // Older/alternate schema (function calling v0): {"function_call": {"name": "...", "arguments": "..."}}
  const auto& fc = message["function_call"];
  return fc.isObject() && fc.isMember("name") && fc.isMember("arguments");
}

static bool parse_json_object(const std::string& s, Json::Value* out_obj) {
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    return false;
  }
  if (!v.isObject()) {
    return false;
  }
  *out_obj = v;
  return true;
}

static Json::Value summarize_tool_output(const std::string& tool_out) {
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
    if (data.isMember("check") && data["check"].isObject() && data["check"].isMember("exit_code")) {
      summary["check_exit_code"] = data["check"]["exit_code"];
    }
    if (data.isMember("apply") && data["apply"].isObject() && data["apply"].isMember("exit_code")) {
      summary["apply_exit_code"] = data["apply"]["exit_code"];
    }
  }
  return summary;
}

static bool build_openai_tools_json(const agent_tool_registry_t* registry, Json::Value* out_tools, std::string* out_error) {
  if (!registry || !out_tools) {
    if (out_error) {
      *out_error = "missing tool registry";
    }
    return false;
  }
  Json::Value tools(Json::arrayValue);
  const size_t n = agent_tool_registry_count(registry);
  for (size_t i = 0; i < n; i++) {
    agent_tool_def_view_t def{};
    if (agent_tool_registry_get(registry, i, &def) != AGENT_OK) {
      continue;
    }
    Json::Value params(Json::objectValue);
    if (!parse_json_object(def.parameters_json ? def.parameters_json : "{}", &params)) {
      if (out_error) {
        *out_error = std::string("invalid parameters_json for tool: ") + (def.name ? def.name : "");
      }
      return false;
    }

    Json::Value t(Json::objectValue);
    t["type"] = "function";
    Json::Value fn(Json::objectValue);
    fn["name"] = def.name ? def.name : "";
    fn["description"] = def.description ? def.description : "";
    fn["parameters"] = params;
    t["function"] = fn;
    tools.append(t);
  }
  *out_tools = tools;
  return true;
}
#endif

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
  if (out_error) {
    out_error->clear();
  }
  if (out_http_status) {
    *out_http_status = 0;
  }
  if (out_http_body) {
    out_http_body->clear();
  }
  if (!out_result) {
    if (out_error) {
      *out_error = "invalid out_result";
    }
    return false;
  }
  *out_result = ToolLoopResult{};

#if !defined(AGENT_HAVE_JSONCPP)
  if (out_error) {
    *out_error = "tool loop requires jsoncpp (AGENT_HAVE_JSONCPP)";
  }
  (void)cfg;
  (void)seed_session;
  (void)user_prompt;
  (void)options;
  return false;
#else
  if (!seed_session) {
    if (out_error) {
      *out_error = "seed session is null";
    }
    return false;
  }
  if (!tools_registry || !executor || !executor->execute) {
    if (out_error) {
      *out_error = "missing tool registry or executor";
    }
    return false;
  }

  Json::Value events(Json::arrayValue);
  bool events_truncated = false;
  size_t events_count = 0;
  size_t captured_bytes = 0;
  uint64_t context_epoch = 0;

  const size_t max_events = 2048;
  const size_t max_total_capture = options.max_capture_bytes == 0 ? (256 * 1024 * 8) : (options.max_capture_bytes * 8);

  auto note_capture = [&](const std::string& s) {
    if (!events_truncated) {
      captured_bytes += s.size();
      if (captured_bytes > max_total_capture) {
        events_truncated = true;
      }
    }
  };

  auto push_event = [&](const std::string& type, const Json::Value& data) {
    if (options.on_event) {
      try {
        const std::string data_json = json_stringify(data);
        options.on_event(options.on_event_ctx, type.c_str(), data_json.c_str());
      } catch (...) {
        // Best-effort: never allow observer failures to break the tool loop.
      }
    }
    if (events_truncated || events_count >= max_events) {
      events_truncated = true;
      return;
    }
    Json::Value e(Json::objectValue);
    e["type"] = type;
    e["data"] = data;
    events.append(e);
    events_count++;
  };

  if (trace_stream) {
    *trace_stream << "=== TOOL LOOP START ===\n";
    *trace_stream << "model=" << cfg.model << " max_steps=" << options.max_steps << "\n";
    *trace_stream << "tools=[";
    const size_t tn = agent_tool_registry_count(tools_registry);
    for (size_t i = 0; i < tn; i++) {
      agent_tool_def_view_t def{};
      if (agent_tool_registry_get(tools_registry, i, &def) != AGENT_OK) {
        continue;
      }
      if (i) *trace_stream << ",";
      *trace_stream << (def.name ? def.name : "");
    }
    *trace_stream << "]\n";
  }

  Json::Value compaction_data;
  Json::Value messages = session_to_compacted_json_messages(seed_session, user_prompt, options, &compaction_data);

  ToolLoopCompactionOptions compact_opt;
  compact_opt.max_chars = options.max_chars;
  compact_opt.keep_last_messages = options.keep_last_messages;
  compact_opt.insert_summary = options.insert_compaction_summary;
  compact_opt.summary_preview_items = options.summary_preview_items;
  compact_opt.summary_snippet_chars = options.summary_snippet_chars;
  compact_opt.summary_max_chars = options.summary_max_chars;

  Json::Value tools(Json::arrayValue);
  std::string tools_err;
  if (!build_openai_tools_json(tools_registry, &tools, &tools_err)) {
    if (out_error) {
      *out_error = tools_err.empty() ? "failed to build tools JSON" : tools_err;
    }
    return false;
  }

  {
    Json::Value d(Json::objectValue);
    d["model"] = cfg.model;
    d["max_steps"] = (Json::UInt64)options.max_steps;
    d["verbose"] = options.verbose;
    d["max_chars"] = (Json::UInt64)(options.max_chars == 0 ? 20000 : options.max_chars);
    d["keep_last_messages"] = (Json::UInt64)(options.keep_last_messages == 0 ? 16 : options.keep_last_messages);
    d["epoch"] = (Json::UInt64)context_epoch;
    push_event("start", d);
  }
  if (compaction_data.isObject()) {
    compaction_data["epoch"] = (Json::UInt64)context_epoch;
    compaction_data["step"] = (Json::UInt64)0;
    push_event("compaction", compaction_data);
  }

  {
    Json::Value um(Json::objectValue);
    um["role"] = "user";
    um["content"] = user_prompt;
    messages.append(um);
  }

  for (size_t step = 0; options.max_steps == 0 || step < options.max_steps; step++) {
    if (options.should_cancel && options.should_cancel(options.should_cancel_ctx)) {
      if (out_error) {
        *out_error = "cancelled";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["reason"] = "cancel_requested";
        push_event("cancelled", d);
      }
      return false;
    }

    {
      ToolLoopCompactionReport rep;
      if (tool_loop_compaction_maybe_compact(&messages, compact_opt, &rep)) {
        Json::Value d(Json::objectValue);
        d["epoch"] = (Json::UInt64)context_epoch;
        d["step"] = (Json::UInt64)step;
        d["before_chars"] = (Json::UInt64)rep.before_chars;
        d["after_chars"] = (Json::UInt64)rep.after_chars;
        d["max_chars"] = (Json::UInt64)(compact_opt.max_chars == 0 ? 20000 : compact_opt.max_chars);
        d["keep_last_messages"] = (Json::UInt64)(compact_opt.keep_last_messages == 0 ? 16 : compact_opt.keep_last_messages);
        d["pinned_system_messages"] = (Json::UInt64)rep.pinned_system_messages;
        d["dropped_messages"] = (Json::UInt64)rep.dropped_messages;
        d["inserted_summary"] = rep.inserted_summary;
        if (options.verbose && rep.inserted_summary && !rep.summary.empty()) {
        bool trunc = false;
        const std::string capped = truncate_str(rep.summary, options.max_capture_bytes, &trunc);
        note_capture(capped);
        d["summary"] = capped;
        d["summary_truncated"] = trunc;
      }
        context_epoch++;
        d["epoch_after"] = (Json::UInt64)context_epoch;
        push_event("compaction", d);
      }
    }

    Json::Value req(Json::objectValue);
    req["model"] = cfg.model;
    req["stream"] = false;
    req["messages"] = messages;
    req["tools"] = tools;

    if (!options.force_tool.empty() && step == 0) {
      Json::Value tc(Json::objectValue);
      tc["type"] = "function";
      Json::Value fn(Json::objectValue);
      fn["name"] = options.force_tool;
      tc["function"] = fn;
      req["tool_choice"] = tc;
    }

    const std::string request_json = json_stringify(req);
    if (trace_stream) {
      *trace_stream << "=== TOOL LOOP STEP " << step << " REQUEST ===\n";
      *trace_stream << request_json << "\n";
    }
    {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)step;
      if (options.verbose) {
        bool trunc = false;
        const std::string capped = truncate_str(request_json, options.max_capture_bytes, &trunc);
        note_capture(capped);
        d["request_json"] = capped;
        d["request_truncated"] = trunc;
      }
      push_event("llm_request", d);
    }

    OpenAIRawResult raw = openai_chat_completions_raw(cfg, request_json);
    // If the request is rejected due to context length, compact more aggressively and retry.
    // This is equivalent to "spawning a new session" on stateless providers: restart with a smaller window + summary.
    for (int retry = 0; retry < 2 && raw.http_status >= 400 && openai_is_context_too_long_error(raw.http_status, raw.response_body); retry++) {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)step;
      d["epoch"] = (Json::UInt64)context_epoch;
      d["http_status"] = (Json::Int64)raw.http_status;
      d["reason"] = "context_too_long_retry";
      push_event("retry", d);

      // Shrink budget and compact in-place, then rebuild request JSON.
      ToolLoopCompactionOptions tighter = compact_opt;
      const size_t cur = tighter.max_chars == 0 ? 20000 : tighter.max_chars;
      tighter.max_chars = std::max<size_t>(2000, (cur * 3) / 4);
      ToolLoopCompactionReport rep;
      if (tool_loop_compaction_maybe_compact(&messages, tighter, &rep)) {
        Json::Value cdata(Json::objectValue);
        cdata["epoch"] = (Json::UInt64)context_epoch;
        cdata["step"] = (Json::UInt64)step;
        cdata["before_chars"] = (Json::UInt64)rep.before_chars;
        cdata["after_chars"] = (Json::UInt64)rep.after_chars;
        cdata["max_chars"] = (Json::UInt64)(tighter.max_chars == 0 ? 20000 : tighter.max_chars);
        cdata["keep_last_messages"] = (Json::UInt64)(tighter.keep_last_messages == 0 ? 16 : tighter.keep_last_messages);
        cdata["pinned_system_messages"] = (Json::UInt64)rep.pinned_system_messages;
        cdata["dropped_messages"] = (Json::UInt64)rep.dropped_messages;
        cdata["inserted_summary"] = rep.inserted_summary;
        if (options.verbose && rep.inserted_summary && !rep.summary.empty()) {
          bool trunc = false;
          const std::string capped = truncate_str(rep.summary, options.max_capture_bytes, &trunc);
          note_capture(capped);
          cdata["summary"] = capped;
          cdata["summary_truncated"] = trunc;
        }
        context_epoch++;
        cdata["epoch_after"] = (Json::UInt64)context_epoch;
        push_event("compaction", cdata);
      }

      req["messages"] = messages;
      const std::string retry_json = json_stringify(req);
      raw = openai_chat_completions_raw(cfg, retry_json);
    }
    if (out_http_status) {
      *out_http_status = raw.http_status;
    }
    if (out_http_body) {
      *out_http_body = raw.response_body;
    }
    if (trace_stream) {
      *trace_stream << "=== TOOL LOOP STEP " << step << " RESPONSE (HTTP " << raw.http_status << ") ===\n";
      *trace_stream << raw.response_body << "\n";
    }
    {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)step;
      d["http_status"] = (Json::Int64)raw.http_status;
      if (options.verbose) {
        bool trunc = false;
        const std::string capped = truncate_str(raw.response_body, options.max_capture_bytes, &trunc);
        note_capture(capped);
        d["response_body"] = capped;
        d["response_truncated"] = trunc;
      }
      push_event("llm_response", d);
    }
    if (raw.http_status < 200 || raw.http_status >= 300) {
      if (out_error) {
        *out_error = openai_format_http_error(raw.http_status, raw.response_body);
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["error"] = out_error ? *out_error : std::string("http error");
        push_event("error", d);
      }
      return false;
    }

    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(raw.response_body);
    Json::Value root;
    if (!Json::parseFromStream(rb, iss, &root, &errs)) {
      if (out_error) {
        *out_error = "failed to parse JSON response";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["error"] = "failed to parse JSON response";
        push_event("error", d);
      }
      return false;
    }

    Json::Value assistant_msg;
    if (!extract_choice0_message(root, &assistant_msg)) {
      if (out_error) {
        *out_error = "missing choices[0].message";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["error"] = "missing choices[0].message";
        push_event("error", d);
      }
      return false;
    }

    out_result->final_assistant_text = extract_assistant_content(assistant_msg);

    const bool has_tools = message_has_tool_calls(assistant_msg);
    if (has_tools) {
      out_result->saw_tool_call = true;
    }

    // Append assistant message as-is (includes tool_calls).
    messages.append(assistant_msg);
    {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)step;
      d["assistant_content"] = out_result->final_assistant_text;
      d["has_tool_calls"] = has_tools;
      if (options.verbose) {
        bool trunc = false;
        const std::string capped = truncate_str(json_stringify(assistant_msg), options.max_capture_bytes, &trunc);
        note_capture(capped);
        d["assistant_message_json"] = capped;
        d["assistant_message_truncated"] = trunc;
      }
      push_event("assistant_message", d);
    }

    if (!has_tools) {
      if (trace_stream) {
        *trace_stream << "=== TOOL LOOP DONE (no tool calls) ===\n";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["reason"] = "no tool calls";
        push_event("done", d);
      }
      break;
    }

    struct ParsedToolCall {
      std::string id;
      std::string name;
      std::string arguments_json;
    };

    std::vector<ParsedToolCall> calls;
    const auto& tool_calls = assistant_msg["tool_calls"];
    if (tool_calls.isArray()) {
      size_t i = 0;
      for (const auto& call : tool_calls) {
        const auto& type = call["type"];
        const auto& fn = call["function"];
        if (!type.isString() || type.asString() != "function") {
          i++;
          continue;
        }
        if (!fn.isObject()) {
          i++;
          continue;
        }
        const auto& namev = fn["name"];
        const auto& argv = fn["arguments"];
        if (!namev.isString() || !argv.isString()) {
          i++;
          continue;
        }
        const auto& idv = call["id"];
        std::string id = idv.isString() ? idv.asString() : (std::string("call_") + std::to_string(step) + "_" + std::to_string(i));
        calls.push_back(ParsedToolCall{id, namev.asString(), argv.asString()});
        i++;
      }
    } else {
      const auto& fc = assistant_msg["function_call"];
      if (fc.isObject()) {
        const auto& namev = fc["name"];
        const auto& argv = fc["arguments"];
        if (namev.isString() && argv.isString()) {
          calls.push_back(ParsedToolCall{std::string("call_") + std::to_string(step) + "_0", namev.asString(), argv.asString()});
        }
      }
    }

    if (calls.empty()) {
      if (out_error) {
        *out_error = "model returned tool calls but none were parseable";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["error"] = "model returned tool calls but none were parseable";
        push_event("error", d);
      }
      return false;
    }

    for (const auto& call : calls) {
      if (options.should_cancel && options.should_cancel(options.should_cancel_ctx)) {
        if (out_error) {
          *out_error = "cancelled";
        }
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["reason"] = "cancel_requested";
        push_event("cancelled", d);
        return false;
      }

      const std::string tool_name = call.name;
      const std::string tool_args_json = call.arguments_json;
      const std::string tool_call_id = call.id;

      if (trace_stream) {
        *trace_stream << "=== TOOL CALL " << tool_name << " ARGS ===\n";
        *trace_stream << tool_args_json << "\n";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["tool_call_id"] = tool_call_id;
        d["tool_name"] = tool_name;
      if (options.verbose) {
        bool trunc = false;
        const std::string capped = truncate_str(tool_args_json, options.max_capture_bytes, &trunc);
        note_capture(capped);
        d["arguments_json"] = capped;
        d["arguments_truncated"] = trunc;
      }
      push_event("tool_call", d);
    }

      agent_string_t tool_result{};
      agent_status_t st = executor->execute(executor->ctx, tool_name.c_str(), tool_args_json.c_str(), &tool_result);
      std::string tool_out;
      if (st == AGENT_OK && tool_result.data) {
        tool_out.assign(tool_result.data, tool_result.len);
      } else {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "tool execution failed";
        o["status"] = (int)st;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        tool_out = Json::writeString(wb, o);
      }
      agent_string_free(&tool_result);

      // Capture tool transcript for host session persistence (portable plain-text record).
      {
        ToolLoopToolRecord rec;
        rec.tool_name = tool_name;
        rec.tool_call_id = tool_call_id;
        rec.arguments_json = tool_args_json;
        rec.result_string = tool_out;
        rec.result_string_for_prompt = tool_loop_cap_tool_output_for_prompt(tool_out, options.max_tool_result_chars, &rec.result_truncated_for_prompt);
        out_result->tool_records.push_back(std::move(rec));
      }

      if (trace_stream) {
        *trace_stream << "=== TOOL CALL " << tool_name << " RESULT ===\n";
        *trace_stream << tool_out << "\n";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["tool_call_id"] = tool_call_id;
        d["tool_name"] = tool_name;
        d["status"] = (Json::Int64)st;
      if (options.verbose) {
        bool trunc = false;
        // Keep tool results JSON-shaped when possible so UIs can render structured views
        // even when event payloads are capped.
        const std::string capped = tool_loop_cap_tool_output_for_prompt(tool_out, options.max_capture_bytes, &trunc);
        note_capture(capped);
        d["content"] = capped;
        d["content_truncated"] = trunc;
      } else {
        d["summary"] = summarize_tool_output(tool_out);
      }
      push_event("tool_result", d);
    }

      Json::Value tm(Json::objectValue);
      tm["role"] = "tool";
      tm["tool_call_id"] = tool_call_id;
      // Avoid blowing up the context window: cap tool results before they enter the prompt.
      tm["content"] = out_result->tool_records.back().result_string_for_prompt;
      messages.append(tm);

      if (options.should_cancel && options.should_cancel(options.should_cancel_ctx)) {
        if (out_error) {
          *out_error = "cancelled";
        }
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["reason"] = "cancel_requested";
        push_event("cancelled", d);
        return false;
      }
    }
  }

  if (options.require_tool_call && !out_result->saw_tool_call) {
    if (out_error) {
      *out_error = "no tool call occurred";
    }
    {
      Json::Value d(Json::objectValue);
      d["error"] = "no tool call occurred";
      push_event("error", d);
    }
    return false;
  }

  if (trace_stream) {
    *trace_stream << "=== TOOL LOOP END ===\n";
  }
  {
    Json::Value d(Json::objectValue);
    d["truncated"] = events_truncated;
    d["captured_bytes"] = (Json::UInt64)captured_bytes;
    d["max_total_capture_bytes"] = (Json::UInt64)max_total_capture;
    d["events_count"] = (Json::UInt64)events_count;
    d["max_events"] = (Json::UInt64)max_events;
    push_event("end", d);
  }

  // Important: do NOT truncate the final JSON string, or it becomes unparsable.
  out_result->events_json = json_stringify(events);
  return true;
#endif
}
