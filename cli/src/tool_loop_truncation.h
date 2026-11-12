#pragma once

#include <cstddef>
#include <string>

// Caps a tool result payload before it is appended into the next LLM request context.
//
// Rationale:
// - Tool outputs (even if already truncated by max_output_bytes) can still be large and repeated,
//   quickly blowing up the context window.
// - We want to preserve the "shape" of JSON envelope outputs when possible, while truncating large fields.
//
// Behavior:
// - If the payload is a JSON object and contains `data.output` as a string, truncates that field.
// - Otherwise truncates the whole payload string.
//
// Returns the capped payload string (still JSON when the input was JSON).
// executed_tool_name is supplied by the executor wrapper, never inferred from payload text.
// Saved shell/proc captures and fs_list listings use a metadata-only receipt even when max_chars is zero.
std::string tool_loop_cap_tool_output_for_prompt(
  const std::string& tool_out, size_t max_chars, bool* out_truncated,
  const std::string& executed_tool_name = {});

