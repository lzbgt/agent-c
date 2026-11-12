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

std::string tool_loop_cap_tool_output_for_prompt(
  const std::string& tool_out, size_t max_chars, bool* out_truncated,
  const std::string& executed_tool_name) {
  if (out_truncated) *out_truncated = false;
#if !defined(AGENT_HAVE_JSONCPP)
  (void)executed_tool_name;
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

  auto stringify = [&](const Json::Value& v) -> std::string {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return Json::writeString(wb, v);
  };


  bool did = false;
  const bool saved_listing = executed_tool_name == "fs_list";
  if ((executed_tool_name == "shell_exec" || executed_tool_name == "proc_exec" || saved_listing) &&
      root["data"].isObject() && root["data"]["output"].isString() &&
      root["data"]["output_artifact"].isObject() &&
      root["data"]["output_artifact"]["path"].isString() &&
      !root["data"]["output_artifact"]["path"].asString().empty()) {
    auto& data = root["data"];
    // Bulk output is an artifact, not text for the model to reproduce or extend.
    // Analysis can explicitly fetch the required lines through the readback tool.
    data.removeMember("output");
    if (saved_listing) {
      // Both fields contain the same directory entry names. Remove both before
      // the fits-fast-path, even when the overall model context budget is large.
      if (data["entries"].isArray()) data["entries_count"] = Json::UInt64(data["entries"].size());
      data.removeMember("entries");
      data["entries_omitted_from_prompt"] = true;
    }
    data["prompt_truncated"] = true;
    data["output_omitted_from_prompt"] = true;
    auto& readback = data["output_readback"];
    readback["tool"] = "fs_read";
    readback["arguments"]["path"] = data["output_artifact"]["path"];
    readback["arguments"]["start_line"] = 1;
    readback["arguments"]["max_lines"] = 80;
    readback["arguments"]["max_chars"] = 8000;
    data["output_note"] = saved_listing
      ? "This is a saved directory-listing receipt; entry names and listing text are intentionally absent. entries_count is the number captured under the listed filters; truncated means additional entries may be missing. Exact native listing bytes are at output_artifact.path; Tool outputs may show a marked display excerpt. For analysis, explicitly fs_read relevant lines. For exact listing delivery, artifact_register the saved path. Do not invent or regenerate entries."
      : "This is a saved-output receipt; stdout/stderr text is intentionally absent. Exact captured bytes are at output_artifact.path; Tool outputs may show a marked display excerpt. For analysis, explicitly fs_read relevant lines. For exact file delivery, artifact_register the saved path. Do not invent or regenerate command output.";
    did = true;
  }

  // If it already fits, keep it (normalized) verbatim.
  {
    const std::string s = stringify(root);
    if (max_chars == 0 || s.size() <= max_chars) {
      if (out_truncated) *out_truncated = did;
      return s;
    }
  }

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

    // Some tools return large structured lists (e.g. fs_list: data.entries[]).
    // If the envelope is still too large, prefer truncating the list while keeping JSON valid.
    if (data.isMember("entries") && data["entries"].isArray()) {
      const Json::ArrayIndex total = data["entries"].size();
      if (total > 0) {
        // Keep at most N entries for token efficiency.
        const Json::ArrayIndex keep = std::min<Json::ArrayIndex>(total, 50);
        if (keep < total) {
          Json::Value kept(Json::arrayValue);
          for (Json::ArrayIndex i = 0; i < keep; i++) {
            kept.append(data["entries"][i]);
          }
          data["entries_total"] = (Json::UInt64)total;
          data["entries"] = kept;
          data["entries_truncated"] = true;
          data["prompt_truncated"] = true;
          did = true;
        }
      }
    }

    // Some tools return large structured match lists (e.g. text_search: data.matches[]).
    if (data.isMember("matches") && data["matches"].isArray()) {
      const Json::ArrayIndex total = data["matches"].size();
      if (total > 0) {
        const Json::ArrayIndex keep = std::min<Json::ArrayIndex>(total, 50);
        if (keep < total) {
          Json::Value kept(Json::arrayValue);
          for (Json::ArrayIndex i = 0; i < keep; i++) {
            kept.append(data["matches"][i]);
          }
          data["matches_total"] = (Json::UInt64)total;
          data["matches"] = kept;
          data["matches_truncated"] = true;
          data["prompt_truncated"] = true;
          did = true;
        }
      }
    }
  }

  std::string s = stringify(root);
  if (s.size() <= max_chars) {
    if (out_truncated) *out_truncated = did;
    return s;
  }

  // Still too large: drop the heaviest known fields while preserving JSON validity.
  if (root.isMember("data") && root["data"].isObject()) {
    Json::Value& data = root["data"];
    bool dropped = false;
    if (data.isMember("entries")) {
      data.removeMember("entries");
      data["entries_dropped"] = true;
      dropped = true;
    }
    if (data.isMember("matches")) {
      data.removeMember("matches");
      data["matches_dropped"] = true;
      dropped = true;
    }
    if (dropped) {
      data["prompt_truncated"] = true;
      did = true;
      s = stringify(root);
      if (s.size() <= max_chars) {
        if (out_truncated) *out_truncated = true;
        return s;
      }
    }
  }

  // Final fallback: return a minimal JSON wrapper that cannot exceed max_chars.
  Json::Value minimal(Json::objectValue);
  minimal["ok"] = false;
  minimal["error"] = "tool_output_truncated";
  Json::Value md(Json::objectValue);
  md["prompt_truncated"] = true;
  md["snippet"] = truncate_str(tool_out, max_chars > 64 ? (max_chars - 32) : max_chars, nullptr);
  minimal["data"] = md;
  s = stringify(minimal);
  if (s.size() > max_chars) {
    // As a last resort, ensure the returned string respects max_chars even if it isn't pretty.
    s = s.substr(0, max_chars);
  }
  if (out_truncated) *out_truncated = true;
  return s;
#endif
}
