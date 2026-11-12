#pragma once

#include <json/json.h>

#include <string>
#include <utility>

// Some OpenAI-compatible chat templates consume only one or two leading system
// messages. Merge compatible leading system envelopes at the wire boundary so
// every managed instruction reaches the model. Durable sessions stay separate.
// Never move later system messages across a user/assistant/tool boundary.
inline void openai_coalesce_leading_system_messages(Json::Value& messages) {
  if (!messages.isArray() || messages.size() < 2 ||
      !messages[0].isObject() || messages[0]["role"] != "system") return;
  Json::Value envelope = messages[0];
  envelope.removeMember("content");
  Json::ArrayIndex count = 0;
  bool have_parts = false;
  for (; count < messages.size(); ++count) {
    const auto& message = messages[count];
    if (!message.isObject() || message["role"] != "system" ||
        (!message["content"].isString() && !message["content"].isArray())) break;
    auto candidate = message;
    candidate.removeMember("content");
    // Preserve message names and other metadata rather than silently dropping
    // them. A differently named compaction system message remains a later one.
    if (candidate != envelope) break;
    have_parts = have_parts || message["content"].isArray();
  }
  if (count < 2) return;

  Json::Value merged = messages[0];
  if (!have_parts) {
    std::string content;
    for (Json::ArrayIndex i = 0; i < count; ++i) {
      if (i) content += "\n\n";
      content += messages[i]["content"].asString();
    }
    merged["content"] = content;
  } else {
    Json::Value parts(Json::arrayValue);
    const auto text_part = [&](const std::string& text) {
      Json::Value part(Json::objectValue);
      part["type"] = "text";
      part["text"] = text;
      parts.append(part);
    };
    for (Json::ArrayIndex i = 0; i < count; ++i) {
      if (i) text_part("\n\n");
      const auto& content = messages[i]["content"];
      if (content.isArray()) {
        for (const auto& part : content) parts.append(part);
      } else {
        text_part(content.asString());
      }
    }
    merged["content"] = parts;
  }
  Json::Value out(Json::arrayValue);
  out.append(merged);
  for (Json::ArrayIndex i = count; i < messages.size(); ++i) out.append(messages[i]);
  messages = std::move(out);
}
