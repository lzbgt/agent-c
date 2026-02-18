#include "agent/multimodal_prefix.h"

#include <string.h>

static const char* kMultimodalPrefix = "__AGENT_MM_V1__";

uint8_t agent_parse_multimodal_prefix(
  const char* content,
  size_t content_len,
  const char** out_text,
  size_t* out_text_len,
  const char** out_mm_json,
  size_t* out_mm_json_len
) {
  if (out_text) *out_text = content;
  if (out_text_len) *out_text_len = content_len;
  if (out_mm_json) *out_mm_json = NULL;
  if (out_mm_json_len) *out_mm_json_len = 0;
  if (!content || content_len == 0) return 0;
  if (!out_text || !out_text_len || !out_mm_json || !out_mm_json_len) return 0;

  const size_t prefix_len = strlen(kMultimodalPrefix);
  if (content_len <= prefix_len) return 0;
  if (memcmp(content, kMultimodalPrefix, prefix_len) != 0) return 0;

  const char* nl = (const char*)memchr(content, '\n', content_len);
  if (!nl) return 0;
  const char* json_begin = content + prefix_len;
  if (nl <= json_begin) return 0;
  const size_t json_len = (size_t)(nl - json_begin);
  const char* text = nl + 1;
  const size_t text_len = content_len - (size_t)(text - content);

  *out_mm_json = json_begin;
  *out_mm_json_len = json_len;
  *out_text = text;
  *out_text_len = text_len;
  return 1;
}
