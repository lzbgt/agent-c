#include "run_endpoints_internal.h"

#include "json_util.h"

#include "agent/multimodal_prefix.h"

#include <json/json.h>

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace agentd {

std::string sanitize_job_request_json_for_persist(const std::string& request_body_json) {
  // Durable jobs persist their request JSON (for resume after restart).
  // Never store secrets in the DB; resumed jobs should use daemon-configured keys.
  Json::Value v;
  std::string perr;
  if (!json_parse_object(request_body_json, &v, &perr) || !v.isObject()) {
    return "{}";
  }
  if (v.isMember("api_key")) v.removeMember("api_key");
  if (v.isMember("Authorization")) v.removeMember("Authorization");
  if (v.isMember("auth_token")) v.removeMember("auth_token");
  if (v.isMember("trace_text")) v.removeMember("trace_text");
  if (v.isMember("http_body")) v.removeMember("http_body");

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string out = Json::writeString(wb, v);
  // Refuse to persist oversized job requests rather than truncating invalid JSON.
  // If this returns empty, the job remains runnable in the current process lifetime but will not be resumable after restart.
  if (out.size() > 256 * 1024) return "";
  return out;
}

bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& p) {
  std::error_code ec;
  const std::filesystem::path abs_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) return false;
  ec.clear();
  const std::filesystem::path abs_p = std::filesystem::weakly_canonical(p, ec);
  if (ec) return false;
  auto it_r = abs_root.begin();
  auto it_p = abs_p.begin();
  for (; it_r != abs_root.end(); ++it_r, ++it_p) {
    if (it_p == abs_p.end()) return false;
    if (*it_r != *it_p) return false;
  }
  return true;
}

bool is_safe_relpath_ascii(const std::string& p) {
  if (p.empty() || p.size() > 512) return false;
  if (p.find('\\') != std::string::npos) return false;
  if (p.find("..") != std::string::npos) return false;
  if (!p.empty() && p[0] == '/') return false;
  for (char c : p) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '/' || c == ' ';
    if (!ok) return false;
  }
  return true;
}

bool read_file_bytes_capped(const std::filesystem::path& path, size_t max_bytes, std::string* out_bytes) {
  if (out_bytes) out_bytes->clear();
  if (!out_bytes) return false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) return false;
  const uintmax_t sz = std::filesystem::file_size(path, ec);
  if (ec) return false;
  if (sz > (uintmax_t)max_bytes) return false;

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  out_bytes->resize((size_t)sz);
  in.read(out_bytes->data(), (std::streamsize)out_bytes->size());
  return (bool)in;
}

bool looks_texty(const std::string& bytes) {
  if (bytes.empty()) return true;
  size_t bad = 0;
  for (unsigned char c : bytes) {
    if (c == 0) return false;
    if (c < 0x09) bad++;
    if (c >= 0x0e && c < 0x20) bad++;
  }
  return bad < (bytes.size() / 40 + 1);
}

namespace {

static std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static bool url_contains_ci(const std::string& url, const std::string& needle) {
  if (needle.empty()) return false;
  const std::string u = lower_copy(url);
  const std::string n = lower_copy(needle);
  return u.find(n) != std::string::npos;
}

}  // namespace

bool provider_rejects_image_parts(const std::string& base_url, const std::string& model) {
  // DeepSeek's OpenAI-compatible API schema only supports `content` parts of `type=text`.
  // Avoid sending `type=image_url` to prevent 400 deserialization failures.
  (void)model;
  return url_contains_ci(base_url, "deepseek");
}

bool provider_requires_tools_none_for_vision(const std::string& base_url, const std::string& model) {
  // Some providers support vision, but only in the tools=none (non tool-calling) schema.
  // Moonshot/Kimi is known to accept `image_url` parts for vision, but rejects them in tool-calling requests.
  (void)model;
  return url_contains_ci(base_url, "moonshot");
}

