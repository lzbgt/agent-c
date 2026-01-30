#include "openai_stream_decoder.h"

#include "agent/agent.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <sstream>

bool openai_stream_parse_chunk_json(const char* chunk_json, size_t chunk_len, OpenAIStreamChunk* out_chunk) {
  if (!chunk_json || chunk_len == 0 || !out_chunk) return false;
  out_chunk->content_delta.clear();
  out_chunk->tool_call_deltas.clear();
  out_chunk->finish_reason.clear();

#if !defined(AGENT_HAVE_JSONCPP)
  return false;
#else
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(std::string(chunk_json, chunk_len));
  Json::Value root;
  if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isObject()) {
    return false;
  }

  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) return false;
  const auto& choice0 = choices[0u];
  if (!choice0.isObject()) return false;

  const auto& fr = choice0["finish_reason"];
  if (fr.isString()) out_chunk->finish_reason = fr.asString();

  const auto& delta = choice0["delta"];
  if (!delta.isObject()) return false;

  const auto& content = delta["content"];
  if (content.isString()) out_chunk->content_delta = content.asString();

  const auto& tc = delta["tool_calls"];
  if (tc.isArray()) {
    out_chunk->tool_call_deltas.reserve((size_t)tc.size());
    for (Json::ArrayIndex i = 0; i < tc.size(); i++) {
      const auto& call = tc[i];
      if (!call.isObject()) continue;

      OpenAIStreamToolCallDelta d;
      const auto& iv = call["index"];
      if (iv.isInt()) d.index = iv.asInt();
      else d.index = (int)i;

      const auto& idv = call["id"];
      if (idv.isString()) d.id = idv.asString();

      const auto& fn = call["function"];
      if (fn.isObject()) {
        const auto& namev = fn["name"];
        if (namev.isString()) d.name = namev.asString();
        const auto& argv = fn["arguments"];
        if (argv.isString()) d.arguments_delta = argv.asString();
      }
      out_chunk->tool_call_deltas.push_back(std::move(d));
    }
  }

  return true;
#endif
}

void OpenAIToolCallStreamAccumulator::reset() {
  calls_.clear();
}

void OpenAIToolCallStreamAccumulator::apply(const std::vector<OpenAIStreamToolCallDelta>& deltas) {
  for (const auto& d : deltas) {
    if (d.index < 0) continue;
    if ((size_t)d.index >= calls_.size()) {
      calls_.resize((size_t)d.index + 1);
    }
    OpenAIStreamToolCall& dst = calls_[(size_t)d.index];
    if (!d.id.empty()) dst.id = d.id;
    if (!d.name.empty()) dst.name = d.name;
    if (!d.arguments_delta.empty()) dst.arguments += d.arguments_delta;
  }
}

