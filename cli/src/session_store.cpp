#include "session_store.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <filesystem>
#include <fstream>

static std::filesystem::path session_path(const SessionStoreConfig& cfg, const std::string& session_id) {
  return std::filesystem::path(cfg.root_dir) / (session_id + ".json");
}

static agent_status_t ensure_dir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}

agent_status_t session_store_load(const SessionStoreConfig& cfg, const std::string& session_id, agent_session_t** out_session) {
  if (!out_session) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_session = nullptr;

  agent_session_t* s = nullptr;
  agent_status_t st = agent_session_create(&s);
  if (st != AGENT_OK) {
    return st;
  }

  const auto path = session_path(cfg, session_id);
  std::ifstream in(path);
  if (!in.is_open()) {
    *out_session = s;
    return AGENT_OK; // new session
  }

#if defined(AGENT_HAVE_JSONCPP)
  Json::CharReaderBuilder rb;
  Json::Value root;
  std::string errs;
  if (!Json::parseFromStream(rb, in, &root, &errs)) {
    agent_session_destroy(s);
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
      agent_session_add_message(s, role, content_s.asCString());
    }
  }
#else
  (void)in;
#endif

  *out_session = s;
  return AGENT_OK;
}

agent_status_t session_store_save(const SessionStoreConfig& cfg, const std::string& session_id, const agent_session_t* session) {
  agent_status_t st = ensure_dir(cfg.root_dir);
  if (st != AGENT_OK) {
    return st;
  }

  const auto path = session_path(cfg, session_id);
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    return AGENT_ERR_INTERNAL;
  }

#if defined(AGENT_HAVE_JSONCPP)
  Json::Value root(Json::objectValue);
  Json::Value messages(Json::arrayValue);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t view{};
    if (agent_session_get_message(session, i, &view) != AGENT_OK) {
      continue;
    }
    Json::Value msg(Json::objectValue);
    msg["role"] = agent_role_to_string(view.role);
    msg["content"] = std::string(view.content, view.content_len);
    messages.append(msg);
  }
  root["messages"] = messages;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  out << Json::writeString(wb, root);
#else
  (void)session;
  out << "{}";
#endif

  return AGENT_OK;
}

