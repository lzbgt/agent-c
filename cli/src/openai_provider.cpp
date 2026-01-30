#include "openai_provider.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

agent_provider_t openai_make_provider(OpenAIProviderCtx* ctx) {
  agent_provider_t p{};
  p.ctx = ctx;
  p.generate = openai_provider_generate;
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

