#pragma once

#include "agent_db.h"
#include "tool_extension.h"

#include "agent/tools.h"
#include "agent/agent.h"

#include <filesystem>
#include <string>
#include <unordered_set>

namespace agentd {

// Durable jobs persist their request JSON; ensure we never store secrets in the DB.
// Returns "" if the sanitized JSON would be oversized (refuse-to-persist).
std::string sanitize_job_request_json_for_persist(const std::string& request_body_json);

bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& p);
bool is_safe_relpath_ascii(const std::string& p);
bool read_file_bytes_capped(const std::filesystem::path& path, size_t max_bytes, std::string* out_bytes);
bool looks_texty(const std::string& bytes);

bool provider_rejects_image_parts(const std::string& base_url, const std::string& model);
bool provider_requires_tools_none_for_vision(const std::string& base_url, const std::string& model);
std::string try_extract_assistant_text_from_response_json(const std::string& response_body);

struct ExtendedToolExecutorCtx {
  agent_tool_executor_t base{};
  ToolExtension ext{};
  std::unordered_set<std::string> ext_tool_names;
};

agent_status_t extended_tool_execute(
  void* vctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
);

agent_status_t host_read_client_events_tail_from_db(
  void* vctx,
  const std::string& session_id,
  size_t max_bytes,
  size_t max_files,
  std::string* out_tail_jsonl
);

bool load_session_from_db(
  AgentDb& db,
  const std::string& session_id,
  agent_session_t** out_session,
  std::string* out_error
);

bool persist_session_to_db(
  AgentDb& db,
  const std::string& session_id,
  const agent_session_t* session,
  int64_t now_unix_ms,
  std::string* out_error
);

bool ensure_pinned_host_system_prompts(
  agent_session_t** session_io,
  const std::string& tools,
  bool no_default_system,
  const std::string& host_system_profile,
  const std::string& client_kind,
  bool allow_default_host_prompt
);

}  // namespace agentd
