#include "toolset_host_internal.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <filesystem>
#include <string>

namespace host_tools_internal {

#if defined(AGENT_HAVE_JSONCPP)
static std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

static std::string stable_rel_path_under_root(
  const std::filesystem::path& root,
  const std::filesystem::path& resolved,
  const std::string& fallback_user_path
) {
  // Prefer returning a stable *relative* path rooted at `root` when possible.
  // This makes artifact URLs deterministic for WebUI clients:
  //   GET /api/v1/file?path=<relative>&yolo=0|1
  //
  // If we cannot safely compute a relative path, return the original user path.
  std::error_code ec;
  const std::filesystem::path canon_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) return fallback_user_path;
  ec.clear();
  const std::filesystem::path canon_file = std::filesystem::weakly_canonical(resolved, ec);
  if (ec) return fallback_user_path;

  if (!path_is_within(canon_root, canon_file)) {
    return fallback_user_path;
  }
  ec.clear();
  std::filesystem::path rel = std::filesystem::relative(canon_file, canon_root, ec);
  if (ec) return fallback_user_path;
  rel = rel.lexically_normal();
  if (rel.empty() || rel.is_absolute()) return fallback_user_path;
  for (const auto& comp : rel) {
    if (comp == "..") return fallback_user_path;
  }
  return to_generic_string(rel);
}

static std::string guess_kind_from_ext(const std::string& path) {
  const std::string p = lower_copy(path);
  if (p.size() >= 4 && (p.rfind(".png") == p.size() - 4 || p.rfind(".jpg") == p.size() - 4 || p.rfind(".gif") == p.size() - 4)) {
    return "image";
  }
  if (p.size() >= 5 && (p.rfind(".jpeg") == p.size() - 5 || p.rfind(".webp") == p.size() - 5)) {
    return "image";
  }
  if (p.size() >= 4 && (p.rfind(".mp3") == p.size() - 4 || p.rfind(".wav") == p.size() - 4)) {
    return "audio";
  }
  if (p.size() >= 4 && (p.rfind(".mp4") == p.size() - 4 || p.rfind(".mov") == p.size() - 4)) {
    return "video";
  }
  if (p.size() >= 5 && p.rfind(".webm") == p.size() - 5) {
    return "video";
  }
  return "file";
}

static std::string guess_mime_from_kind_and_ext(const std::string& kind, const std::string& path) {
  const std::string p = lower_copy(path);
  // File-type overrides (independent of kind).
  if (p.size() >= 5 && p.rfind(".pptx") == p.size() - 5) {
    return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  }
  if (kind == "image") {
    if (p.size() >= 4 && p.rfind(".png") == p.size() - 4) return "image/png";
    if (p.size() >= 4 && (p.rfind(".jpg") == p.size() - 4 || p.rfind(".jpeg") == p.size() - 5)) return "image/jpeg";
    if (p.size() >= 4 && p.rfind(".gif") == p.size() - 4) return "image/gif";
    if (p.size() >= 5 && p.rfind(".webp") == p.size() - 5) return "image/webp";
    if (p.size() >= 4 && p.rfind(".svg") == p.size() - 4) return "image/svg+xml";
    return "image/*";
  }
  if (kind == "audio") {
    if (p.size() >= 4 && p.rfind(".mp3") == p.size() - 4) return "audio/mpeg";
    if (p.size() >= 4 && p.rfind(".wav") == p.size() - 4) return "audio/wav";
    return "audio/*";
  }
  if (kind == "video") {
    if (p.size() >= 4 && p.rfind(".mp4") == p.size() - 4) return "video/mp4";
    if (p.size() >= 5 && p.rfind(".webm") == p.size() - 5) return "video/webm";
    if (p.size() >= 4 && p.rfind(".mov") == p.size() - 4) return "video/quicktime";
    return "video/*";
  }
  if (kind == "text") return "text/plain";
  return "application/octet-stream";
}

