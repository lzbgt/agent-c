#include "session_store.h"

#include "agent/session_codec.h"

#include <json/json.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static std::string json_session_with_single_message(const char* role, const std::string& content) {
  Json::Value root(Json::objectValue);
  Json::Value messages(Json::arrayValue);
  Json::Value m(Json::objectValue);
  m["role"] = role;
  m["content"] = content;
  messages.append(m);
  root["messages"] = messages;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, root);
}

static void write_file(const std::filesystem::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  assert(out.is_open());
  out.write(s.data(), (std::streamsize)s.size());
  assert(out.good());
}

static std::filesystem::path make_temp_root() {
  auto base = std::filesystem::temp_directory_path() / "agent_session_store_tests";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  assert(!ec);
  // Per-process unique-ish directory (best-effort; ok for unit tests).
  const auto p = base / ("run_" + std::to_string((unsigned long long)std::rand()));
  ec.clear();
  std::filesystem::create_directories(p, ec);
  assert(!ec);
  return p;
}

static void test_load_prefers_sess_over_json(void) {
  const auto root = make_temp_root();
  SessionStoreConfig cfg;
  cfg.root_dir = root.string();

  const std::string id = "s1";
  const auto json_path = root / (id + ".json");
  const auto sess_path = root / (id + ".sess");

  // Write conflicting files: .json says "from_json", .sess says "from_sess".
  write_file(json_path, json_session_with_single_message("user", "from_json"));

  agent_session_t* s = nullptr;
  assert(agent_session_create(&s) == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "from_sess") == AGENT_OK);
  agent_string_t enc = {0};
  assert(agent_session_codec_encode_v1(s, &enc) == AGENT_OK);
  write_file(sess_path, std::string(enc.data ? enc.data : "", enc.len));
  agent_string_free(&enc);
  agent_session_destroy(s);

  agent_session_t* loaded = nullptr;
  assert(session_store_load(cfg, id, &loaded) == AGENT_OK);
  assert(loaded != nullptr);
  assert(agent_session_message_count(loaded) == 1);
  agent_message_view_t v{};
  assert(agent_session_get_message(loaded, 0, &v) == AGENT_OK);
  assert(std::string(v.content, v.content_len) == "from_sess");
  agent_session_destroy(loaded);
}

static void test_load_falls_back_to_json_if_sess_corrupt(void) {
  const auto root = make_temp_root();
  SessionStoreConfig cfg;
  cfg.root_dir = root.string();

  const std::string id = "s2";
  const auto json_path = root / (id + ".json");
  const auto sess_path = root / (id + ".sess");

  write_file(json_path, json_session_with_single_message("assistant", "from_json_ok"));
  write_file(sess_path, "NOPE\nM\tuser\tbroken\n");

  agent_session_t* loaded = nullptr;
  assert(session_store_load(cfg, id, &loaded) == AGENT_OK);
  assert(loaded != nullptr);
  assert(agent_session_message_count(loaded) == 1);
  agent_message_view_t v{};
  assert(agent_session_get_message(loaded, 0, &v) == AGENT_OK);
  assert(std::string(v.content, v.content_len) == "from_json_ok");
  agent_session_destroy(loaded);
}

static void test_list_dedups_extensions(void) {
  const auto root = make_temp_root();
  SessionStoreConfig cfg;
  cfg.root_dir = root.string();

  write_file(root / "a.sess", "AGENT_SESSION\t1\n");
  write_file(root / "a.json", json_session_with_single_message("user", "x"));
  write_file(root / "b.sess", "AGENT_SESSION\t1\n");
  write_file(root / "c.events.jsonl", "{\"ok\":true}\n"); // should be ignored

  std::vector<std::string> ids;
  assert(session_store_list(cfg, &ids) == AGENT_OK);
  // a, b (c ignored)
  assert(ids.size() == 2);
  assert(ids[0] == "a");
  assert(ids[1] == "b");
}

int main() {
  test_load_prefers_sess_over_json();
  test_load_falls_back_to_json_if_sess_corrupt();
  test_list_dedups_extensions();
  std::printf("session_store_tests OK\n");
  return 0;
}
