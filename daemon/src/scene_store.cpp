#include "scene_store.h"

#include "json_util.h"

#include <algorithm>
#include <random>
#include <sstream>

namespace agentd {

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static Json::Value safe_object(const Json::Value& v) {
  return v.isObject() ? v : Json::Value(Json::objectValue);
}

static std::string safe_string(const Json::Value& v) {
  return v.isString() ? v.asString() : "";
}

static std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static bool ends_with(const std::string& s, const std::string& suf) {
  if (s.size() < suf.size()) return false;
  return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static bool starts_with(const std::string& s, const std::string& pre) {
  if (s.size() < pre.size()) return false;
  return s.compare(0, pre.size(), pre) == 0;
}

static bool artifact_is_audio(const Json::Value& artifact_obj, const std::string& path) {
  const std::string kind = to_lower_ascii(safe_string(artifact_obj["kind"]));
  if (kind == "audio") return true;
  const std::string mime = to_lower_ascii(safe_string(artifact_obj["mime"]));
  if (!mime.empty() && starts_with(mime, "audio/")) return true;
  const std::string p = to_lower_ascii(path);
  return ends_with(p, ".mp3") || ends_with(p, ".wav") || ends_with(p, ".ogg") || ends_with(p, ".m4a") || ends_with(p, ".aac") ||
    ends_with(p, ".flac") || ends_with(p, ".aiff") || ends_with(p, ".aif") || ends_with(p, ".aifc");
}

static std::string gen_entity_id(int64_t now_unix_ms) {
  // Best-effort unique id without extra deps.
  static thread_local std::mt19937_64 gen((uint64_t)std::random_device{}());
  const uint64_t r = gen();
  std::ostringstream oss;
  oss << "ent-" << now_unix_ms << "-" << std::hex << r;
  return oss.str();
}

bool scene_store_get(
  AgentDb* db,
  const std::string& session_id,
  Json::Value* out_scene_obj,
  int64_t* out_updated_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_scene_obj) *out_scene_obj = Json::Value(Json::objectValue);
  if (out_updated_unix_ms) *out_updated_unix_ms = 0;
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (session_id.empty()) {
    if (out_error) *out_error = "missing session_id";
    return false;
  }
  std::string raw;
  int64_t updated = 0;
  std::string err;
  if (!db->get_scene_state(session_id, &raw, &updated, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to read scene state" : err;
    return false;
  }
  if (out_updated_unix_ms) *out_updated_unix_ms = updated;

  // Missing row -> empty raw -> empty scene.
  if (raw.empty()) {
    if (out_scene_obj) *out_scene_obj = Json::Value(Json::objectValue);
    return true;
  }
  Json::Value parsed(Json::objectValue);
  std::string perr;
  if (!json_parse_object(raw, &parsed, &perr)) {
    // Corrupt scene state should not crash; treat as empty but surface error for debugging.
    if (out_error) *out_error = "scene_json parse error: " + (perr.empty() ? std::string("invalid JSON") : perr);
    if (out_scene_obj) *out_scene_obj = Json::Value(Json::objectValue);
    return false;
  }
  if (out_scene_obj) *out_scene_obj = parsed;
  return true;
}

bool scene_store_apply_ops(
  AgentDb* db,
  const std::string& session_id,
  const Json::Value& ops,
  int64_t now_unix_ms,
  Json::Value* out_apply_result,
  int64_t* out_updated_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_apply_result) *out_apply_result = Json::Value(Json::objectValue);
  if (out_updated_unix_ms) *out_updated_unix_ms = 0;
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (session_id.empty()) {
    if (out_error) *out_error = "missing session_id";
    return false;
  }
  if (!ops.isArray()) {
    if (out_error) *out_error = "ops must be an array";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : (int64_t)0;

  Json::Value scene_obj(Json::objectValue);
  int64_t prev_updated = 0;
  std::string get_err;
  // If the stored scene JSON is corrupt, treat as empty but still apply ops (repair by overwrite).
  (void)scene_store_get(db, session_id, &scene_obj, &prev_updated, &get_err);
  if (!scene_obj.isObject()) scene_obj = Json::Value(Json::objectValue);

  auto get_ent = [&](const std::string& id) -> Json::Value {
    return scene_obj.isMember(id) ? scene_obj[id] : Json::Value(Json::nullValue);
  };
  auto set_ent = [&](const std::string& id, const Json::Value& ent) {
    scene_obj[id] = ent;
  };

  Json::Value results(Json::arrayValue);

  const auto get_kind = [](const Json::Value& op) -> std::string {
    if (op.isMember("op") && op["op"].isString()) return op["op"].asString();
    if (op.isMember("kind") && op["kind"].isString()) return op["kind"].asString();
    return "";
  };
  const auto get_create_kind = [](const Json::Value& op) -> std::string {
    if (op.isMember("entity_kind") && op["entity_kind"].isString()) return op["entity_kind"].asString();
    if (op.isMember("entityKind") && op["entityKind"].isString()) return op["entityKind"].asString();
    return "";
  };

  // Bound the number of ops applied per request to keep behavior predictable (still "unleashed"; just not infinite).
  const Json::ArrayIndex max_ops = std::min<Json::ArrayIndex>(ops.size(), 1000U);
  for (Json::ArrayIndex i = 0; i < max_ops; i++) {
    try {
      const Json::Value op = ops[i].isObject() ? ops[i] : Json::Value(Json::objectValue);
      const std::string kind = get_kind(op);
      if (kind.empty()) throw std::runtime_error("missing op");

      if (kind == "create") {
        std::string id = safe_string(op["id"]);
        if (id.empty()) id = gen_entity_id(now);
        const std::string entity_kind = get_create_kind(op);
        if (entity_kind.empty()) throw std::runtime_error("create requires entity_kind");

        Json::Value ent(Json::objectValue);
        ent["id"] = id;
        ent["kind"] = entity_kind;
        if (op.isMember("title") && op["title"].isString()) ent["title"] = op["title"];
        ent["props"] = op.isMember("props") ? op["props"] : Json::Value(Json::objectValue);
        ent["created_ms"] = (Json::Int64)now;
        ent["updated_ms"] = (Json::Int64)now;
        set_ent(id, ent);
        Json::Value r(Json::objectValue);
        r["ok"] = true;
        r["op"] = "create";
        r["id"] = id;
        results.append(r);
        continue;
      }

      if (kind == "update") {
        const std::string id = safe_string(op["id"]);
        if (id.empty()) throw std::runtime_error("update requires id");
        Json::Value existing = get_ent(id);
        if (!existing.isObject()) throw std::runtime_error("entity not found");

        Json::Value props = existing.isMember("props") ? safe_object(existing["props"]) : Json::Value(Json::objectValue);
        const Json::Value patch = op.isMember("props") ? op["props"] : Json::Value(Json::objectValue);
        if (patch.isObject()) {
          for (const auto& k : patch.getMemberNames()) {
            props[k] = patch[k];
          }
        }
        existing["props"] = props;
        existing["updated_ms"] = (Json::Int64)now;
        set_ent(id, existing);

        Json::Value r(Json::objectValue);
        r["ok"] = true;
        r["op"] = "update";
        r["id"] = id;
        results.append(r);
        continue;
      }

      if (kind == "delete" || kind == "remove") {
        const std::string id = safe_string(op["id"]);
        if (id.empty()) throw std::runtime_error("delete requires id");
        const bool existed = scene_obj.isMember(id);
        scene_obj.removeMember(id);
        Json::Value r(Json::objectValue);
        r["ok"] = true;
        r["op"] = "delete";
        r["id"] = id;
        r["existed"] = existed;
        results.append(r);
        continue;
      }

      if (kind == "clear") {
        std::string filter_kind;
        if (op.isMember("entity_kind") && op["entity_kind"].isString()) filter_kind = op["entity_kind"].asString();
        if (filter_kind.empty() && op.isMember("kind2") && op["kind2"].isString()) filter_kind = op["kind2"].asString();
        if (filter_kind.empty() && op.isMember("kind") && op["kind"].isString()) filter_kind = op["kind"].asString();

        int removed = 0;
        const auto ids = scene_obj.getMemberNames();
        for (const auto& id : ids) {
          if (!filter_kind.empty()) {
            const Json::Value ent = scene_obj[id];
            const std::string ek = safe_string(ent["kind"]);
            if (ek != filter_kind) continue;
          }
          scene_obj.removeMember(id);
          removed++;
        }
        Json::Value r(Json::objectValue);
        r["ok"] = true;
        r["op"] = "clear";
        r["removed"] = removed;
        if (!filter_kind.empty()) r["kind"] = filter_kind;
        results.append(r);
        continue;
      }

      if (kind == "action") {
        const std::string id = safe_string(op["id"]);
        const std::string action = safe_string(op["action"]);
        if (id.empty()) throw std::runtime_error("action requires id");
        if (action.empty()) throw std::runtime_error("action requires action");
        Json::Value existing = get_ent(id);
        if (!existing.isObject()) throw std::runtime_error("entity not found");
        Json::Value props = existing.isMember("props") ? safe_object(existing["props"]) : Json::Value(Json::objectValue);
        Json::Value last(Json::objectValue);
        last["name"] = action;
        last["args"] = op.isMember("args") ? op["args"] : Json::Value(Json::objectValue);
        last["ts_unix_ms"] = (Json::Int64)now;
        props["last_action"] = last;
        existing["props"] = props;
        existing["updated_ms"] = (Json::Int64)now;
        set_ent(id, existing);

        Json::Value r(Json::objectValue);
        r["ok"] = true;
        r["op"] = "action";
        r["id"] = id;
        r["action"] = action;
        results.append(r);
        continue;
      }

      throw std::runtime_error("unsupported op: " + kind);
    } catch (const std::exception& e) {
      Json::Value r(Json::objectValue);
      r["ok"] = false;
      r["error"] = std::string(e.what());
      results.append(r);
    } catch (...) {
      Json::Value r(Json::objectValue);
      r["ok"] = false;
      r["error"] = "unknown error";
      results.append(r);
    }
  }

  // Persist updated scene state (even if some ops failed).
  const std::string scene_json = json_stringify_compact(scene_obj);
  std::string put_err;
  if (!db->put_scene_state(session_id, scene_json, now, &put_err)) {
    if (out_error) *out_error = put_err.empty() ? "failed to persist scene state" : put_err;
    return false;
  }
  if (out_updated_unix_ms) *out_updated_unix_ms = now;
  if (out_apply_result) {
    Json::Value out(Json::objectValue);
    out["ok"] = true;
    out["results"] = results;
    out["count"] = (Json::UInt64)scene_obj.getMemberNames().size();
    if (!get_err.empty()) out["note"] = get_err; // e.g. prior corrupt JSON repaired
    *out_apply_result = out;
  }
  return true;
}

bool scene_store_mirror_artifact(
  AgentDb* db,
  const std::string& session_id,
  const Json::Value& artifact_obj,
  const std::string& tool_call_id,
  int64_t now_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  const std::string path = artifact_obj.isMember("path") && artifact_obj["path"].isString() ? artifact_obj["path"].asString() : "";
  if (path.empty()) return true; // nothing to mirror

  std::string stable_id;
  if (!tool_call_id.empty()) {
    stable_id = "artifact:" + tool_call_id;
  } else {
    stable_id = "artifact:" + path;
    // Normalize to keep id shortish and deterministic.
    std::replace(stable_id.begin(), stable_id.end(), '/', '_');
    std::replace(stable_id.begin(), stable_id.end(), '\\', '_');
    if (stable_id.size() > 160) stable_id.resize(160);
  }

  const std::string title =
    artifact_obj.isMember("title") && artifact_obj["title"].isString()
      ? artifact_obj["title"].asString()
      : (!path.empty() ? path : "artifact");

  Json::Value ops(Json::arrayValue);
  {
    Json::Value op(Json::objectValue);
    op["op"] = "create";
    op["id"] = stable_id;
    op["entity_kind"] = "artifact";
    op["title"] = "Artifact: " + title;
    Json::Value props(Json::objectValue);
    props["artifact"] = artifact_obj;
    op["props"] = props;
    ops.append(op);
  }

  // High-leverage UX: for audio artifacts, also mirror a durable, refresh-proof player into the Scene.
  // This avoids brittle reliance on client RPC (artifact_url/page_eval) for the common "say hello in voice" case.
  if (artifact_is_audio(artifact_obj, path)) {
    const std::string player_id = stable_id + ":player";
    const bool autoplay = artifact_obj.isMember("autoplay") && artifact_obj["autoplay"].isBool() ? artifact_obj["autoplay"].asBool() : false;
    const int repeat = artifact_obj.isMember("repeat") && artifact_obj["repeat"].isInt() ? std::max(1, artifact_obj["repeat"].asInt()) : 1;

    Json::Value op(Json::objectValue);
    op["op"] = "create";
    op["id"] = player_id.size() > 200 ? player_id.substr(0, 200) : player_id;
    op["entity_kind"] = "dom";
    op["title"] = "Audio: " + title;

    Json::Value props(Json::objectValue);
    props["html"] =
      "<div style='display:flex;flex-direction:column;gap:8px'>"
      "  <div style='font:12px ui-sans-serif;color:#e5e7eb'>Audio player</div>"
      "  <audio data-agent-role='audio' controls style='width:100%'></audio>"
      "  <div data-agent-role='status' style='font:11px ui-sans-serif;color:#94a3b8'></div>"
      "</div>";

    props["script"] =
      "const audio = api.root.querySelector('[data-agent-role=\"audio\"]');\n"
      "const status = api.root.querySelector('[data-agent-role=\"status\"]');\n"
      "if (!audio) throw new Error('missing audio element');\n"
      "const setStatus = (s) => { try { if (status) status.textContent = String(s || ''); } catch (e) {} };\n"
      "setStatus('loading...');\n"
      "audio.addEventListener('play', () => setStatus('playing'));\n"
      "audio.addEventListener('pause', () => setStatus('paused'));\n"
      "audio.addEventListener('ended', () => setStatus('ended'));\n"
      "audio.addEventListener('error', () => setStatus('error'));\n"
      "const url = await api.artifact.url(args.path);\n"
      "audio.src = url;\n"
      "audio.autoplay = !!args.autoplay;\n"
      "audio.loop = !!args.loop;\n"
      "setStatus(args.autoplay ? 'attempting autoplay...' : 'ready');\n"
      "if (args.autoplay) {\n"
      "  try { await audio.play(); setStatus('playing'); }\n"
      "  catch (e) { setStatus('autoplay blocked; click play'); }\n"
      "}\n"
      "return () => { try { audio.pause(); } catch (e) {} };\n";

    Json::Value args(Json::objectValue);
    args["path"] = path;
    args["autoplay"] = autoplay;
    args["loop"] = repeat > 1;
    props["script_args"] = args;
    op["props"] = props;
    ops.append(op);
  }

  Json::Value apply_result(Json::objectValue);
  int64_t updated_ms = 0;
  std::string err;
  if (!scene_store_apply_ops(db, session_id, ops, now_unix_ms, &apply_result, &updated_ms, &err)) {
    if (out_error) *out_error = err;
    return false;
  }
  return true;
}

}  // namespace agentd
