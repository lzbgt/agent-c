#include "tool_loop_truncation.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <sstream>

static std::string truncate_str(const std::string& s, size_t max_chars, bool* out_truncated) {
  if (out_truncated) *out_truncated = false;
  if (max_chars == 0 || s.size() <= max_chars) {
    return s;
  }
  if (out_truncated) *out_truncated = true;
  if (max_chars <= 32) {
    return s.substr(0, max_chars);
  }
  const size_t keep = max_chars - 20;
  return s.substr(0, keep) + "...(truncated)";
}

std::string tool_loop_cap_tool_output_for_prompt(const std::string& tool_out, size_t max_chars, bool* out_truncated) {
  if (out_truncated) *out_truncated = false;
  if (max_chars == 0) {
    return tool_out;
  }

#if !defined(AGENT_HAVE_JSONCPP)
  return truncate_str(tool_out, max_chars, out_truncated);
#else
  // Try to preserve JSON envelope shape:
  // { ok, error?, data: { output: "..." } }
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(tool_out);
  Json::Value root;
  if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isObject()) {
    return truncate_str(tool_out, max_chars, out_truncated);
  }

  // If it already fits, keep it verbatim.
  {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string s = Json::writeString(wb, root);
    if (s.size() <= max_chars) {
      return s;
    }
  }

  bool did = false;
  if (root.isMember("data") && root["data"].isObject()) {
    Json::Value& data = root["data"];
    if (data.isMember("output") && data["output"].isString()) {
      bool trunc = false;
      const std::string out = truncate_str(data["output"].asString(), max_chars / 2, &trunc);
      if (trunc) {
        did = true;
        data["output"] = out;
        data["prompt_truncated"] = true;
      }
    }
    // Some tools use `content` for returned text (e.g. fs_read).
    if (data.isMember("content") && data["content"].isString()) {
      bool trunc = false;
      const std::string out = truncate_str(data["content"].asString(), max_chars / 2, &trunc);
      if (trunc) {
        did = true;
        data["content"] = out;
        data["prompt_truncated"] = true;
      }
    }
    // Sometimes tools return `patch` or other large fields.
    if (data.isMember("patch") && data["patch"].isString()) {
      bool trunc = false;
      const std::string out = truncate_str(data["patch"].asString(), max_chars / 2, &trunc);
      if (trunc) {
        did = true;
        data["patch"] = out;
        data["prompt_truncated"] = true;
      }
    }
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string s = Json::writeString(wb, root);
  if (s.size() > max_chars) {
    // Fall back to whole-string truncation if still too big.
    return truncate_str(s, max_chars, out_truncated);
  }
  if (out_truncated) *out_truncated = did;
  return s;
#endif
}
