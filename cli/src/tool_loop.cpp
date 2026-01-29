#include "tool_loop.h"

#include "agent/agent.h"
#include "agent/tools.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <ostream>
#include <sstream>

#include "openai_client.h"

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

static std::string truncate_str(const std::string& s, size_t max_bytes) {
  if (max_bytes == 0 || s.size() <= max_bytes) {
    return s;
  }
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
  return tc.isArray() && !tc.empty();
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
  auto push_event = [&](const std::string& type, const Json::Value& data) {
    Json::Value e(Json::objectValue);
    e["type"] = type;
    e["data"] = data;
    events.append(e);
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

  Json::Value messages = session_to_json_messages(seed_session);
  {
    Json::Value um(Json::objectValue);
    um["role"] = "user";
    um["content"] = user_prompt;
    messages.append(um);
  }

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
    push_event("start", d);
  }

  for (size_t step = 0; options.max_steps == 0 || step < options.max_steps; step++) {
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
        d["request_json"] = truncate_str(request_json, options.max_capture_bytes);
      }
      push_event("llm_request", d);
    }

    const OpenAIRawResult raw = openai_chat_completions_raw(cfg, request_json);
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
        d["response_body"] = truncate_str(raw.response_body, options.max_capture_bytes);
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
        d["assistant_message_json"] = truncate_str(json_stringify(assistant_msg), options.max_capture_bytes);
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

    const auto& tool_calls = assistant_msg["tool_calls"];
    for (const auto& call : tool_calls) {
      const auto& type = call["type"];
      const auto& idv = call["id"];
      const auto& fn = call["function"];
      if (!type.isString() || type.asString() != "function") {
        continue;
      }
      if (!idv.isString() || !fn.isObject()) {
        continue;
      }
      const auto& namev = fn["name"];
      const auto& argv = fn["arguments"];
      if (!namev.isString() || !argv.isString()) {
        continue;
      }
      const std::string tool_name = namev.asString();
      const std::string tool_args_json = argv.asString();

      if (trace_stream) {
        *trace_stream << "=== TOOL CALL " << tool_name << " ARGS ===\n";
        *trace_stream << tool_args_json << "\n";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["tool_call_id"] = idv.asString();
        d["tool_name"] = tool_name;
        if (options.verbose) {
          d["arguments_json"] = truncate_str(tool_args_json, options.max_capture_bytes);
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

      if (trace_stream) {
        *trace_stream << "=== TOOL CALL " << tool_name << " RESULT ===\n";
        *trace_stream << tool_out << "\n";
      }
      {
        Json::Value d(Json::objectValue);
        d["step"] = (Json::UInt64)step;
        d["tool_call_id"] = idv.asString();
        d["tool_name"] = tool_name;
        d["status"] = (Json::Int64)st;
        if (options.verbose) {
          d["content"] = truncate_str(tool_out, options.max_capture_bytes);
        } else {
          d["summary"] = summarize_tool_output(tool_out);
        }
        push_event("tool_result", d);
      }

      Json::Value tm(Json::objectValue);
      tm["role"] = "tool";
      tm["tool_call_id"] = idv.asString();
      tm["content"] = tool_out;
      messages.append(tm);
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
  push_event("end", Json::Value(Json::objectValue));

  out_result->events_json = truncate_str(json_stringify(events), options.max_capture_bytes * 8);
  return true;
#endif
}
