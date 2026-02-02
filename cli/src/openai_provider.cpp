#include "openai_provider.h"

#include "agent/agent.h"
#include "agent/parts.h"

#include "base64.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <sstream>

agent_provider_t openai_make_provider(OpenAIProviderCtx* ctx) {
  agent_provider_t p{};
  p.ctx = ctx;
  p.generate = openai_provider_generate;
  p.generate_ex = openai_provider_generate_ex;
  return p;
}

agent_status_t openai_provider_generate(
  void* provider_ctx,
  const agent_generate_request_t* req,
  agent_generate_response_t* out_resp
) {
  if (!provider_ctx || !req || !out_resp) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  auto* ctx = static_cast<OpenAIProviderCtx*>(provider_ctx);
  ctx->last_http_status = 0;
  ctx->last_body.clear();
  ctx->last_request_body.clear();
  ctx->last_error.clear();

  OpenAIClientConfig cfg = ctx->cfg;
  if (req->model && req->model[0]) {
    cfg.model = req->model;
  }

#if defined(AGENT_HAVE_JSONCPP)
  {
    Json::Value root(Json::objectValue);
    root["model"] = cfg.model;
    root["stream"] = false;
    Json::Value messages(Json::arrayValue);
    for (size_t i = 0; i < req->message_count; i++) {
      Json::Value m(Json::objectValue);
      m["role"] = agent_role_to_string(req->messages[i].role);
      m["content"] = std::string(req->messages[i].content, req->messages[i].content_len);
      messages.append(m);
    }
    root["messages"] = messages;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    ctx->last_request_body = Json::writeString(wb, root);
  }
#endif

  const auto chat_fn = ctx->chat_fn ? ctx->chat_fn : openai_chat_completions;
  const OpenAIChatResult r = chat_fn(cfg, req->messages, req->message_count);
  ctx->last_http_status = r.http_status;
  ctx->last_body = r.response_body;
  ctx->last_error = r.error_message;

  if (r.http_status < 200 || r.http_status >= 300) {
    if (ctx->last_error.empty()) {
      ctx->last_error = openai_format_http_error(r.http_status, r.response_body);
    }
    if (openai_is_context_too_long_error(r.http_status, r.response_body)) {
      return AGENT_ERR_CONTEXT_TOO_LONG;
    }
    return AGENT_ERR_INTERNAL;
  }
  if (r.assistant_text.empty()) {
    // If parsing failed, treat as error (caller can inspect last_body).
    if (ctx->last_error.empty()) {
      ctx->last_error = "failed to extract assistant text from response";
    }
    return AGENT_ERR_INTERNAL;
  }
  return agent_string_set_copy(&out_resp->assistant_text, r.assistant_text.c_str(), r.assistant_text.size());
}

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

static std::string json_try_extract_assistant_text(const Json::Value& root) {
  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) return "";
  const auto& msg = choices[0]["message"];
  const auto& content = msg["content"];
  if (content.isString()) return content.asString();
  const auto& text = choices[0]["text"];
  if (text.isString()) return text.asString();
  return "";
}
#endif

