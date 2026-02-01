#pragma once

#include "session_id_util.h"
#include "string_util.h"

#include <filesystem>
#include <string>

namespace agentd {

inline std::filesystem::path session_root_path(
  const std::string& sessions_root_dir,
  const std::string& session_id
) {
  if (sessions_root_dir.empty()) return {};
  if (session_id.empty()) return {};
  if (!session_id_is_safe(session_id)) return {};
  const std::string safe_sid = sanitize_filename_component_ascii(session_id);
  return std::filesystem::path(sessions_root_dir) / ("session_" + safe_sid);
}

}  // namespace agentd

