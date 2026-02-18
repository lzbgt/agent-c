#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parses a __AGENT_MM_V1__ prefix if present, returning:
// - out_text/out_text_len: the content after the prefix/newline (original text)
// - out_mm_json/out_mm_json_len: the raw JSON blob embedded in the prefix line
//
// Returns 1 when a valid prefix is found, else 0.
uint8_t agent_parse_multimodal_prefix(
  const char* content,
  size_t content_len,
  const char** out_text,
  size_t* out_text_len,
  const char** out_mm_json,
  size_t* out_mm_json_len
);

#ifdef __cplusplus
}  // extern "C"
#endif
