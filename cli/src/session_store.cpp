#include "session_store.h"

#include "agent/session_codec.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_set>

static bool is_portable_tool_transcript_marker(agent_role_t role, const std::string& content) {
  // Older versions stored tool calls/results as assistant text markers in the session message list:
  //   "[tool_call] name=...\\n{...json...}"
  //   "[tool_result] name=...\\n{...json...}"
  //
  // This is useful for a fully portable transcript, but it bloats context windows and pollutes tools=none runs.
  // We now persist tool details to the per-session audit JSONL, while session messages store only user/assistant text.
  if (role != AGENT_ROLE_ASSISTANT) return false;
  if (content.rfind("[tool_call] name=", 0) == 0) return true;
  if (content.rfind("[tool_result] name=", 0) == 0) return true;
  return false;
}

static std::filesystem::path session_path_json(const SessionStoreConfig& cfg, const std::string& session_id) {
  return std::filesystem::path(cfg.root_dir) / (session_id + ".json");
}

static std::filesystem::path session_path_sess(const SessionStoreConfig& cfg, const std::string& session_id) {
  return std::filesystem::path(cfg.root_dir) / (session_id + ".sess");
}

static std::filesystem::path session_audit_path(const SessionStoreConfig& cfg, const std::string& session_id) {
  return std::filesystem::path(cfg.root_dir) / (session_id + ".events.jsonl");
}

static agent_status_t ensure_dir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}

static bool file_exists(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec);
}

static agent_status_t read_file_all(const std::filesystem::path& p, std::string* out) {
  if (!out) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  out->clear();
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) {
    return AGENT_ERR_INTERNAL;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return AGENT_OK;
}

static agent_status_t write_file_atomic(const std::filesystem::path& p, const char* data, size_t len) {
  const auto tmp = std::filesystem::path(p.string() + ".tmp");

  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return AGENT_ERR_INTERNAL;
    }
    if (len) {
      out.write(data, (std::streamsize)len);
      if (!out) {
        return AGENT_ERR_INTERNAL;
      }
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (!ec) {
    return AGENT_OK;
  }

  // Best-effort fallback for platforms where rename() won't replace existing files.
  ec.clear();
  std::filesystem::remove(p, ec);
  ec.clear();
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}

static agent_status_t load_sess_file_filtered(const std::filesystem::path& p, agent_session_t** out_session) {
  if (!out_session) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_session = nullptr;

  std::string text;
  agent_status_t st = read_file_all(p, &text);
  if (st != AGENT_OK) {
    return st;
  }

  agent_session_t* loaded = nullptr;
  st = agent_session_codec_decode_v1(text.data(), text.size(), &loaded);
  if (st != AGENT_OK) {
    return st;
  }

  agent_session_t* filtered = nullptr;
  st = agent_session_create(&filtered);
  if (st != AGENT_OK) {
    agent_session_destroy(loaded);
    return st;
  }

  const size_t n = agent_session_message_count(loaded);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t view{};
    if (agent_session_get_message(loaded, i, &view) != AGENT_OK) {
      continue;
    }
    const std::string content(view.content, view.content_len);
    if (is_portable_tool_transcript_marker(view.role, content)) {
      continue;
    }
    (void)agent_session_add_message(filtered, view.role, content.c_str());
  }

  agent_session_destroy(loaded);
  *out_session = filtered;
  return AGENT_OK;
}

agent_status_t session_store_load(const SessionStoreConfig& cfg, const std::string& session_id, agent_session_t** out_session) {
  if (!out_session) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_session = nullptr;

  const auto json_path = session_path_json(cfg, session_id);
  const auto sess_path = session_path_sess(cfg, session_id);

  // Prefer the portable JSON-free session format when present (it is the primary persisted format).
  // Fall back to .json for older sessions or if the .sess file is corrupt/unreadable.
  if (file_exists(sess_path)) {
    agent_status_t st = load_sess_file_filtered(sess_path, out_session);
    if (st == AGENT_OK && *out_session) {
      return AGENT_OK;
    }
    // Best-effort fallback: try .json next.
  }

#if defined(AGENT_HAVE_JSONCPP)
  if (file_exists(json_path)) {
    agent_session_t* s = nullptr;
    agent_status_t st = agent_session_create(&s);
    if (st != AGENT_OK) {
      return st;
    }

    std::ifstream in(json_path);
    if (!in.is_open()) {
      agent_session_destroy(s);
      return AGENT_ERR_INTERNAL;
    }

  Json::CharReaderBuilder rb;
  Json::Value root;
  std::string errs;
  if (!Json::parseFromStream(rb, in, &root, &errs)) {
    agent_session_destroy(s);
    if (file_exists(sess_path)) {
      return load_sess_file_filtered(sess_path, out_session);
    }
    return AGENT_ERR_INTERNAL;
  }
  const auto& messages = root["messages"];
  if (messages.isArray()) {
    for (const auto& m : messages) {
      if (!m.isObject()) {
        continue;
      }
      agent_role_t role;
      const auto role_s = m["role"];
      const auto content_s = m["content"];
      if (!role_s.isString() || !content_s.isString()) {
        continue;
      }
      if (agent_role_from_string(role_s.asCString(), &role) != AGENT_OK) {
        continue;
      }
      const std::string content = content_s.asString();
      if (is_portable_tool_transcript_marker(role, content)) {
        continue;
      }
      agent_session_add_message(s, role, content.c_str());
    }
  }
    *out_session = s;
    return AGENT_OK;
  }
#endif

  agent_session_t* s = nullptr;
  agent_status_t st = agent_session_create(&s);
  if (st != AGENT_OK) {
    return st;
  }
  *out_session = s;
  return AGENT_OK; // new session
}

