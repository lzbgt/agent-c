#include "openai_tool_provider.h"

#include "agent/agent.h"
#include "openai_stream_adapter.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <vector>
#include <sstream>
#include <cstring>
#include <unordered_map>

#include "openai_client.h"

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

static std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static bool url_contains_ci(const std::string& url, const std::string& needle) {
  if (needle.empty()) return false;
  const std::string u = lower_copy(url);
  const std::string n = lower_copy(needle);
  return u.find(n) != std::string::npos;
}

static bool is_moonshot_provider(const std::string& base_url) {
  // Moonshot endpoints include:
  // - https://api.moonshot.ai/v1
  // - https://api.moonshot.cn/v1
  return url_contains_ci(base_url, "moonshot");
}

static bool is_deepseek_provider(const std::string& base_url) {
  return url_contains_ci(base_url, "deepseek");
}

static bool provider_rejects_image_parts(const std::string& base_url, const std::string& model) {
  // Some OpenAI-compatible providers only support `content` parts of `{"type":"text","text":"..."}` and will
  // reject `{"type":"image_url",...}` with a 400 deserialization error.
  //
  // Keep this heuristic conservative and prefer correctness over vision in tool-calling flows:
  // - Moonshot/Kimi tool-call endpoints reject image parts.
  // - DeepSeek's API schema expects only `text` parts (even when content is an array).
  (void)model;
  if (is_moonshot_provider(base_url)) return true;
  if (is_deepseek_provider(base_url)) return true;
  return false;
}

static const char* kMultimodalPrefix = "__AGENT_MM_V1__";

static bool try_parse_multimodal_prefix(
  const std::string& content,
  std::string* out_text,
  Json::Value* out_mm
) {
  if (out_text) *out_text = content;
  if (out_mm) *out_mm = Json::Value(Json::nullValue);
  if (!out_text || !out_mm) return false;

  if (content.rfind(kMultimodalPrefix, 0) != 0) return false;
  const size_t nl = content.find('\n');
  if (nl == std::string::npos) return false;

  const std::string json_part = content.substr(std::strlen(kMultimodalPrefix), nl - std::strlen(kMultimodalPrefix));
  if (json_part.empty()) return false;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(json_part);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) {
    return false;
  }

  *out_mm = v;
  *out_text = content.substr(nl + 1);
  return true;
}

