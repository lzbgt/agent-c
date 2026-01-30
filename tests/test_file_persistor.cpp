#include "file_persistor.h"

#include "agent/agent.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

static std::filesystem::path make_temp_root() {
  auto base = std::filesystem::temp_directory_path() / "agent_file_persistor_tests";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  assert(!ec);
  const auto p = base / ("run_" + std::to_string((unsigned long long)std::rand()));
  ec.clear();
  std::filesystem::create_directories(p, ec);
  assert(!ec);
  return p;
}

static void test_roundtrip_list_delete(void) {
  const auto root = make_temp_root();

  agent_persistor_t p{};
  assert(agent_file_persistor_create(root.string().c_str(), &p) == AGENT_OK);
  assert(p.load && p.save && p.del && p.list);

  agent_session_t* s = nullptr;
  assert(agent_session_create(&s) == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "hello") == AGENT_OK);
  assert(p.save(p.ctx, "x", s) == AGENT_OK);
  agent_session_destroy(s);

  agent_session_t* loaded = nullptr;
  assert(p.load(p.ctx, "x", &loaded) == AGENT_OK);
  assert(loaded != nullptr);
  assert(agent_session_message_count(loaded) == 1);
  agent_message_view_t v{};
  assert(agent_session_get_message(loaded, 0, &v) == AGENT_OK);
  assert(std::string(v.content, v.content_len) == "hello");
  agent_session_destroy(loaded);

  std::vector<std::string> ids;
  auto sink = [](void* vctx, const char* id) {
    auto* vec = static_cast<std::vector<std::string>*>(vctx);
    vec->push_back(id ? id : "");
  };
  assert(p.list(p.ctx, sink, &ids) == AGENT_OK);
  assert(ids.size() == 1);
  assert(ids[0] == "x");

  assert(p.del(p.ctx, "x") == AGENT_OK);

  agent_session_t* after_del = nullptr;
  assert(p.load(p.ctx, "x", &after_del) == AGENT_OK);
  assert(after_del != nullptr);
  assert(agent_session_message_count(after_del) == 0);
  agent_session_destroy(after_del);

  agent_persistor_destroy(&p);
}

int main() {
  test_roundtrip_list_delete();
  return 0;
}