agent_status_t tool_artifact_register(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }

  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args.isMember("path") || !args["path"].isString()) {
    return write_envelope(false, "missing string field 'path'", Json::Value(Json::objectValue));
  }

  const std::string path = args["path"].asString();
  // Prefer session-relative resolution when running under agentd. This makes artifact URLs stable per session.
  // If that fails, fall back to root-relative (legacy) resolution so we can copy artifacts into the session folder.
  std::filesystem::path source_resolved;
  {
    const auto r1 = resolve_under_ctx_root(ctx, path);
    if (r1) {
      source_resolved = *r1;
    } else if (!ctx->session_id.empty()) {
      // In session mode, accept legacy root-relative artifact paths as an input source to re-home into the session.
      const auto r2 = resolve_under_root(ctx->root, path, ctx->unrestricted);
      if (r2) source_resolved = *r2;
    }
  }
  if (source_resolved.empty()) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, source_resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }

  std::error_code ec;
  if (!std::filesystem::exists(source_resolved, ec) || !std::filesystem::is_regular_file(source_resolved, ec)) {
    return write_envelope(false, "file not found", Json::Value(Json::objectValue));
  }

  // Enforce "no global artifacts" policy for agentd sessions:
  // - A session's artifacts should live under <sessions_root_dir>/session_<session_id>/out/.
  // - If the source file is outside that directory, copy it in (best-effort), then register the session-local path.
  std::filesystem::path final_resolved = source_resolved;
  if (!ctx->session_id.empty()) {
    const std::filesystem::path out_dir = session_out_dir(ctx);
    if (!out_dir.empty()) {
      ec.clear();
      (void)std::filesystem::create_directories(out_dir, ec);

      const std::filesystem::path norm_out = out_dir.lexically_normal();
      const std::filesystem::path norm_src = source_resolved.lexically_normal();
      const bool already_in_session_out = path_is_within(norm_out, norm_src);
      if (!already_in_session_out) {
        std::filesystem::path base = source_resolved.filename();
        if (base.empty()) base = "artifact";
        std::filesystem::path dest = out_dir / base;

        // Avoid overwriting an existing file (from the same or another run). Generate a unique name if needed.
        if (std::filesystem::exists(dest, ec)) {
          const std::string stem = dest.stem().string();
          const std::string ext = dest.extension().string();
          for (int i = 1; i <= 1000; i++) {
            dest = out_dir / std::filesystem::path(stem + "_" + std::to_string(i) + ext);
            ec.clear();
            if (!std::filesystem::exists(dest, ec)) break;
          }
        }

        ec.clear();
        if (std::filesystem::copy_file(source_resolved, dest, std::filesystem::copy_options::none, ec)) {
          final_resolved = dest;
        } else {
          // If we couldn't copy, fall back to the original path (best-effort).
          final_resolved = source_resolved;
        }
      }
    }
  }

  // Prefer a stable relative path rooted at the session directory when available.
  // This is the canonical artifact addressing mode for agentd clients:
  //   GET /api/v1/file?session_id=<sid>&path=<relative>&yolo=...
  std::filesystem::path stable_root = ctx->root;
  if (!ctx->session_id.empty()) {
    const auto sr = session_root_dir(ctx);
    if (!sr.empty()) stable_root = sr;
  }
  std::string stable_fallback = path;
  {
    std::error_code ec2;
    std::filesystem::path rel = std::filesystem::relative(final_resolved, stable_root, ec2);
    if (!ec2 && !rel.empty() && !rel.is_absolute()) {
      stable_fallback = to_generic_string(rel.lexically_normal());
    }
  }
  const std::string stable_path = stable_rel_path_under_root(stable_root, final_resolved, stable_fallback);

  const std::string kind =
    args.isMember("kind") && args["kind"].isString() ? args["kind"].asString() : guess_kind_from_ext(path);
  const std::string mime =
    args.isMember("mime") && args["mime"].isString() ? args["mime"].asString() : guess_mime_from_kind_and_ext(kind, path);

  const bool autoplay = args.isMember("autoplay") && args["autoplay"].isBool() ? args["autoplay"].asBool() : false;
  int repeat = args.isMember("repeat") && args["repeat"].isInt() ? args["repeat"].asInt() : 1;
  if (repeat < 1) repeat = 1;
  if (repeat > 16) repeat = 16;

  Json::Value artifact(Json::objectValue);
  artifact["path"] = stable_path;
  artifact["resolved_path"] = to_generic_string(final_resolved);
  artifact["source_resolved_path"] = to_generic_string(source_resolved);
  artifact["root_dir"] = to_generic_string(stable_root);
  artifact["unrestricted"] = ctx->unrestricted;
  artifact["kind"] = kind;
  artifact["mime"] = mime;
  artifact["autoplay"] = autoplay;
  artifact["repeat"] = repeat;
  if (args.isMember("title") && args["title"].isString()) {
    artifact["title"] = args["title"].asString();
  }
  ec.clear();
  const uintmax_t sz = std::filesystem::file_size(final_resolved, ec);
  if (!ec) artifact["size_bytes"] = (Json::UInt64)sz;
  ec.clear();
  const auto mtime = std::filesystem::last_write_time(final_resolved, ec);
  if (!ec) artifact["mtime_unix_ms"] = (Json::Int64)file_time_to_unix_ms(mtime);
  if (args.isMember("blob_id") && args["blob_id"].isString()) {
    artifact["blob_id"] = args["blob_id"].asString();
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "artifact_register";
  data["artifact"] = artifact;
  {
    std::string title = artifact.isMember("title") && artifact["title"].isString() ? artifact["title"].asString() : "";
    if (title.empty()) title = path;
    data["output"] = "registered artifact: " + title;
  }

  return write_envelope(true, "", data);
}

