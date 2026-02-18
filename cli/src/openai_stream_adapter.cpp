#include "openai_stream_adapter.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <algorithm>
#include <sstream>
#include <string.h>
#include <string>
#include <vector>

namespace {

static agent_status_t append_agent_string(agent_string_t* dst, const char* data, size_t len) {
  if (!dst || (!data && len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (len == 0) return AGENT_OK;
  const size_t new_len = dst->len + len;
  char* p = (char*)agent_malloc(new_len + 1);
  if (!p) return AGENT_ERR_OOM;
  if (dst->data && dst->len) memcpy(p, dst->data, dst->len);
  memcpy(p + dst->len, data, len);
  p[new_len] = '\0';
  if (dst->data) agent_free(dst->data);
  dst->data = p;
  dst->len = new_len;
  return AGENT_OK;
}

static agent_status_t set_agent_string(agent_string_t* dst, const char* data, size_t len) {
  if (!dst) return AGENT_ERR_INVALID_ARGUMENT;
  if (!data) {
    agent_string_free(dst);
    return agent_string_set_copy(dst, "", 0);
  }
  agent_string_free(dst);
  return agent_string_set_copy(dst, data, len);
}

#if defined(AGENT_HAVE_JSONCPP)
struct ToolDeltaTmp {
  size_t index = 0;
  std::string id;
  std::string name;
  std::string args;
};

static bool json_get_i64_nonneg(const Json::Value& obj, const char* key, int64_t* out) {
  if (!out) return false;
  *out = 0;
  if (!obj.isMember(key)) return false;
  const Json::Value& v = obj[key];
  if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return false;
  const int64_t n = v.asInt64();
  if (n < 0) return false;
  *out = n;
  return true;
}
#endif

static agent_status_t openai_stream_decode_chunk_json(
  const char* chunk_json,
  size_t chunk_len,
  agent_stream_chunk_t* out_chunk
) {
  if (!out_chunk) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out_chunk, 0, sizeof(*out_chunk));
  if (!chunk_json || chunk_len == 0) return AGENT_OK;

#if !defined(AGENT_HAVE_JSONCPP)
  (void)chunk_json;
  (void)chunk_len;
  return AGENT_OK;
#else
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(std::string(chunk_json, chunk_len));
  Json::Value root;
  if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isObject()) {
    return AGENT_OK;
  }

  // Best-effort error extraction.
  if (root.isMember("error")) {
    const Json::Value& e = root["error"];
    if (e.isObject()) {
      const auto& msg = e["message"];
      if (msg.isString()) {
        out_chunk->has_error = 1;
        const std::string s = msg.asString();
        agent_status_t st = set_agent_string(&out_chunk->error_message, s.c_str(), s.size());
        if (st != AGENT_OK) return st;
      }
    } else if (e.isString()) {
      out_chunk->has_error = 1;
      const std::string s = e.asString();
      agent_status_t st = set_agent_string(&out_chunk->error_message, s.c_str(), s.size());
      if (st != AGENT_OK) return st;
    }
  } else if (root.isMember("message") && root["message"].isString()) {
    out_chunk->has_error = 1;
    const std::string s = root["message"].asString();
    agent_status_t st = set_agent_string(&out_chunk->error_message, s.c_str(), s.size());
    if (st != AGENT_OK) return st;
  } else if (root.isMember("detail") && root["detail"].isString()) {
    out_chunk->has_error = 1;
    const std::string s = root["detail"].asString();
    agent_status_t st = set_agent_string(&out_chunk->error_message, s.c_str(), s.size());
    if (st != AGENT_OK) return st;
  }

  // Usage tokens (final stream chunk when include_usage=true).
  if (root.isMember("usage") && root["usage"].isObject()) {
    const Json::Value u = root["usage"];
    int64_t prompt = 0;
    int64_t completion = 0;
    int64_t total = 0;
    const bool have_prompt = json_get_i64_nonneg(u, "prompt_tokens", &prompt);
    const bool have_completion = json_get_i64_nonneg(u, "completion_tokens", &completion);
    const bool have_total = json_get_i64_nonneg(u, "total_tokens", &total);
    if (have_prompt || have_completion || have_total) {
      if (total <= 0 && (prompt > 0 || completion > 0)) total = prompt + completion;
      out_chunk->has_usage = 1;
      out_chunk->prompt_tokens = (uint64_t)std::max<int64_t>(0, prompt);
      out_chunk->completion_tokens = (uint64_t)std::max<int64_t>(0, completion);
      out_chunk->total_tokens = (uint64_t)std::max<int64_t>(0, total);
    }
  }

  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) return AGENT_OK;
  const auto& choice0 = choices[0u];
  if (!choice0.isObject()) return AGENT_OK;

