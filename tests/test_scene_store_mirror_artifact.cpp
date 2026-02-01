#include "agent_db.h"
#include "scene_store.h"

#include <json/json.h>

#include <cassert>
#include <filesystem>
#include <string>
#include <unistd.h>

static std::string get_str(const Json::Value& v, const char* k) {
  if (!v.isObject()) return "";
  const Json::Value& x = v[k];
  return x.isString() ? x.asString() : "";
}

static bool get_bool(const Json::Value& v, const char* k, bool def) {
  if (!v.isObject()) return def;
  const Json::Value& x = v[k];
  return x.isBool() ? x.asBool() : def;
}

static Json::Value get_obj(const Json::Value& v, const char* k) {
  if (!v.isObject()) return Json::Value(Json::objectValue);
  const Json::Value& x = v[k];
  return x.isObject() ? x : Json::Value(Json::objectValue);
}

int main() {
#if !defined(AGENT_HAVE_SQLITE3) || !defined(AGENT_HAVE_JSONCPP)
  return 77;
#else
  const std::filesystem::path tmp =
    std::filesystem::temp_directory_path() / ("agentd_scene_store_test_" + std::to_string((long long)getpid()) + ".sqlite");
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
  const int64_t now = 1700000000000LL;

  Json::Value art(Json::objectValue);
  art["path"] = "out/hello.mp3";
  art["kind"] = "audio";
  art["mime"] = "audio/mpeg";
  art["title"] = "Hello in voice";
  art["autoplay"] = true;
  art["repeat"] = 1;

  if (!agentd::scene_store_mirror_artifact(&db, session_id, art, tool_call_id, now, &err)) {
    std::fprintf(stderr, "scene_store_mirror_artifact failed: %s\n", err.c_str());
    return 1;
  }

  Json::Value scene(Json::objectValue);
  int64_t updated = 0;
  if (!agentd::scene_store_get(&db, session_id, &scene, &updated, &err)) {
    std::fprintf(stderr, "scene_store_get failed: %s\n", err.c_str());
    return 1;
  }
  assert(updated == now);
  assert(scene.isObject());

  const std::string artifact_ent_id = "artifact:" + tool_call_id;
  const std::string player_ent_id = artifact_ent_id + ":player";

  assert(scene.isMember(artifact_ent_id));
  assert(scene.isMember(player_ent_id));

  const Json::Value artifact_ent = get_obj(scene, artifact_ent_id.c_str());
  const Json::Value player_ent = get_obj(scene, player_ent_id.c_str());
  assert(get_str(artifact_ent, "kind") == "artifact");
  assert(get_str(player_ent, "kind") == "dom");

  const Json::Value player_props = get_obj(player_ent, "props");
  const Json::Value player_args = get_obj(player_props, "script_args");
  assert(get_str(player_args, "path") == "out/hello.mp3");
  assert(get_bool(player_args, "autoplay", false) == true);

  // Idempotency: mirroring the same artifact again should not create extra entities.
  if (!agentd::scene_store_mirror_artifact(&db, session_id, art, tool_call_id, now + 1, &err)) {
    std::fprintf(stderr, "scene_store_mirror_artifact (repeat) failed: %s\n", err.c_str());
    return 1;
  }
  Json::Value scene2(Json::objectValue);
  int64_t updated2 = 0;
  if (!agentd::scene_store_get(&db, session_id, &scene2, &updated2, &err)) {
    std::fprintf(stderr, "scene_store_get (repeat) failed: %s\n", err.c_str());
    return 1;
  }
  assert(updated2 == now + 1);
  assert(scene2.isObject());
  assert(scene2.getMemberNames().size() == 2);

  db.close();
  std::filesystem::remove(tmp, ec);
  return 0;
#endif
}