agent_status_t session_store_save(const SessionStoreConfig& cfg, const std::string& session_id, const agent_session_t* session) {
  agent_status_t st = ensure_dir(cfg.root_dir);
  if (st != AGENT_OK) {
    return st;
  }

  // Always write the portable JSON-free session codec.
  {
    agent_string_t encoded{};
    st = agent_session_codec_encode_v1(session, &encoded);
    if (st != AGENT_OK) {
      return st;
    }
    const auto sess_path = session_path_sess(cfg, session_id);
    st = write_file_atomic(sess_path, encoded.data ? encoded.data : "", encoded.len);
    agent_string_free(&encoded);
    if (st != AGENT_OK) {
      return st;
    }
  }

#if defined(AGENT_HAVE_JSONCPP)
  // Also write an OpenAI-ish JSON shape for debugging and interoperability.
  Json::Value root(Json::objectValue);
  Json::Value messages(Json::arrayValue);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t view{};
    if (agent_session_get_message(session, i, &view) != AGENT_OK) {
      continue;
    }
    const std::string content(view.content, view.content_len);
    if (is_portable_tool_transcript_marker(view.role, content)) {
      continue;
    }
    Json::Value msg(Json::objectValue);
    msg["role"] = agent_role_to_string(view.role);
    msg["content"] = content;
    messages.append(msg);
  }
  root["messages"] = messages;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  const std::string json = Json::writeString(wb, root);
  const auto json_path = session_path_json(cfg, session_id);
  st = write_file_atomic(json_path, json.data(), json.size());
  if (st != AGENT_OK) {
    return st;
  }
#endif

  return AGENT_OK;
}

agent_status_t session_store_list(const SessionStoreConfig& cfg, std::vector<std::string>* out_session_ids) {
  if (!out_session_ids) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  out_session_ids->clear();

  std::error_code ec;
  if (!std::filesystem::exists(cfg.root_dir, ec)) {
    return AGENT_OK;
  }

  std::unordered_set<std::string> seen;
  for (const auto& entry : std::filesystem::directory_iterator(cfg.root_dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const auto p = entry.path();
    const std::string filename = p.filename().string();
    // Skip audit files if someone misnames them.
    if (filename.size() >= std::string(".events.jsonl").size() &&
        filename.rfind(".events.jsonl") == filename.size() - std::string(".events.jsonl").size()) {
      continue;
    }

    const auto ext = p.extension().string();
    if (ext != ".json" && ext != ".sess") {
      continue;
    }

    seen.insert(p.stem().string());
  }

  out_session_ids->assign(seen.begin(), seen.end());
  std::sort(out_session_ids->begin(), out_session_ids->end());
  return AGENT_OK;
}

agent_status_t session_store_delete(const SessionStoreConfig& cfg, const std::string& session_id) {
  agent_status_t st = ensure_dir(cfg.root_dir);
  if (st != AGENT_OK) {
    return st;
  }
  std::error_code ec;
  std::filesystem::remove(session_path_json(cfg, session_id), ec);
  ec.clear();
  std::filesystem::remove(session_path_sess(cfg, session_id), ec);
  ec.clear();
  std::filesystem::remove(session_audit_path(cfg, session_id), ec);
  return AGENT_OK;
}

agent_status_t session_store_append_audit_jsonl(const SessionStoreConfig& cfg, const std::string& session_id, const std::string& record_json) {
  agent_status_t st = ensure_dir(cfg.root_dir);
  if (st != AGENT_OK) {
    return st;
  }
  const auto p = session_audit_path(cfg, session_id);
  std::ofstream out(p, std::ios::app);
  if (!out.is_open()) {
    return AGENT_ERR_INTERNAL;
  }
  out << record_json << "\n";
  return AGENT_OK;
}

agent_status_t session_store_read_audit_tail(const SessionStoreConfig& cfg, const std::string& session_id, size_t max_bytes, std::string* out_text) {
  if (!out_text) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  out_text->clear();

  const auto p = session_audit_path(cfg, session_id);
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) {
    return AGENT_OK;
  }

  in.seekg(0, std::ios::end);
  std::streamoff size = in.tellg();
  if (size <= 0) {
    return AGENT_OK;
  }
  const std::streamoff want = (std::streamoff)std::min<size_t>((size_t)size, max_bytes);
  in.seekg(size - want, std::ios::beg);

  std::string buf;
  buf.resize((size_t)want);
  in.read(buf.data(), want);
  if (!in) {
    // Best-effort: partial read.
    buf.resize((size_t)in.gcount());
  }

  // If we started mid-line, drop until next newline so caller can parse JSONL.
  if (want < size) {
    const size_t nl = buf.find('\n');
    if (nl != std::string::npos) {
      buf = buf.substr(nl + 1);
    } else {
      buf.clear();
    }
  }

  *out_text = buf;
  return AGENT_OK;
}
