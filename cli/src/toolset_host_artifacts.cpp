#include "toolset_host_internal.h"

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#include <filesystem>
#include <fstream>
#include <string>

namespace host_tools_internal {

#if defined(AGENT_HAVE_JSONCPP)
static std::string lower_copy(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
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

static std::string svg_escape_text(std::string s) {
  // Minimal escaping for embedding arbitrary text into SVG <text>.
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c; break;
    }
  }
  return out;
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
  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }

  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_regular_file(*resolved, ec)) {
    return write_envelope(false, "file not found", Json::Value(Json::objectValue));
  }

  const std::string kind =
    args.isMember("kind") && args["kind"].isString() ? args["kind"].asString() : guess_kind_from_ext(path);
  const std::string mime =
    args.isMember("mime") && args["mime"].isString() ? args["mime"].asString() : guess_mime_from_kind_and_ext(kind, path);

  const bool autoplay = args.isMember("autoplay") && args["autoplay"].isBool() ? args["autoplay"].asBool() : false;
  int repeat = args.isMember("repeat") && args["repeat"].isInt() ? args["repeat"].asInt() : 1;
  if (repeat < 1) repeat = 1;
  if (repeat > 16) repeat = 16;

  Json::Value artifact(Json::objectValue);
  artifact["path"] = path;
  artifact["resolved_path"] = to_generic_string(*resolved);
  artifact["root_dir"] = to_generic_string(ctx->root);
  artifact["unrestricted"] = ctx->unrestricted;
  artifact["kind"] = kind;
  artifact["mime"] = mime;
  artifact["autoplay"] = autoplay;
  artifact["repeat"] = repeat;
  if (args.isMember("title") && args["title"].isString()) {
    artifact["title"] = args["title"].asString();
  }
  ec.clear();
  const uintmax_t sz = std::filesystem::file_size(*resolved, ec);
  if (!ec) artifact["size_bytes"] = (Json::UInt64)sz;
  ec.clear();
  const auto mtime = std::filesystem::last_write_time(*resolved, ec);
  if (!ec) artifact["mtime_unix_ms"] = (Json::Int64)file_time_to_unix_ms(mtime);

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

  // Media actions can reference host files; validate and normalize metadata.
  if (type == "play_audio") {
    if (!args.isMember("path") || !args["path"].isString()) {
      return write_envelope(false, "missing string field 'path' for play_audio", Json::Value(Json::objectValue));
    }
    const std::string path = args["path"].asString();
    const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
    if (!resolved) {
      return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
    }
    if (!ctx->unrestricted && !ctx->allow_symlinks) {
      if (path_contains_symlink_component(ctx->root, *resolved)) {
        return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
      }
    }

    std::error_code ec;
    if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_regular_file(*resolved, ec)) {
      return write_envelope(false, "file not found", Json::Value(Json::objectValue));
    }

    std::string mime = args.isMember("mime") && args["mime"].isString() ? args["mime"].asString() : "";
    if (mime.empty()) {
      mime = guess_mime_from_kind_and_ext("audio", path);
    }
    action["path"] = path;
    action["resolved_path"] = to_generic_string(*resolved);
    action["root_dir"] = to_generic_string(ctx->root);
    action["unrestricted"] = ctx->unrestricted;
    action["mime"] = mime;
  } else if (type == "notify") {
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

agent_status_t tool_camera_capture(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
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

  const std::string backend =
    args.isMember("backend") && args["backend"].isString() ? args["backend"].asString() : "auto";
  const bool register_artifact =
    !(args.isMember("register_artifact") && args["register_artifact"].isBool() && args["register_artifact"].asBool() == false);
  const bool notify = args.isMember("notify") && args["notify"].isBool() ? args["notify"].asBool() : false;
  const std::string title = args.isMember("title") && args["title"].isString() ? args["title"].asString() : "";
  const int timeout_ms = args.isMember("timeout_ms") && args["timeout_ms"].isInt() ? args["timeout_ms"].asInt() : 8000;

  const bool use_mock = (backend == "mock") || (backend == "auto" && !ctx->exec_enabled);
  std::string path = args.isMember("path") && args["path"].isString() ? args["path"].asString() : "";
  if (path.empty()) {
    path = use_mock ? "camera_capture.svg" : "camera_capture.jpg";
  }

  const auto resolved = resolve_under_root(ctx->root, path, ctx->unrestricted);
  if (!resolved) {
    return write_envelope(false, "invalid path", Json::Value(Json::objectValue));
  }
  if (!ctx->unrestricted && !ctx->allow_symlinks) {
    if (path_contains_symlink_component(ctx->root, *resolved)) {
      return write_envelope(false, "path escapes via symlink", Json::Value(Json::objectValue));
    }
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(resolved->parent_path(), ec);
    if (ec) {
      return write_envelope(false, "failed to create output directory", Json::Value(Json::objectValue));
    }
  }

  std::string chosen_backend = use_mock ? "mock" : (backend == "auto" ? "ffmpeg" : backend);
  if (chosen_backend == "ffmpeg") {
    if (!ctx->exec_enabled) {
      return write_envelope(false, "camera_capture(ffmpeg) requires exec-enabled sandbox (yolo)", Json::Value(Json::objectValue));
    }

    // Reuse proc_exec tool logic to run ffmpeg and capture output with timeout.
    Json::Value pargs(Json::objectValue);
    Json::Value argv(Json::arrayValue);
    argv.append("ffmpeg");
    argv.append("-f");
    argv.append("avfoundation");
    argv.append("-video_device_index");
    argv.append("0");
    argv.append("-i");
    argv.append("default");
    argv.append("-frames:v");
    argv.append("1");
    argv.append("-update");
    argv.append("1");
    argv.append("-y");
    argv.append(to_generic_string(*resolved));
    pargs["argv"] = argv;
    pargs["timeout_ms"] = std::max(1000, timeout_ms);
    pargs["max_output_bytes"] = 64 * 1024;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    agent_string_t tmp{};
    const agent_status_t est = tool_proc_exec(ctx, Json::writeString(wb, pargs).c_str(), &tmp);
    std::string tool_out = tmp.data ? std::string(tmp.data, tmp.len) : std::string();
    agent_string_free(&tmp);
    if (est != AGENT_OK) {
      return write_envelope(false, "ffmpeg backend failed to execute", Json::Value(Json::objectValue));
    }

    Json::Value env;
    std::string jerr;
    if (!parse_json(tool_out.c_str(), &env, &jerr) || !env.isObject()) {
      return write_envelope(false, "ffmpeg backend returned non-JSON", Json::Value(Json::objectValue));
    }
    const bool ok = env.isMember("ok") && env["ok"].isBool() ? env["ok"].asBool() : false;
    if (!ok) {
      const std::string e = env.isMember("error") && env["error"].isString() ? env["error"].asString() : "ffmpeg failed";
      Json::Value data(Json::objectValue);
      data["tool"] = "camera_capture";
      data["backend"] = chosen_backend;
      data["path"] = path;
      data["resolved_path"] = to_generic_string(*resolved);
      data["ffmpeg"] = env;
      data["output"] = "ffmpeg capture failed; try backend=mock or install/permit ffmpeg camera access";
      return write_envelope(false, e, data);
    }
  } else if (chosen_backend == "mock") {
    // Write a deterministic SVG so tests do not require camera hardware.
    std::ofstream out(*resolved, std::ios::binary);
    if (!out.is_open()) {
      return write_envelope(false, "failed to open output file", Json::Value(Json::objectValue));
    }
    const std::string label = title.empty() ? std::string("camera_capture mock") : title;
    const std::string text = svg_escape_text(label);
    out <<
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"480\" viewBox=\"0 0 640 480\">"
      "<rect width=\"640\" height=\"480\" fill=\"#111827\"/>"
      "<rect x=\"20\" y=\"20\" width=\"600\" height=\"440\" rx=\"12\" fill=\"#0b1220\" stroke=\"#334155\"/>"
      "<text x=\"40\" y=\"80\" fill=\"#e5e7eb\" font-family=\"ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace\" font-size=\"22\">"
      "mock capture"
      "</text>"
      "<text x=\"40\" y=\"120\" fill=\"#94a3b8\" font-family=\"ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace\" font-size=\"16\">"
      << text <<
      "</text>"
      "<text x=\"40\" y=\"160\" fill=\"#94a3b8\" font-family=\"ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace\" font-size=\"14\">"
      "file: " << svg_escape_text(to_generic_string(*resolved)) <<
      "</text>"
      "</svg>";
    out.close();
    if (!out) {
      return write_envelope(false, "failed to write output file", Json::Value(Json::objectValue));
    }
  } else {
    return write_envelope(false, "unsupported backend (expected: auto|ffmpeg|mock)", Json::Value(Json::objectValue));
  }

  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_regular_file(*resolved, ec)) {
    Json::Value data(Json::objectValue);
    data["tool"] = "camera_capture";
    data["backend"] = chosen_backend;
    data["path"] = path;
    data["resolved_path"] = to_generic_string(*resolved);
    data["output"] = "capture did not produce a regular file";
    return write_envelope(false, "capture failed", data);
  }

  const std::string kind = "image";
  const std::string mime = guess_mime_from_kind_and_ext(kind, path);

  Json::Value data(Json::objectValue);
  data["tool"] = "camera_capture";
  data["backend"] = chosen_backend;
  data["path"] = path;
  data["resolved_path"] = to_generic_string(*resolved);
  data["mime"] = mime;
  ec.clear();
  const uintmax_t sz = std::filesystem::file_size(*resolved, ec);
  if (!ec) data["size_bytes"] = (Json::UInt64)sz;

  if (register_artifact) {
    Json::Value artifact(Json::objectValue);
    artifact["path"] = path;
    artifact["resolved_path"] = to_generic_string(*resolved);
    artifact["root_dir"] = to_generic_string(ctx->root);
    artifact["unrestricted"] = ctx->unrestricted;
    artifact["kind"] = kind;
    artifact["mime"] = mime;
    if (!title.empty()) artifact["title"] = title;
    if (!ec) artifact["size_bytes"] = (Json::UInt64)sz;
    ec.clear();
    const auto mtime = std::filesystem::last_write_time(*resolved, ec);
    if (!ec) artifact["mtime_unix_ms"] = (Json::Int64)file_time_to_unix_ms(mtime);
    data["artifact"] = artifact;
  }

  if (notify) {
    Json::Value action(Json::objectValue);
    action["type"] = "notify";
    action["title"] = title.empty() ? "camera_capture" : title;
    action["message"] = std::string("captured: ") + path;
    data["action"] = action;
  }

  data["output"] = std::string("captured image via ") + chosen_backend + ": " + path;
  return write_envelope(true, "", data);
}
#else
agent_status_t tool_artifact_register(HostToolCtx* /*ctx*/, const char* /*arguments_json*/, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"artifact_register requires jsoncpp\",\"data\":{}}");
}

agent_status_t tool_ui_action(HostToolCtx* /*ctx*/, const char* /*arguments_json*/, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"ui_action requires jsoncpp\",\"data\":{}}");
}

agent_status_t tool_camera_capture(HostToolCtx* /*ctx*/, const char* /*arguments_json*/, agent_string_t* out_result) {
  return set_result(out_result, "{\"ok\":false,\"error\":\"camera_capture requires jsoncpp\",\"data\":{}}");
}
#endif

}  // namespace host_tools_internal