agent_status_t openai_provider_generate_ex(
  void* provider_ctx,
  const agent_generate_request_ex_t* req,
  agent_generate_response_t* out_resp
) {
  if (!provider_ctx || !req || !out_resp || !req->session || !req->messages) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  auto* ctx = static_cast<OpenAIProviderCtx*>(provider_ctx);
  ctx->last_http_status = 0;
  ctx->last_body.clear();
  ctx->last_request_body.clear();
  ctx->last_error.clear();

  // If tests injected a chat_fn (message_view based), fall back to text-only behavior.
  // The host raw client path below is needed for multimodal JSON.
  if (ctx->chat_fn) {
    agent_generate_request_t r;
    r.model = req->model;
    r.messages = req->messages;
    r.message_count = req->message_count;
    return openai_provider_generate(provider_ctx, &r, out_resp);
  }

#if !defined(AGENT_HAVE_JSONCPP)
  (void)ctx;
  return AGENT_ERR_INTERNAL;
#else
  OpenAIClientConfig cfg = ctx->cfg;
  if (req->model && req->model[0]) {
    cfg.model = req->model;
  }

  const bool deepseek = url_contains_ci(cfg.base_url, "deepseek");

  Json::Value root(Json::objectValue);
  root["model"] = cfg.model;
  root["stream"] = false;
  Json::Value messages(Json::arrayValue);

  for (size_t i = 0; i < req->message_count; i++) {
    const agent_message_view_t& mv = req->messages[i];
    Json::Value m(Json::objectValue);
    m["role"] = agent_role_to_string(mv.role);

    const size_t part_count = agent_session_message_part_count(req->session, i);
    if (part_count == 0) {
      m["content"] = std::string(mv.content ? mv.content : "", mv.content_len);
      messages.append(m);
      continue;
    }

    Json::Value content(Json::arrayValue);
    for (size_t j = 0; j < part_count; j++) {
      agent_content_part_view_t pv{};
      if (agent_session_get_message_part(req->session, i, j, &pv) != AGENT_OK) continue;

      if (pv.type == AGENT_PART_TEXT) {
        std::string t(pv.text ? pv.text : "", pv.text_len);
        if (t.empty()) continue;
        Json::Value part(Json::objectValue);
        part["type"] = "text";
        part["text"] = t;
        content.append(part);
        continue;
      }

      // OpenAI-compatible "image_url" part. For Kimi K2.5, base64 data: URLs are supported
      // while external URL images are not (provider will reject them).
      if (pv.type == AGENT_PART_IMAGE_URL) {
        std::string u(pv.url ? pv.url : "", pv.url_len);
        if (u.empty()) continue;
        if (deepseek) {
          Json::Value part(Json::objectValue);
          part["type"] = "text";
          part["text"] = "[image omitted: provider does not accept image_url content parts]";
          content.append(part);
        } else {
          Json::Value part(Json::objectValue);
          part["type"] = "image_url";
          Json::Value iu(Json::objectValue);
          iu["url"] = u;
          part["image_url"] = iu;
          content.append(part);
        }
        continue;
      }

      if (pv.type == AGENT_PART_BINARY && pv.bytes && pv.bytes_len > 0) {
        std::string mime(pv.mime ? pv.mime : "", pv.mime_len);
        const std::string m_lc = [&]() {
          std::string s = mime;
          for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
          return s;
        }();
        if (m_lc.rfind("image/", 0) == 0) {
          if (deepseek) {
            Json::Value part(Json::objectValue);
            part["type"] = "text";
            part["text"] = std::string("[image omitted: provider does not accept image_url content parts; mime=") +
              (mime.empty() ? "image/png" : mime) + "]";
            content.append(part);
          } else {
            const std::string b64 = base64_encode((const char*)pv.bytes, pv.bytes_len);
            const std::string url = std::string("data:") + (mime.empty() ? "image/png" : mime) + ";base64," + b64;
            Json::Value part(Json::objectValue);
            part["type"] = "image_url";
            Json::Value iu(Json::objectValue);
            iu["url"] = url;
            part["image_url"] = iu;
            content.append(part);
          }
        } else {
          Json::Value part(Json::objectValue);
          part["type"] = "text";
          part["text"] = std::string("[binary attachment omitted; mime=") + (mime.empty() ? "application/octet-stream" : mime) + "]";
          content.append(part);
        }
        continue;
      }

      // Unknown/unsupported part types: fall back to a text hint.
      Json::Value part(Json::objectValue);
      part["type"] = "text";
      part["text"] = "[unsupported multimodal part]";
      content.append(part);
    }

    // Fallback: ensure there is at least some content.
    if (content.empty()) {
      m["content"] = std::string(mv.content ? mv.content : "", mv.content_len);
    } else {
      m["content"] = content;
    }
    messages.append(m);
  }

  root["messages"] = messages;
  const std::string request_json = json_stringify(root);
  ctx->last_request_body = request_json;

  OpenAIRawResult raw = openai_chat_completions_raw(cfg, request_json);
  ctx->last_http_status = raw.http_status;
  ctx->last_body = raw.response_body;

  if (raw.http_status < 200 || raw.http_status >= 300) {
    ctx->last_error = openai_format_http_error(raw.http_status, raw.response_body);
    if (openai_is_context_too_long_error(raw.http_status, raw.response_body)) {
      return AGENT_ERR_CONTEXT_TOO_LONG;
    }
    return AGENT_ERR_INTERNAL;
  }

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(raw.response_body);
  Json::Value parsed;
  if (!Json::parseFromStream(rb, iss, &parsed, &errs) || !parsed.isObject()) {
    ctx->last_error = "failed to parse JSON response";
    return AGENT_ERR_INTERNAL;
  }

  const std::string assistant = json_try_extract_assistant_text(parsed);
  if (assistant.empty()) {
    ctx->last_error = "failed to extract assistant text from response";
    return AGENT_ERR_INTERNAL;
  }
  return agent_string_set_copy(&out_resp->assistant_text, assistant.c_str(), assistant.size());
#endif
}