  const auto& fr = choice0["finish_reason"];
  if (fr.isString()) {
    out_chunk->has_finish_reason = 1;
    const std::string s = fr.asString();
    agent_status_t st = set_agent_string(&out_chunk->finish_reason, s.c_str(), s.size());
    if (st != AGENT_OK) return st;
  }

  const auto& delta = choice0["delta"];
  if (!delta.isObject()) return AGENT_OK;

  const auto& content = delta["content"];
  if (content.isString()) {
    const std::string s = content.asString();
    if (!s.empty()) {
      out_chunk->has_delta_text = 1;
      agent_status_t st = set_agent_string(&out_chunk->delta_text, s.c_str(), s.size());
      if (st != AGENT_OK) return st;
    }
  }
  if (!out_chunk->has_delta_text) {
    const auto& text = delta["text"];
    if (text.isString()) {
      const std::string s = text.asString();
      if (!s.empty()) {
        out_chunk->has_delta_text = 1;
        agent_status_t st = set_agent_string(&out_chunk->delta_text, s.c_str(), s.size());
        if (st != AGENT_OK) return st;
      }
    }
  }

  std::vector<ToolDeltaTmp> deltas;
  const auto& tc = delta["tool_calls"];
  if (tc.isArray()) {
    deltas.reserve((size_t)tc.size());
    for (Json::ArrayIndex i = 0; i < tc.size(); i++) {
      const auto& call = tc[i];
      if (!call.isObject()) continue;
      ToolDeltaTmp d;
      const auto& iv = call["index"];
      if (iv.isInt64() || iv.isUInt64() || iv.isInt() || iv.isUInt()) {
        const int64_t n = iv.asInt64();
        d.index = (n >= 0) ? (size_t)n : (size_t)i;
      } else {
        d.index = (size_t)i;
      }
      const auto& idv = call["id"];
      if (idv.isString()) d.id = idv.asString();
      const auto& fn = call["function"];
      if (fn.isObject()) {
        const auto& namev = fn["name"];
        if (namev.isString()) d.name = namev.asString();
        const auto& argv = fn["arguments"];
        if (argv.isString()) d.args = argv.asString();
      }
      deltas.push_back(std::move(d));
    }
  }

  const auto& fc = delta["function_call"];
  if (fc.isObject()) {
    ToolDeltaTmp d;
    d.index = 0;
    const auto& namev = fc["name"];
    if (namev.isString()) d.name = namev.asString();
    const auto& argv = fc["arguments"];
    if (argv.isString()) d.args = argv.asString();
    if (!d.name.empty() || !d.args.empty()) {
      deltas.push_back(std::move(d));
    }
  }

