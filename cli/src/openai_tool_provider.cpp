#include "openai_tool_provider.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <vector>
#include <sstream>
#include <cstring>

#include "openai_client.h"

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

static std::string truncate_str(const std::string& s, size_t max_chars, bool* out_truncated = nullptr) {
  if (out_truncated) *out_truncated = false;
  if (max_chars == 0 || s.size() <= max_chars) return s;
  if (out_truncated) *out_truncated = true;
  return s.substr(0, max_chars) + "...(truncated)";
}

static bool parse_json_object(const std::string& s, Json::Value* out_obj) {
  if (!out_obj) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) return false;
  if (!v.isObject()) return false;
  *out_obj = v;
  return true;
}

static bool build_openai_tools_json(const agent_tool_registry_t* registry, Json::Value* out_tools, std::string* out_error) {
  if (!registry || !out_tools) {
    if (out_error) *out_error = "missing tool registry";
    return false;
  }
  Json::Value tools(Json::arrayValue);
  const size_t n = agent_tool_registry_count(registry);
  for (size_t i = 0; i < n; i++) {
    agent_tool_def_view_t def{};
    if (agent_tool_registry_get(registry, i, &def) != AGENT_OK) continue;

    Json::Value params(Json::objectValue);
    if (!parse_json_object(def.parameters_json ? def.parameters_json : "{}", &params)) {
      if (out_error) *out_error = std::string("invalid parameters_json for tool: ") + (def.name ? def.name : "");
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

static bool extract_choice0_message(const Json::Value& root, Json::Value* out_message) {
  if (!out_message) return false;
  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) return false;
  const auto& msg = choices[0]["message"];
  if (!msg.isObject()) return false;
  *out_message = msg;
  return true;
}

static std::string extract_assistant_content(const Json::Value& message) {
  const auto& content = message["content"];
  if (content.isString()) return content.asString();
  return "";
}

static bool message_has_tool_calls(const Json::Value& message) {
  const auto& tc = message["tool_calls"];
  if (tc.isArray() && !tc.empty()) return true;
  const auto& fc = message["function_call"];
  return fc.isObject() && fc.isMember("name") && fc.isMember("arguments");
}

static void provider_emit_event(OpenAIToolProviderCtx* ctx, const char* type, const Json::Value& data) {
  if (!ctx || !ctx->on_event || !type) return;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string s = Json::writeString(wb, data);
  ctx->on_event(ctx->on_event_ctx, type, s.c_str());
}

static void provider_emit_delta(OpenAIToolProviderCtx* ctx, uint64_t step, uint64_t epoch, const std::string& delta) {
  if (!ctx || delta.empty()) return;
  Json::Value d(Json::objectValue);
  d["step"] = (Json::UInt64)step;
  d["epoch"] = (Json::UInt64)epoch;
  d["delta"] = delta;
  provider_emit_event(ctx, "assistant_delta", d);
}

static agent_status_t set_error(agent_tool_provider_response_t* out_resp, const std::string& s) {
  if (!out_resp) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_string_set_copy(&out_resp->error_message, s.c_str(), s.size());
}

static Json::Value build_messages_json(const agent_chat_message_view_t* messages, size_t message_count) {
  Json::Value out(Json::arrayValue);
  for (size_t i = 0; i < message_count; i++) {
    const agent_chat_message_view_t& v = messages[i];
    Json::Value m(Json::objectValue);
    m["role"] = agent_role_to_string(v.role);
    m["content"] = std::string(v.content ? v.content : "", v.content_len);
    if (v.name && v.name[0]) m["name"] = v.name;
    if (v.role == AGENT_ROLE_TOOL && v.tool_call_id && v.tool_call_id[0]) {
      m["tool_call_id"] = v.tool_call_id;
    }
    if (v.role == AGENT_ROLE_ASSISTANT && v.tool_call_count > 0 && v.tool_calls) {
      Json::Value tc(Json::arrayValue);
      for (size_t j = 0; j < v.tool_call_count; j++) {
        const auto& c = v.tool_calls[j];
        Json::Value call(Json::objectValue);
        call["type"] = "function";
        if (c.id && c.id[0]) call["id"] = c.id;
        Json::Value fn(Json::objectValue);
        fn["name"] = c.name ? c.name : "";
        fn["arguments"] = c.arguments_json ? c.arguments_json : "{}";
        call["function"] = fn;
        tc.append(call);
      }
      m["tool_calls"] = tc;
    }
    out.append(m);
  }
  return out;
}

#endif  // AGENT_HAVE_JSONCPP

extern "C" {
static agent_status_t openai_tool_provider_generate(
  void* vctx,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  if (!vctx || !req || !out_resp) return AGENT_ERR_INVALID_ARGUMENT;
  auto* ctx = static_cast<OpenAIToolProviderCtx*>(vctx);

  ctx->last_http_status = 0;
  ctx->last_response_body.clear();
  ctx->last_request_json.clear();

  agent_tool_provider_response_free(out_resp);

#if !defined(AGENT_HAVE_JSONCPP)
  (void)ctx;
  (void)req;
  (void)set_error(out_resp, "tool provider requires jsoncpp (AGENT_HAVE_JSONCPP)");
  return AGENT_ERR_INTERNAL;
#else
  Json::Value tools(Json::arrayValue);
  std::string terr;
  if (!build_openai_tools_json(req->tools, &tools, &terr)) {
    (void)set_error(out_resp, terr.empty() ? "failed to build tools json" : terr);
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  Json::Value root(Json::objectValue);
  root["model"] = req->model ? req->model : "";
  root["stream"] = ctx->stream_assistant;
  root["messages"] = build_messages_json(req->messages, req->message_count);
  root["tools"] = tools;

  if (req->force_tool_or_null && req->force_tool_or_null[0]) {
    Json::Value tc(Json::objectValue);
    tc["type"] = "function";
    Json::Value fn(Json::objectValue);
    fn["name"] = req->force_tool_or_null;
    tc["function"] = fn;
    root["tool_choice"] = tc;
  }

  const std::string request_json = json_stringify(root);
  ctx->last_request_json = request_json;

  {
    Json::Value d(Json::objectValue);
    d["step"] = (Json::UInt64)req->step;
    d["epoch"] = (Json::UInt64)req->epoch;
    if (ctx->verbose_events) {
      bool trunc = false;
      d["request_json"] = truncate_str(request_json, ctx->max_capture_chars, &trunc);
      d["request_truncated"] = trunc;
    }
    provider_emit_event(ctx, "llm_request", d);
  }

  Json::Value parsed;
  bool parsed_is_stream = false;
  std::string assistant_content;

  struct ParsedToolCall { std::string id; std::string name; std::string arguments; };
  std::vector<ParsedToolCall> calls;

  if (ctx->stream_assistant) {
    struct StreamCallAccum {
      std::string id;
      std::string name;
      std::string arguments;
    };
    struct StreamAccum {
      OpenAIToolProviderCtx* ctx = nullptr;
      uint64_t step = 0;
      uint64_t epoch = 0;

      std::string assistant;
      std::string pending_delta;
      std::vector<StreamCallAccum> tool_calls;
      std::string finish_reason;
    } acc;

    acc.ctx = ctx;
    acc.step = req->step;
    acc.epoch = req->epoch;

    auto on_chunk = [](void* vctx, const char* chunk_json, size_t chunk_len) {
      auto* a = static_cast<StreamAccum*>(vctx);
      if (!a || !a->ctx || !chunk_json || chunk_len == 0) return;

      Json::Value root;
      if (!parse_json_object(std::string(chunk_json, chunk_len), &root)) {
        return;
      }
      const auto& choices = root["choices"];
      if (!choices.isArray() || choices.empty()) return;

      const auto& choice0 = choices[0u];
      const auto& fr = choice0["finish_reason"];
      if (fr.isString()) {
        a->finish_reason = fr.asString();
      }

      const auto& delta = choice0["delta"];
      if (!delta.isObject()) return;

      const auto& content = delta["content"];
      if (content.isString()) {
        const std::string d = content.asString();
        if (!d.empty()) {
          a->assistant += d;
          a->pending_delta += d;
          // Coalesce small deltas to avoid flooding event logs/UIs.
          if (a->pending_delta.size() >= 128) {
            provider_emit_delta(a->ctx, a->step, a->epoch, a->pending_delta);
            a->pending_delta.clear();
          }
        }
      }

      const auto& tc = delta["tool_calls"];
      if (tc.isArray()) {
        for (Json::ArrayIndex i = 0; i < tc.size(); i++) {
          const auto& call = tc[i];
          if (!call.isObject()) continue;

          int index = -1;
          const auto& iv = call["index"];
          if (iv.isInt()) {
            index = iv.asInt();
          } else {
            index = (int)i;
          }
          if (index < 0) continue;
          if ((size_t)index >= a->tool_calls.size()) {
            a->tool_calls.resize((size_t)index + 1);
          }

          auto& dst = a->tool_calls[(size_t)index];
          const auto& idv = call["id"];
          if (idv.isString()) dst.id = idv.asString();

          const auto& fn = call["function"];
          if (fn.isObject()) {
            const auto& namev = fn["name"];
            if (namev.isString()) dst.name = namev.asString();
            const auto& argv = fn["arguments"];
            if (argv.isString()) dst.arguments += argv.asString();
          }
        }
      }
    };

    const auto stream_fn = ctx->chat_stream_fn ? ctx->chat_stream_fn : openai_chat_completions_raw_stream;
    OpenAIStreamResult sr = stream_fn(ctx->cfg, request_json, on_chunk, &acc, ctx->max_capture_chars);
    ctx->last_http_status = sr.http_status;
    ctx->last_response_body = sr.response_body;

    {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)req->step;
      d["epoch"] = (Json::UInt64)req->epoch;
      d["http_status"] = (Json::Int64)sr.http_status;
      d["stream"] = true;
      d["saw_done"] = sr.saw_done;
      if (ctx->verbose_events) {
        bool trunc = false;
        d["response_body"] = truncate_str(sr.response_body, ctx->max_capture_chars, &trunc);
        d["response_truncated"] = trunc;
      }
      provider_emit_event(ctx, "llm_response", d);
    }

    // Flush any pending delta.
    if (!acc.pending_delta.empty()) {
      provider_emit_delta(ctx, req->step, req->epoch, acc.pending_delta);
      acc.pending_delta.clear();
    }

    if (sr.http_status < 200 || sr.http_status >= 300) {
      if (openai_is_context_too_long_error(sr.http_status, sr.response_body)) {
        (void)set_error(out_resp, openai_format_http_error(sr.http_status, sr.response_body));
        return AGENT_ERR_CONTEXT_TOO_LONG;
      }
      (void)set_error(out_resp, openai_format_http_error(sr.http_status, sr.response_body));
      return AGENT_ERR_INTERNAL;
    }

    // Some providers ignore `stream: true` and return a normal JSON response.
    // If we didn't decode any deltas/tool_calls, fall back to parsing the raw body.
    bool decoded_any = (!acc.assistant.empty());
    for (const auto& c : acc.tool_calls) {
      if (!c.name.empty()) {
        decoded_any = true;
        break;
      }
    }
    if (!decoded_any && !sr.saw_done && !sr.response_body.empty() && sr.response_body.size() < (4u * 1024u * 1024u) &&
        sr.response_body[0] == '{') {
      if (!parse_json_object(sr.response_body, &parsed)) {
        (void)set_error(out_resp, "failed to parse JSON response (stream fallback)");
        return AGENT_ERR_INTERNAL;
      }
    } else {
      parsed_is_stream = true;
      assistant_content = acc.assistant;
      for (size_t i = 0; i < acc.tool_calls.size(); i++) {
        const auto& c = acc.tool_calls[i];
        if (c.name.empty()) continue;
        const std::string id = c.id.empty() ? (std::string("call_") + std::to_string(req->step) + "_" + std::to_string(i)) : c.id;
        calls.push_back(ParsedToolCall{id, c.name, c.arguments.empty() ? "{}" : c.arguments});
      }
    }
  } else {
    const auto raw_fn = ctx->chat_raw_fn ? ctx->chat_raw_fn : openai_chat_completions_raw;
    OpenAIRawResult raw = raw_fn(ctx->cfg, request_json);
    ctx->last_http_status = raw.http_status;
    ctx->last_response_body = raw.response_body;

    {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)req->step;
      d["epoch"] = (Json::UInt64)req->epoch;
      d["http_status"] = (Json::Int64)raw.http_status;
      d["stream"] = false;
      if (ctx->verbose_events) {
        bool trunc = false;
        d["response_body"] = truncate_str(raw.response_body, ctx->max_capture_chars, &trunc);
        d["response_truncated"] = trunc;
      }
      provider_emit_event(ctx, "llm_response", d);
    }

    if (raw.http_status < 200 || raw.http_status >= 300) {
      if (openai_is_context_too_long_error(raw.http_status, raw.response_body)) {
        (void)set_error(out_resp, openai_format_http_error(raw.http_status, raw.response_body));
        return AGENT_ERR_CONTEXT_TOO_LONG;
      }
      (void)set_error(out_resp, openai_format_http_error(raw.http_status, raw.response_body));
      return AGENT_ERR_INTERNAL;
    }

    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(raw.response_body);
    if (!Json::parseFromStream(rb, iss, &parsed, &errs) || !parsed.isObject()) {
      (void)set_error(out_resp, "failed to parse JSON response");
      return AGENT_ERR_INTERNAL;
    }
  }

  if (!parsed_is_stream) {
    Json::Value assistant_msg;
    if (!extract_choice0_message(parsed, &assistant_msg)) {
      (void)set_error(out_resp, "missing choices[0].message");
      return AGENT_ERR_INTERNAL;
    }

    assistant_content = extract_assistant_content(assistant_msg);

    if (message_has_tool_calls(assistant_msg)) {
      const auto& tool_calls = assistant_msg["tool_calls"];
      if (tool_calls.isArray()) {
        size_t idx = 0;
        for (const auto& call : tool_calls) {
          const auto& type = call["type"];
          const auto& fn = call["function"];
          if (!type.isString() || type.asString() != "function") { idx++; continue; }
          if (!fn.isObject()) { idx++; continue; }
          const auto& namev = fn["name"];
          const auto& argv = fn["arguments"];
          if (!namev.isString() || !argv.isString()) { idx++; continue; }
          const auto& idv = call["id"];
          std::string id = idv.isString() ? idv.asString() : (std::string("call_") + std::to_string(req->step) + "_" + std::to_string(idx));
          calls.push_back(ParsedToolCall{id, namev.asString(), argv.asString()});
          idx++;
        }
      } else {
        // Legacy/alternate schema: {"function_call":{"name":"...","arguments":"..."}}
        const auto& fc = assistant_msg["function_call"];
        if (fc.isObject()) {
          const auto& namev = fc["name"];
          const auto& argv = fc["arguments"];
          if (namev.isString() && argv.isString()) {
            calls.push_back(ParsedToolCall{std::string("call_") + std::to_string(req->step) + "_0", namev.asString(), argv.asString()});
          }
        }
      }
    }
  }

  if (agent_string_set_copy(&out_resp->assistant_content, assistant_content.c_str(), assistant_content.size()) != AGENT_OK) {
    return AGENT_ERR_OOM;
  }

  if (calls.empty()) {
    return AGENT_OK;
  }

  out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(calls.size() * sizeof(agent_tool_call_t));
  if (!out_resp->tool_calls) return AGENT_ERR_OOM;
  memset(out_resp->tool_calls, 0, calls.size() * sizeof(agent_tool_call_t));
  out_resp->tool_call_count = calls.size();
  for (size_t i = 0; i < calls.size(); i++) {
    const auto& c = calls[i];
    if (agent_string_set_copy(&out_resp->tool_calls[i].id, c.id.c_str(), c.id.size()) != AGENT_OK) return AGENT_ERR_OOM;
    if (agent_string_set_copy(&out_resp->tool_calls[i].name, c.name.c_str(), c.name.size()) != AGENT_OK) return AGENT_ERR_OOM;
    if (agent_string_set_copy(&out_resp->tool_calls[i].arguments_json, c.arguments.c_str(), c.arguments.size()) != AGENT_OK) return AGENT_ERR_OOM;
  }

  return AGENT_OK;
#endif
}
} // extern "C"

agent_tool_provider_t openai_make_tool_provider(OpenAIToolProviderCtx* ctx) {
  agent_tool_provider_t p{};
  p.ctx = ctx;
  p.generate = openai_tool_provider_generate;
  return p;
}