std::string try_extract_assistant_text_from_response_json(const std::string& response_body) {
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(response_body);
  Json::Value root;
  if (!Json::parseFromStream(rb, iss, &root, &errs) || !root.isObject()) return "";
  const auto& choices = root["choices"];
  if (!choices.isArray() || choices.empty()) return "";
  const auto& msg = choices[0]["message"];
  if (msg.isObject()) {
    const auto& content = msg["content"];
    if (content.isString()) return content.asString();
  }
  const auto& text = choices[0]["text"];
  if (text.isString()) return text.asString();
  return "";
}

agent_status_t extended_tool_execute(
  void* vctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  if (!vctx) return AGENT_ERR_INVALID_ARGUMENT;
  auto* ctx = static_cast<ExtendedToolExecutorCtx*>(vctx);
  const char* name = tool_name ? tool_name : "";
  if (ctx->ext.execute_tool && ctx->ext_tool_names.find(name) != ctx->ext_tool_names.end()) {
    return ctx->ext.execute_tool(ctx->ext.ctx, tool_name, arguments_json, out_result);
  }
  if (!ctx->base.execute) return AGENT_ERR_INVALID_ARGUMENT;
  return ctx->base.execute(ctx->base.ctx, tool_name, arguments_json, out_result);
}

agent_status_t host_read_client_events_tail_from_db(
  void* vctx,
  const std::string& session_id,
  size_t max_bytes,
  size_t /*max_files*/,
  std::string* out_tail_jsonl
) {
  if (!out_tail_jsonl) return AGENT_ERR_INVALID_ARGUMENT;
  out_tail_jsonl->clear();
  auto* db = static_cast<AgentDb*>(vctx);
  if (!db || !db->is_open()) return AGENT_ERR_INVALID_ARGUMENT;
  std::string err;
  if (!db->read_client_events_tail_jsonl(session_id, max_bytes, /*max_events=*/0, out_tail_jsonl, &err)) {
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}

bool load_session_from_db(
  AgentDb& db,
  const std::string& session_id,
  agent_session_t** out_session,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_session) return false;
  *out_session = nullptr;

  std::vector<AgentDb::MessageRow> msgs;
  std::string err;
  if (!db.load_session_messages(session_id, &msgs, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to load session messages" : err;
    return false;
  }

  agent_session_t* session = nullptr;
  if (agent_session_create(&session) != AGENT_OK || !session) {
    if (out_error) *out_error = "failed to create session";
    return false;
  }

  for (const auto& rc : msgs) {
    agent_role_t role = AGENT_ROLE_USER;
    if (!rc.role.empty()) {
      (void)agent_role_from_string(rc.role.c_str(), &role);
    }
    std::string content = rc.content;
    if (!rc.mm_json.empty()) {
      content.reserve(rc.mm_json.size() + rc.content.size() + 24);
      content = "__AGENT_MM_V1__";
      content += rc.mm_json;
      content += "\n";
      content += rc.content;
    }
    (void)agent_session_add_message(session, role, content.c_str());
  }

  *out_session = session;
  return true;
}

bool persist_session_to_db(
  AgentDb& db,
  const std::string& session_id,
  const agent_session_t* session,
  int64_t now_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!session) {
    if (out_error) *out_error = "missing session";
    return false;
  }
  std::vector<AgentDb::MessageRow> msgs;
  msgs.reserve(agent_session_message_count(session));
  for (size_t i = 0; i < agent_session_message_count(session); i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    AgentDb::MessageRow row;
    row.role = agent_role_to_string(v.role);
    const std::string content = std::string(v.content ? v.content : "", v.content_len);
    const char* text = nullptr;
    size_t text_len = 0;
    const char* mm_json = nullptr;
    size_t mm_json_len = 0;
    const uint8_t has_mm = agent_parse_multimodal_prefix(
      content.c_str(),
      content.size(),
      &text,
      &text_len,
      &mm_json,
      &mm_json_len
    );
    if (has_mm && mm_json && mm_json_len > 0) {
      row.mm_json.assign(mm_json, mm_json_len);
      row.mm_bytes = (int64_t)mm_json_len;
      row.mm_truncated = 0;
      row.content.assign(text ? text : "", text_len);
    } else {
      row.content = content;
    }
    msgs.push_back(std::move(row));
  }
  return db.replace_session_messages(session_id, msgs, now_unix_ms, out_error);
}

}  // namespace agentd