  if (!deltas.empty()) {
    out_chunk->tool_calls = (agent_stream_tool_call_delta_t*)agent_malloc(deltas.size() * sizeof(agent_stream_tool_call_delta_t));
    if (!out_chunk->tool_calls) {
      agent_stream_chunk_free(out_chunk);
      return AGENT_ERR_OOM;
    }
    memset(out_chunk->tool_calls, 0, deltas.size() * sizeof(agent_stream_tool_call_delta_t));
    out_chunk->tool_calls_count = deltas.size();
    for (size_t i = 0; i < deltas.size(); i++) {
      out_chunk->tool_calls[i].index = deltas[i].index;
      if (!deltas[i].id.empty()) {
        agent_status_t st = set_agent_string(&out_chunk->tool_calls[i].id, deltas[i].id.c_str(), deltas[i].id.size());
        if (st != AGENT_OK) {
          agent_stream_chunk_free(out_chunk);
          return st;
        }
      }
      if (!deltas[i].name.empty()) {
        agent_status_t st = set_agent_string(&out_chunk->tool_calls[i].name, deltas[i].name.c_str(), deltas[i].name.size());
        if (st != AGENT_OK) {
          agent_stream_chunk_free(out_chunk);
          return st;
        }
      }
      if (!deltas[i].args.empty()) {
        agent_status_t st = set_agent_string(&out_chunk->tool_calls[i].arguments_delta, deltas[i].args.c_str(), deltas[i].args.size());
        if (st != AGENT_OK) {
          agent_stream_chunk_free(out_chunk);
          return st;
        }
      }
    }
  }

  return AGENT_OK;
#endif
}

static void openai_stream_core_event(void* vctx, const char* type, const char* data_json) {
  OpenAIStreamCoreAdapter* adapter = (OpenAIStreamCoreAdapter*)vctx;
  if (!adapter || !type) return;

  if (strcmp(type, "assistant_delta") == 0) {
#if defined(AGENT_HAVE_JSONCPP)
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(std::string(data_json ? data_json : ""));
    Json::Value root;
    if (Json::parseFromStream(rb, iss, &root, &errs) && root.isObject()) {
      const auto& dv = root["delta"];
      if (dv.isString()) {
        const std::string d = dv.asString();
        if (!d.empty()) {
          adapter->saw_delta = 1;
          (void)append_agent_string(&adapter->assistant, d.data(), d.size());
          (void)append_agent_string(&adapter->pending_delta, d.data(), d.size());
          if (adapter->delta_fn && adapter->pending_delta.len >= adapter->delta_flush_bytes) {
            adapter->delta_fn(adapter->delta_ctx,
                              adapter->pending_delta.data,
                              adapter->pending_delta.len,
                              adapter->step,
                              adapter->epoch);
            agent_string_free(&adapter->pending_delta);
          }
        }
      }
    }
#else
    (void)data_json;
#endif
    return;
  }

  if (strcmp(type, "llm_usage") == 0) {
#if defined(AGENT_HAVE_JSONCPP)
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(std::string(data_json ? data_json : ""));
    Json::Value root;
    if (Json::parseFromStream(rb, iss, &root, &errs) && root.isObject()) {
      int64_t prompt = 0;
      int64_t completion = 0;
      int64_t total = 0;
      bool have_any = false;
      if (json_get_i64_nonneg(root, "prompt_tokens", &prompt)) have_any = true;
      if (json_get_i64_nonneg(root, "completion_tokens", &completion)) have_any = true;
      if (json_get_i64_nonneg(root, "total_tokens", &total)) have_any = true;
      if (total <= 0 && (prompt > 0 || completion > 0)) total = prompt + completion;
      if (have_any) {
        adapter->has_usage = 1;
        adapter->prompt_tokens = (uint64_t)std::max<int64_t>(0, prompt);
        adapter->completion_tokens = (uint64_t)std::max<int64_t>(0, completion);
        adapter->total_tokens = (uint64_t)std::max<int64_t>(0, total);
      }
    }
#else
    (void)data_json;
#endif
    return;
  }

  if (strcmp(type, "error") == 0) {
#if defined(AGENT_HAVE_JSONCPP)
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(std::string(data_json ? data_json : ""));
    Json::Value root;
    if (Json::parseFromStream(rb, iss, &root, &errs) && root.isObject()) {
      const auto& ev = root["error"];
      if (ev.isString()) {
        const std::string s = ev.asString();
        adapter->saw_error = 1;
        (void)set_agent_string(&adapter->error_message, s.c_str(), s.size());
      }
    }
#else
    (void)data_json;
#endif
    return;
  }
}