static Json::Value multimodal_content_from_parts(const std::string& text, const Json::Value& mm, bool allow_image_parts) {
  // Expected `mm` shape:
  //   { "images":[{"mime":"image/png","b64":"...","name":"..."}],
  //     "files":[{"mime":"text/plain","name":"...","text":"...","truncated":true}] }
  //
  // This is *host-internal* and intentionally minimal; callers should not treat it as public API.
  const bool have_images = mm.isMember("images") && mm["images"].isArray() && !mm["images"].empty();
  const bool have_files = mm.isMember("files") && mm["files"].isArray() && !mm["files"].empty();
  if (!have_images && !have_files) {
    return Json::Value(text);
  }

  Json::Value arr(Json::arrayValue);
  if (!text.empty()) {
    Json::Value t(Json::objectValue);
    t["type"] = "text";
    t["text"] = text;
    arr.append(t);
  }

  if (have_files) {
    for (const auto& f : mm["files"]) {
      if (!f.isObject()) continue;
      const std::string name = f.isMember("name") && f["name"].isString() ? f["name"].asString() : "";
      const std::string mime = f.isMember("mime") && f["mime"].isString() ? f["mime"].asString() : "";
      const std::string ft = f.isMember("text") && f["text"].isString() ? f["text"].asString() : "";
      const bool trunc = f.isMember("truncated") && f["truncated"].isBool() ? f["truncated"].asBool() : false;
      if (ft.empty()) continue;
      std::string block;
      block += "[Attachment";
      if (!name.empty()) block += ": " + name;
      if (!mime.empty()) block += " (" + mime + ")";
      block += "]\n";
      block += ft;
      if (trunc) block += "\n...(truncated)";

      Json::Value t(Json::objectValue);
      t["type"] = "text";
      t["text"] = block;
      arr.append(t);
    }
  }

  if (have_images) {
    for (const auto& im : mm["images"]) {
      if (!im.isObject()) continue;
      const std::string name = im.isMember("name") && im["name"].isString() ? im["name"].asString() : "";
      const std::string mime = im.isMember("mime") && im["mime"].isString() ? im["mime"].asString() : "image/png";
      const std::string b64 = im.isMember("b64") && im["b64"].isString() ? im["b64"].asString() : "";
      if (b64.empty()) continue;

      if (allow_image_parts) {
        const std::string url = std::string("data:") + mime + ";base64," + b64;
        Json::Value part(Json::objectValue);
        part["type"] = "image_url";
        Json::Value iu(Json::objectValue);
        iu["url"] = url;
        part["image_url"] = iu;
        arr.append(part);
      } else {
        // Moonshot/Kimi tool-call endpoints reject non-text content part variants (e.g. `image_url`).
        // Keep attachments discoverable without sending image content.
        std::string hint;
        hint += "[Image attachment";
        if (!name.empty()) hint += ": " + name;
        if (!mime.empty()) hint += " (" + mime + ")";
        hint += "]\n";
        hint += "(Image omitted: provider does not accept image parts when tools are enabled.)";
        Json::Value part(Json::objectValue);
        part["type"] = "text";
        part["text"] = hint;
        arr.append(part);
      }
    }
  }

  return arr;
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

static bool try_extract_usage_tokens(const Json::Value& root, Json::Value* out_usage_obj) {
  if (out_usage_obj) *out_usage_obj = Json::Value(Json::nullValue);
  if (!out_usage_obj) return false;
  if (!root.isObject()) return false;
  if (!root.isMember("usage") || !root["usage"].isObject()) return false;
  const Json::Value u = root["usage"];
  auto get_i64_nonneg = [&](const char* k, int64_t* out_v) -> bool {
    if (!out_v || !k) return false;
    *out_v = 0;
    if (!u.isMember(k)) return false;
    const Json::Value& v = u[k];
    if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return false;
    const int64_t n = v.asInt64();
    if (n < 0) return false;
    *out_v = n;
    return true;
  };

  int64_t prompt = 0;
  int64_t completion = 0;
  int64_t total = 0;
  const bool have_prompt = get_i64_nonneg("prompt_tokens", &prompt);
  const bool have_completion = get_i64_nonneg("completion_tokens", &completion);
  const bool have_total = get_i64_nonneg("total_tokens", &total);
  if (!have_prompt && !have_completion && !have_total) return false;
  if (total <= 0 && (prompt > 0 || completion > 0)) total = prompt + completion;

  Json::Value out(Json::objectValue);
  if (have_prompt) out["prompt_tokens"] = (Json::Int64)prompt;
  if (have_completion) out["completion_tokens"] = (Json::Int64)completion;
  out["total_tokens"] = (Json::Int64)total;
  *out_usage_obj = out;
  return true;
}

static void free_tool_call_array(agent_tool_call_t* calls, size_t count) {
  if (!calls) return;
  for (size_t i = 0; i < count; i++) {
    agent_string_free(&calls[i].id);
    agent_string_free(&calls[i].name);
    agent_string_free(&calls[i].arguments_json);
  }
  agent_free(calls);
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

static Json::Value build_messages_json(
  OpenAIToolProviderCtx* ctx,
  const agent_chat_message_view_t* messages,
  size_t message_count,
  bool include_reasoning_content,
  bool allow_image_parts
) {
  Json::Value out(Json::arrayValue);
  for (size_t i = 0; i < message_count; i++) {
    const agent_chat_message_view_t& v = messages[i];
    Json::Value m(Json::objectValue);
    m["role"] = agent_role_to_string(v.role);
    const std::string raw(v.content ? v.content : "", v.content_len);
    std::string text = raw;
    Json::Value mm(Json::nullValue);
    (void)try_parse_multimodal_prefix(raw, &text, &mm);
    if (mm.isObject()) {
      m["content"] = multimodal_content_from_parts(text, mm, allow_image_parts);
    } else {
      m["content"] = text;
    }
    // DeepSeek thinking-mode models (e.g. deepseek-reasoner) require a `reasoning_content` field
    // in assistant messages for tool-calling flows. If omitted, DeepSeek may return HTTP 400.
    if (include_reasoning_content && v.role == AGENT_ROLE_ASSISTANT) {
      std::string rc;
      if (ctx && v.tool_call_count > 0 && v.tool_calls) {
        std::string key;
        for (size_t j = 0; j < v.tool_call_count; j++) {
          const auto& c = v.tool_calls[j];
          if (c.id && c.id[0]) {
            if (!key.empty()) key += "|";
            key += c.id;
          }
        }
        if (!key.empty()) {
          const auto it = ctx->reasoning_by_tool_call_ids.find(key);
          if (it != ctx->reasoning_by_tool_call_ids.end()) rc = it->second;
        }
      }
      m["reasoning_content"] = rc;
    }
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
  const bool deepseek_provider = is_deepseek_provider(ctx->cfg.base_url);
  const bool moonshot = is_moonshot_provider(ctx->cfg.base_url);
  const bool include_reasoning = deepseek_provider || moonshot;
  const bool use_stream_assistant = ctx->stream_assistant && !moonshot;
  if (moonshot && ctx->stream_assistant) {
    // Kimi K2.5 tool-use constraints + requirements make streaming tricky:
    // - reasoning_content must be carried forward across tool-calling turns
    // - our streaming decoder does not currently capture reasoning deltas
    //
    // Use non-streaming calls for correctness.
  }
  root["stream"] = use_stream_assistant;
  if (ctx->cfg.max_completion_tokens > 0) {
    root["max_completion_tokens"] = (Json::Int64)ctx->cfg.max_completion_tokens;
  } else if (ctx->cfg.max_tokens > 0) {
    root["max_tokens"] = (Json::Int64)ctx->cfg.max_tokens;
  }
  const bool want_stream_usage = use_stream_assistant;
  if (want_stream_usage) {
    // Best-effort token accounting for streaming calls:
    // - OpenAI-compatible APIs can include `usage` in the final stream chunk when requested.
    // - Some providers may reject unknown keys; we handle that by retrying once without stream_options.
    Json::Value so(Json::objectValue);
    so["include_usage"] = true;
    root["stream_options"] = so;
  }
  const bool allow_image_parts = !provider_rejects_image_parts(ctx->cfg.base_url, root["model"].asString());
  root["messages"] = build_messages_json(ctx, req->messages, req->message_count, include_reasoning, allow_image_parts);
  root["tools"] = tools;

  if (req->force_tool_or_null && req->force_tool_or_null[0]) {
    // Some providers (DeepSeek thinking mode, Kimi thinking mode) do not support forcing tool_choice.
    if (!deepseek_provider && !moonshot) {
      Json::Value tc(Json::objectValue);
      tc["type"] = "function";
      Json::Value fn(Json::objectValue);
      fn["name"] = req->force_tool_or_null;
      tc["function"] = fn;
      root["tool_choice"] = tc;
    }
  }

  const std::string request_json = json_stringify(root);
  ctx->last_request_json = request_json;

  {
    Json::Value d(Json::objectValue);
    d["step"] = (Json::UInt64)req->step;
    d["epoch"] = (Json::UInt64)req->epoch;
    if (ctx->verbose_events) {
      bool trunc = false;
      d["request_json"] = truncate_str(request_json, ctx->max_event_chars, &trunc);
      d["request_truncated"] = trunc;
    }
    provider_emit_event(ctx, "llm_request", d);
  }

  Json::Value parsed;
  bool parsed_is_stream = false;
  std::string assistant_content;

  struct ParsedToolCall { std::string id; std::string name; std::string arguments; };
  std::vector<ParsedToolCall> calls;

  if (use_stream_assistant) {
    struct StreamAccum {
      OpenAIToolProviderCtx* ctx = nullptr;
      OpenAIStreamCoreAdapter core;
    } acc;

    acc.ctx = ctx;
    OpenAIStreamCoreConfig scfg{};
    scfg.max_tool_calls_total = 0;
    scfg.max_tool_call_args_chars = 0;
    scfg.max_events_per_feed = 0;
    scfg.delta_flush_bytes = 128;
    scfg.step = req->step;
    scfg.epoch = req->epoch;

    auto on_delta = [](void* vctx, const char* delta, size_t delta_len, uint64_t step, uint64_t epoch) {
      auto* a = static_cast<StreamAccum*>(vctx);
      if (!a || !a->ctx || !delta || delta_len == 0) return;
      provider_emit_delta(a->ctx, step, epoch, std::string(delta, delta_len));
    };

    openai_stream_core_init(&acc.core, &scfg, on_delta, &acc);

    auto on_chunk = [](void* vctx, const char* chunk_json, size_t chunk_len) {
      auto* a = static_cast<StreamAccum*>(vctx);
      if (!a || !chunk_json || chunk_len == 0) return;
      (void)openai_stream_core_feed_chunk(&a->core, chunk_json, chunk_len);
    };

    const auto stream_fn = ctx->chat_stream_fn ? ctx->chat_stream_fn : openai_chat_completions_raw_stream;
    OpenAIStreamResult sr = stream_fn(ctx->cfg, request_json, on_chunk, &acc, ctx->max_capture_bytes);
    if (want_stream_usage && (sr.http_status < 200 || sr.http_status >= 300) &&
        (!acc.core.saw_delta && acc.core.dec.tool_call_count == 0)) {
      // Best-effort compatibility: some OpenAI-compatible providers reject stream_options.
      // If we didn't decode any deltas/tool_calls, retry once without stream_options.
      bool decoded_any = false;
      if (acc.core.saw_delta) decoded_any = true;
      for (size_t i = 0; !decoded_any && i < acc.core.dec.tool_call_count; i++) {
        const auto& c = acc.core.dec.tool_calls[i];
        if (c.name.data && c.name.len) decoded_any = true;
      }
      if (!decoded_any) {
        const std::string msg =
          (!sr.error_message.empty() ? sr.error_message : openai_format_http_error(sr.http_status, sr.response_body));
        const std::string m = lower_copy(msg);
        if (m.find("stream_options") != std::string::npos || m.find("include_usage") != std::string::npos ||
            m.find("unknown field") != std::string::npos || m.find("unrecognized field") != std::string::npos) {
          Json::Value d(Json::objectValue);
          d["step"] = (Json::UInt64)req->step;
          d["epoch"] = (Json::UInt64)req->epoch;
          d["http_status"] = (Json::Int64)sr.http_status;
          d["stream"] = true;
          d["scope"] = "provider";
          d["reason"] = "stream_options_rejected";
          d["will_retry"] = true;
          provider_emit_event(ctx, "retry", d);

          Json::Value root2 = root;
          root2.removeMember("stream_options");
          const std::string req2 = json_stringify(root2);
          ctx->last_request_json = req2;
          openai_stream_core_reset(&acc.core);
          sr = stream_fn(ctx->cfg, req2, on_chunk, &acc, ctx->max_capture_bytes);
        }
      }
    }
    openai_stream_core_flush(&acc.core);
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
        d["response_body"] = truncate_str(sr.response_body, ctx->max_event_chars, &trunc);
        d["response_truncated"] = trunc;
      }
      provider_emit_event(ctx, "llm_response", d);
    }

    // Flush any pending delta.
    if (sr.http_status < 200 || sr.http_status >= 300) {
      if (openai_is_context_too_long_error(sr.http_status, sr.response_body)) {
        const std::string msg = (!sr.error_message.empty() ? sr.error_message : openai_format_http_error(sr.http_status, sr.response_body));
        (void)set_error(out_resp, msg);
        openai_stream_core_free(&acc.core);
        return AGENT_ERR_CONTEXT_TOO_LONG;
      }
      const std::string msg = (!sr.error_message.empty() ? sr.error_message : openai_format_http_error(sr.http_status, sr.response_body));
      (void)set_error(out_resp, msg);
      openai_stream_core_free(&acc.core);
      return AGENT_ERR_INTERNAL;
    }

    // Some providers ignore `stream: true` and return a normal JSON response.
    // If we didn't decode any deltas/tool_calls, fall back to parsing the raw body.
    bool decoded_any = acc.core.saw_delta != 0;
    for (size_t i = 0; !decoded_any && i < acc.core.dec.tool_call_count; i++) {
      const auto& c = acc.core.dec.tool_calls[i];
      if (c.name.data && c.name.len) decoded_any = true;
    }
    if (!decoded_any && !sr.saw_done && !sr.response_body.empty() && sr.response_body.size() < (4u * 1024u * 1024u) &&
        sr.response_body[0] == '{') {
      if (!parse_json_object(sr.response_body, &parsed)) {
        openai_stream_core_free(&acc.core);
        (void)set_error(out_resp, "failed to parse JSON response (stream fallback)");
        return AGENT_ERR_INTERNAL;
      }
    } else {
      parsed_is_stream = true;
      if (acc.core.assistant.data && acc.core.assistant.len) {
        assistant_content.assign(acc.core.assistant.data, acc.core.assistant.len);
      }
      agent_tool_call_t* stream_calls = NULL;
      size_t stream_call_count = 0;
      agent_status_t cst = agent_stream_decoder_copy_tool_calls(&acc.core.dec, &stream_calls, &stream_call_count);
      if (cst == AGENT_OK && stream_calls) {
        size_t idx = 0;
        for (size_t i = 0; i < stream_call_count; i++) {
          const auto& c = stream_calls[i];
          if (!c.name.data || c.name.len == 0) continue;
          const std::string name(c.name.data, c.name.len);
          const std::string args = (c.arguments_json.data && c.arguments_json.len)
            ? std::string(c.arguments_json.data, c.arguments_json.len)
            : std::string("{}");
          const std::string id = (c.id.data && c.id.len)
            ? std::string(c.id.data, c.id.len)
            : (std::string("call_") + std::to_string(req->step) + "_" + std::to_string(idx));
          calls.push_back(ParsedToolCall{id, name, args});
          idx++;
        }
      }
      free_tool_call_array(stream_calls, stream_call_count);
    }

    if (parsed_is_stream && acc.core.has_usage) {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)req->step;
      d["epoch"] = (Json::UInt64)req->epoch;
      Json::Value usage(Json::objectValue);
      usage["prompt_tokens"] = (Json::Int64)acc.core.prompt_tokens;
      usage["completion_tokens"] = (Json::Int64)acc.core.completion_tokens;
      usage["total_tokens"] = (Json::Int64)acc.core.total_tokens;
      d["prompt_tokens"] = usage["prompt_tokens"];
      d["completion_tokens"] = usage["completion_tokens"];
      d["total_tokens"] = usage["total_tokens"];
      d["usage"] = usage;
      provider_emit_event(ctx, "llm_usage", d);
    }

    openai_stream_core_free(&acc.core);
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
        d["response_body"] = truncate_str(raw.response_body, ctx->max_event_chars, &trunc);
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
    Json::Value usage(Json::nullValue);
    if (try_extract_usage_tokens(parsed, &usage) && usage.isObject()) {
      Json::Value d(Json::objectValue);
      d["step"] = (Json::UInt64)req->step;
      d["epoch"] = (Json::UInt64)req->epoch;
      if (usage.isMember("prompt_tokens")) d["prompt_tokens"] = usage["prompt_tokens"];
      if (usage.isMember("completion_tokens")) d["completion_tokens"] = usage["completion_tokens"];
      if (usage.isMember("total_tokens")) d["total_tokens"] = usage["total_tokens"];
      d["usage"] = usage;
      provider_emit_event(ctx, "llm_usage", d);
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

    // Best-effort reasoning content capture for providers that require it across tool-call turns.
    if (include_reasoning && assistant_msg.isMember("reasoning_content") && assistant_msg["reasoning_content"].isString()) {
      const std::string rc = assistant_msg["reasoning_content"].asString();
      if (!rc.empty() && !calls.empty()) {
        std::string key;
        for (const auto& c : calls) {
          if (!c.id.empty()) {
            if (!key.empty()) key += "|";
            key += c.id;
          }
        }
        if (!key.empty()) {
          ctx->reasoning_by_tool_call_ids[key] = rc;
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
