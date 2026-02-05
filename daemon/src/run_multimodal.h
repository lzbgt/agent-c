#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

// Historical prefix used to tunnel multimodal attachments through a single text field.
extern const char* kMultimodalPrefix;

bool try_parse_multimodal_prefix(const std::string& content, Json::Value* out_mm, std::string* out_text);

// Build OpenAI-compatible "content" value:
// - string when only text exists
// - array of parts when images/files exist
Json::Value multimodal_content_from_parts(const std::string& text, const Json::Value& mm, bool allow_image_parts);

}  // namespace agentd