static agent_status_t openai_stream_decode_fn(
  void* ctx,
  const char* data,
  size_t len,
  agent_stream_chunk_t* out_chunk
) {
  (void)ctx;
  return openai_stream_decode_chunk_json(data, len, out_chunk);
}

static agent_status_t feed_data_as_sse(
  agent_stream_decoder_t* dec,
  const char* data,
  size_t len
) {
  if (!dec) return AGENT_ERR_INVALID_ARGUMENT;
  if (!data || len == 0) return AGENT_OK;

  std::string buf;
  buf.reserve(len + 16);

  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || data[i] == '\n') {
      size_t line_len = i - start;
      if (line_len > 0 && data[start + line_len - 1] == '\r') {
        line_len--;
      }
      buf.append("data: ");
      if (line_len > 0) {
        buf.append(data + start, line_len);
      }
      buf.push_back('\n');
      start = i + 1;
    }
  }
  buf.push_back('\n');
  return agent_stream_decoder_feed(dec, buf.data(), buf.size());
}

}  // namespace

void openai_stream_core_init(
  OpenAIStreamCoreAdapter* adapter,
  const OpenAIStreamCoreConfig* cfg,
  OpenAIStreamDeltaFn delta_fn,
  void* delta_ctx
) {
  if (!adapter) return;
  memset(adapter, 0, sizeof(*adapter));

  OpenAIStreamCoreConfig local{};
  if (cfg) {
    local = *cfg;
  }

  agent_stream_config_t scfg{};
  scfg.max_tool_calls_total = local.max_tool_calls_total;
  scfg.max_tool_call_args_chars = local.max_tool_call_args_chars;
  scfg.max_events_per_feed = local.max_events_per_feed;
  scfg.step = local.step;
  scfg.epoch = local.epoch;

  adapter->step = local.step;
  adapter->epoch = local.epoch;
  adapter->delta_flush_bytes = local.delta_flush_bytes > 0 ? local.delta_flush_bytes : 128;
  adapter->delta_fn = delta_fn;
  adapter->delta_ctx = delta_ctx;

  agent_stream_decoder_init(&adapter->dec, &scfg, openai_stream_decode_fn, nullptr, openai_stream_core_event, adapter);
}

void openai_stream_core_reset(OpenAIStreamCoreAdapter* adapter) {
  if (!adapter) return;
  agent_stream_decoder_reset(&adapter->dec);
  agent_string_free(&adapter->assistant);
  agent_string_free(&adapter->pending_delta);
  agent_string_free(&adapter->error_message);
  adapter->saw_delta = 0;
  adapter->saw_error = 0;
  adapter->has_usage = 0;
  adapter->prompt_tokens = 0;
  adapter->completion_tokens = 0;
  adapter->total_tokens = 0;
}

void openai_stream_core_free(OpenAIStreamCoreAdapter* adapter) {
  if (!adapter) return;
  openai_stream_core_reset(adapter);
  agent_stream_decoder_free(&adapter->dec);
}

agent_status_t openai_stream_core_feed_chunk(
  OpenAIStreamCoreAdapter* adapter,
  const char* chunk_json,
  size_t chunk_len
) {
  if (!adapter) return AGENT_ERR_INVALID_ARGUMENT;
  return feed_data_as_sse(&adapter->dec, chunk_json, chunk_len);
}

void openai_stream_core_flush(OpenAIStreamCoreAdapter* adapter) {
  if (!adapter) return;
  if (adapter->delta_fn && adapter->pending_delta.data && adapter->pending_delta.len > 0) {
    adapter->delta_fn(adapter->delta_ctx,
                      adapter->pending_delta.data,
                      adapter->pending_delta.len,
                      adapter->step,
                      adapter->epoch);
  }
  agent_string_free(&adapter->pending_delta);
}