agent_status_t tool_scene_apply(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }

  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args.isMember("ops") || !args["ops"].isArray()) {
    return write_envelope(false, "missing array field 'ops'", Json::Value(Json::objectValue));
  }

  // Keep a large but finite cap to avoid pathological memory usage.
  Json::Value ops(Json::arrayValue);
  const Json::Value in_ops = args["ops"];
  const Json::ArrayIndex max_ops = std::min<Json::ArrayIndex>(in_ops.size(), 2000U);
  for (Json::ArrayIndex i = 0; i < max_ops; i++) {
    ops.append(in_ops[i]);
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "scene_apply";
  data["ops"] = ops;
  data["output"] = "scene_apply ops=" + std::to_string((unsigned)ops.size());
  return write_envelope(true, "", data);
}

agent_status_t tool_ui_action(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (is_cancelled(ctx)) {
    return set_result(out_result, "{\"ok\":false,\"error\":\"cancelled\"}");
  }

  auto write_envelope = [&](bool ok, const std::string& error, const Json::Value& data) -> agent_status_t {
    Json::Value o(Json::objectValue);
    o["ok"] = ok;
    if (!error.empty()) o["error"] = error;
    o["data"] = data;
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return set_result(out_result, Json::writeString(wb, o));
  };

  Json::Value args;
  std::string err;
  if (!parse_json(arguments_json, &args, &err) || !args.isObject()) {
    return write_envelope(false, "invalid args", Json::Value(Json::objectValue));
  }
  if (!args.isMember("type") || !args["type"].isString()) {
    return write_envelope(false, "missing string field 'type'", Json::Value(Json::objectValue));
  }

  const std::string type = args["type"].asString();
  const std::string title = args.isMember("title") && args["title"].isString() ? args["title"].asString() : "";
  const std::string message = args.isMember("message") && args["message"].isString() ? args["message"].asString() : "";
  const bool autoplay = args.isMember("autoplay") && args["autoplay"].isBool() ? args["autoplay"].asBool() : false;
  int repeat = args.isMember("repeat") && args["repeat"].isInt() ? args["repeat"].asInt() : 1;
  if (repeat < 1) repeat = 1;
  if (repeat > 16) repeat = 16;

  Json::Value action(Json::objectValue);
  action["type"] = type;
  if (!title.empty()) action["title"] = title;
  if (!message.empty()) action["message"] = message;
  action["autoplay"] = autoplay;
  action["repeat"] = repeat;

  // Pass through structured payload fields for collaboration RPCs and other UI-driven flows.
  //
  // Important: The daemon emits the derived `ui_action` event based on the tool output `data.action`.
  // If we drop fields here (e.g. rpc_id/rpc/code), the client cannot execute the requested action.
  auto pass_string = [&](const char* k) {
    if (!args.isMember(k) || !args[k].isString()) return;
    // Keep a very large but finite cap to avoid pathological memory usage.
    std::string v = args[k].asString();
    const size_t kMax = 256 * 1024;
    if (v.size() > kMax) v.resize(kMax);
    action[k] = v;
  };
  auto pass_bool = [&](const char* k) {
    if (!args.isMember(k) || !args[k].isBool()) return;
    action[k] = args[k].asBool();
  };
  auto pass_object = [&](const char* k) {
    if (!args.isMember(k) || !args[k].isObject()) return;
    action[k] = args[k];
  };

  pass_string("rpc_id");
  pass_object("rpc");
  pass_bool("side_effects");
  pass_bool("auto_run");
  pass_bool("auto");

  // Legacy / compatibility payloads.
  pass_string("probe_id");
  pass_object("probe");

  // Client snapshot request/response correlation.
  pass_string("query_id");

  // Explicitly reject deprecated/brittle UI actions.
  // The Web UI is a generic collaboration client surface; presentation should be driven via:
  // - artifact_register (for files)
  // - client_rpc (dom_apply/page_eval/script_eval/entity_apply)
  if (type == "play_audio") {
    return write_envelope(false, "ui_action type=play_audio is deprecated; use artifact_register + client_rpc instead", Json::Value(Json::objectValue));
  }

  if (type == "notify") {
    if (title.empty() && message.empty()) {
      return write_envelope(false, "notify requires title or message", Json::Value(Json::objectValue));
    }
  } else {
    // Unknown action types are allowed at the tool level (for rolling evolution),
    // but the UI is expected to implement an allowlist and render unknown types safely.
  }

  Json::Value data(Json::objectValue);
  data["tool"] = "ui_action";
  data["action"] = action;
  if (type == "notify") {
    data["output"] = "ui_action notify";
  } else {
    data["output"] = "ui_action " + type;
  }

  return write_envelope(true, "", data);
}
#else
agent_status_t tool_artifact_register(HostToolCtx* /*ctx*/, const char* /*arguments_json*/, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"artifact_register requires jsoncpp\",\"data\":{}}");
}

agent_status_t tool_scene_apply(HostToolCtx* /*ctx*/, const char* /*arguments_json*/, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"scene_apply requires jsoncpp\",\"data\":{}}");
}

agent_status_t tool_ui_action(HostToolCtx* /*ctx*/, const char* /*arguments_json*/, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"ui_action requires jsoncpp\",\"data\":{}}");
}
#endif

}  // namespace host_tools_internal
