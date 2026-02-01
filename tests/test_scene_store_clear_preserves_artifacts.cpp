#include "agent_db.h"
#include "scene_store.h"

#include <json/json.h>

#include <cassert>
#include <filesystem>
#include <string>
#include <unistd.h>

static Json::Value obj() { return Json::Value(Json::objectValue); }
static Json::Value arr() { return Json::Value(Json::arrayValue); }

int main() {
#if !defined(AGENT_HAVE_SQLITE3) || !defined(AGENT_HAVE_JSONCPP)
  return 77;
#else
  const std::filesystem::path tmp =
    std::filesystem::temp_directory_path() / ("agentd_scene_clear_test_" + std::to_string((long long)getpid()) + ".sqlite");
  std::error_code ec;
  std::filesystem::remove(tmp, ec);

  agentd::AgentDb db;
  std::string err;
  if (!db.open(tmp.string(), &err)) {
    std::fprintf(stderr, "db.open failed: %s\n", err.c_str());
    return 1;
  }

  const std::string session_id = "s1";
  const std::string tool_call_id = "call_audio_1";
  int64_t now = 1700000000000LL;

  Json::Value art = obj();
  art["path"] = "out/hello.mp3";
  art["kind"] = "audio";
  art["mime"] = "audio/mpeg";
  art["title"] = "Hello in voice";
  if (!agentd::scene_store_mirror_artifact(&db, session_id, art, tool_call_id, now, &err)) {
    std::fprintf(stderr, "scene_store_mirror_artifact failed: %s\n", err.c_str());
    return 1;
  }

  const std::string artifact_ent_id = "artifact:" + tool_call_id;
  const std::string player_ent_id = artifact_ent_id + ":player";

  // Add a normal entity, then clear: artifacts should be preserved.
  now++;
  Json::Value ops = arr();
  Json::Value c = obj();
  c["op"] = "create";
  c["id"] = "note";
  c["entity_kind"] = "dom";
  c["title"] = "Note";
  c["props"] = obj();
  ops.append(c);
  Json::Value apply = obj();
  int64_t updated = 0;
  if (!agentd::scene_store_apply_ops(&db, session_id, ops, now, &apply, &updated, &err)) {
    std::fprintf(stderr, "scene_store_apply_ops (create) failed: %s\n", err.c_str());
    return 1;
  }

  now++;
  Json::Value clear_ops = arr();
  Json::Value cl = obj();
  cl["op"] = "clear";
  clear_ops.append(cl);
  if (!agentd::scene_store_apply_ops(&db, session_id, clear_ops, now, &apply, &updated, &err)) {
    std::fprintf(stderr, "scene_store_apply_ops (clear) failed: %s\n", err.c_str());
    return 1;
  }

  Json::Value scene = obj();
  if (!agentd::scene_store_get(&db, session_id, &scene, &updated, &err)) {
    std::fprintf(stderr, "scene_store_get failed: %s\n", err.c_str());
    return 1;
  }
  assert(scene.isObject());
  assert(!scene.isMember("note"));
  assert(scene.isMember(artifact_ent_id));
  assert(scene.isMember(player_ent_id));

  // Explicitly include artifacts when clearing: remove everything.
  now++;
  Json::Value clear_all = arr();
  Json::Value cl_all = obj();
  cl_all["op"] = "clear";
  cl_all["include_artifacts"] = true;
  clear_all.append(cl_all);
  if (!agentd::scene_store_apply_ops(&db, session_id, clear_all, now, &apply, &updated, &err)) {
    std::fprintf(stderr, "scene_store_apply_ops (clear all) failed: %s\n", err.c_str());
    return 1;
  }
  Json::Value scene2 = obj();
  if (!agentd::scene_store_get(&db, session_id, &scene2, &updated, &err)) {
    std::fprintf(stderr, "scene_store_get (after clear all) failed: %s\n", err.c_str());
    return 1;
  }
  assert(scene2.isObject());
  assert(scene2.getMemberNames().empty());

  db.close();
  std::filesystem::remove(tmp, ec);
  return 0;
#endif
}

