#include "cli_transcript_writer.h"

#include <chrono>
#include <cstdio>
#include <sstream>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

CliTranscriptStyle cli_default_transcript_style(bool is_tty) {
  CliTranscriptStyle s;
  s.use_color = is_tty;
  s.dim_prefix = true;
  return s;
}

void CliTranscriptWriterMux::add(std::unique_ptr<CliTranscriptWriter> w) {
  if (!w) return;
  writers_.push_back(std::move(w));
}

void CliTranscriptWriterMux::on_event(const char* type, const char* data_json) {
  for (auto& w : writers_) {
    if (!w) continue;
    w->on_event(type, data_json);
  }
}

void CliTranscriptWriterMux::flush() {
  for (auto& w : writers_) {
    if (!w) continue;
    w->flush();
  }
}

#if defined(AGENT_HAVE_JSONCPP)
static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static const char* kAnsiReset = "\x1b[0m";
static const char* kAnsiDim = "\x1b[2m";
static const char* kAnsiBold = "\x1b[1m";
static const char* kAnsiRed = "\x1b[31m";
static const char* kAnsiGreen = "\x1b[32m";
static const char* kAnsiYellow = "\x1b[33m";
static const char* kAnsiCyan = "\x1b[36m";

static void write_styled(std::ostream& os, const CliTranscriptStyle& style, const char* ansi, const std::string& s) {
  if (style.use_color && ansi) os << ansi;
  os << s;
  if (style.use_color && ansi) os << kAnsiReset;
}

static bool looks_like_diff(const std::string& s) {
  if (s.find("diff --git") != std::string::npos) return true;
  if (s.find("\n@@ ") != std::string::npos || s.rfind("@@ ", 0) == 0) return true;
  if (s.find("\n--- ") != std::string::npos || s.rfind("--- ", 0) == 0) return true;
  if (s.find("\n+++ ") != std::string::npos || s.rfind("+++ ", 0) == 0) return true;
  return false;
}

bool CliPrettyConsoleWriter::parse_json_any(const std::string& s, Json::Value* out_v) {
  if (!out_v) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) return false;
  *out_v = v;
  return true;
}

bool CliPrettyConsoleWriter::parse_json_object(const std::string& s, Json::Value* out_obj) {
  Json::Value v;
  if (!parse_json_any(s, &v)) return false;
  if (!v.isObject()) return false;
  if (out_obj) *out_obj = v;
  return true;
}

std::string CliPrettyConsoleWriter::json_get_string(const Json::Value& obj, const char* key) {
  if (!obj.isObject() || !key) return "";
  const auto& v = obj[key];
  return v.isString() ? v.asString() : "";
}

int64_t CliPrettyConsoleWriter::json_get_i64(const Json::Value& obj, const char* key, int64_t def) {
  if (!obj.isObject() || !key) return def;
  const auto& v = obj[key];
  if (v.isInt64()) return v.asInt64();
  if (v.isUInt64()) return (int64_t)v.asUInt64();
  if (v.isInt()) return (int64_t)v.asInt();
  if (v.isUInt()) return (int64_t)v.asUInt();
  return def;
}

bool CliPrettyConsoleWriter::json_get_bool(const Json::Value& obj, const char* key, bool def) {
  if (!obj.isObject() || !key) return def;
  const auto& v = obj[key];
  return v.isBool() ? v.asBool() : def;
}

CliPrettyConsoleWriter::CliPrettyConsoleWriter(Options opt) : opt_(opt) {}

void CliPrettyConsoleWriter::flush() {
  if (opt_.out) opt_.out->flush();
  if (opt_.assistant_delta_out) opt_.assistant_delta_out->flush();
}

void CliPrettyConsoleWriter::print_dim(const std::string& s) {
  if (!opt_.out) return;
  write_styled(*opt_.out, opt_.style, kAnsiDim, s);
}

void CliPrettyConsoleWriter::print_error_line(const std::string& s) {
  if (!opt_.out) return;
  if (opt_.style.use_color) {
    write_styled(*opt_.out, opt_.style, kAnsiRed, s);
    (*opt_.out) << "\n";
  } else {
    (*opt_.out) << s << "\n";
  }
}

void CliPrettyConsoleWriter::print_ok_line(const std::string& s) {
  if (!opt_.out) return;
  if (opt_.style.use_color) {
    write_styled(*opt_.out, opt_.style, kAnsiGreen, s);
    (*opt_.out) << "\n";
  } else {
    (*opt_.out) << s << "\n";
  }
}

void CliPrettyConsoleWriter::print_header_line(const std::string& s) {
  if (!opt_.out) return;
  if (opt_.style.use_color) {
    write_styled(*opt_.out, opt_.style, kAnsiBold, s);
    (*opt_.out) << "\n";
  } else {
    (*opt_.out) << s << "\n";
  }
}

std::string CliPrettyConsoleWriter::join_argv(const Json::Value& argv) {
  if (!argv.isArray()) return "";
  std::string out;
  for (Json::ArrayIndex i = 0; i < argv.size(); i++) {
    const auto& a = argv[i];
    if (!a.isString()) continue;
    if (!out.empty()) out += " ";
    out += a.asString();
  }
  return out;
}

std::string CliPrettyConsoleWriter::format_tool_invocation(const std::string& tool_name, const std::string& args_json) {
  Json::Value args;
  if (!parse_json_object(args_json, &args)) {
    if (!args_json.empty()) return tool_name + " " + args_json;
    return tool_name;
  }

  if (tool_name == "shell_exec") {
    const std::string cmd = json_get_string(args, "cmd");
    const std::string cwd = json_get_string(args, "cwd");
    if (cmd.empty()) return "shell_exec";
    if (!cwd.empty()) return "cd " + cwd + " && " + cmd;
    return "$ " + cmd;
  }
  if (tool_name == "proc_exec") {
    const std::string a = join_argv(args["argv"]);
    const std::string cwd = json_get_string(args, "cwd");
    if (a.empty()) return "proc_exec";
    if (!cwd.empty()) return "cd " + cwd + " && " + a;
    return a;
  }
  if (tool_name == "fs_list") {
    const std::string p = json_get_string(args, "path");
    return p.empty() ? "ls ." : ("ls " + p);
  }
  if (tool_name == "fs_read") {
    const std::string p = json_get_string(args, "path");
    return p.empty() ? "cat <path>" : ("cat " + p);
  }
  if (tool_name == "fs_stat") {
    const std::string p = json_get_string(args, "path");
    return p.empty() ? "stat <path>" : ("stat " + p);
  }
  if (tool_name == "file_apply_patch") {
    return "apply patch";
  }

  // Generic: tool_name(key=value,...)
  std::string out = tool_name;
  const auto names = args.getMemberNames();
  if (!names.empty()) {
    out += "(";
    bool first = true;
    for (const auto& k : names) {
      if (!first) out += ", ";
      first = false;
      out += k;
      out += "=";
      const auto& v = args[k];
      if (v.isString()) out += v.asString();
      else if (v.isBool()) out += (v.asBool() ? "true" : "false");
      else if (v.isInt64()) out += std::to_string((long long)v.asInt64());
      else if (v.isUInt64()) out += std::to_string((unsigned long long)v.asUInt64());
      else out += "...";
    }
    out += ")";
  }
  return out;
}

std::string CliPrettyConsoleWriter::summarize_failure_reason(const std::string& /*tool_name*/, const std::string& tool_out) {
  Json::Value env;
  if (!parse_json_object(tool_out, &env)) {
    return tool_out.empty() ? "unknown error" : tool_out;
  }

  const bool ok = env.isMember("ok") && env["ok"].isBool() ? env["ok"].asBool() : false;
  const std::string err = json_get_string(env, "error");
  const auto& data = env["data"];

  if (!ok && !err.empty()) return err;
  if (data.isObject()) {
    if (data.isMember("timed_out") && data["timed_out"].isBool() && data["timed_out"].asBool()) {
      return "Timeout";
    }
    if (data.isMember("cancelled") && data["cancelled"].isBool() && data["cancelled"].asBool()) {
      return "Cancelled";
    }
    const int64_t exit_code = data.isMember("exit_code") ? json_get_i64(data, "exit_code", 0) : 0;
    if (exit_code != 0) return "Exit code " + std::to_string((long long)exit_code);
  }
  if (!ok) return "Failed";
  return "";
}

void CliPrettyConsoleWriter::print_output_block(std::ostream& os, const CliTranscriptStyle& style, const std::string& out) {
  if (out.empty()) return;
  const bool is_diff = looks_like_diff(out);
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    if (style.dim_prefix) {
      write_styled(os, style, kAnsiDim, "│ ");
    } else {
      os << "│ ";
    }
    if (!is_diff) {
      os << line << "\n";
      continue;
    }

    // Minimal diff highlighting (best-effort).
    if (line.rfind("diff --git", 0) == 0) {
      write_styled(os, style, kAnsiBold, line);
      os << "\n";
    } else if (line.rfind("@@ ", 0) == 0 || line.rfind("@@", 0) == 0) {
      write_styled(os, style, kAnsiCyan, line);
      os << "\n";
    } else if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0) {
      write_styled(os, style, kAnsiCyan, line);
      os << "\n";
    } else if (!line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0) {
      write_styled(os, style, kAnsiGreen, line);
      os << "\n";
    } else if (!line.empty() && line[0] == '-' && line.rfind("---", 0) != 0) {
      write_styled(os, style, kAnsiRed, line);
      os << "\n";
    } else {
      os << line << "\n";
    }
  }
}

void CliPrettyConsoleWriter::on_tool_call(const Json::Value& data) {
  if (!opt_.out) return;
  const std::string tool = json_get_string(data, "tool_name");
  const std::string args_json = json_get_string(data, "arguments_json");
  const int64_t step = json_get_i64(data, "step", -1);

  tool_index_ += 1;
  if (tool_index_ > 1) (*opt_.out) << "\n";
  const std::string inv = format_tool_invocation(tool, args_json);
  std::ostringstream hdr;
  hdr << "Tool";
  if (!tool.empty()) hdr << " " << tool;
  if (step >= 0) hdr << " (step " << (long long)step << ")";
  if (!inv.empty()) hdr << ": " << inv;
  print_header_line(hdr.str());
}

void CliPrettyConsoleWriter::on_tool_result(const Json::Value& data) {
  if (!opt_.out) return;
  const std::string tool = json_get_string(data, "tool_name");
  const std::string content = json_get_string(data, "content");

  // Parse tool output envelope (host/basic tools).
  Json::Value env;
  const bool has_env = parse_json_object(content, &env);
  const bool ok = has_env && env.isMember("ok") && env["ok"].isBool() ? env["ok"].asBool() : true;

  std::string primary_output;
  if (has_env && env.isMember("data") && env["data"].isObject()) {
    primary_output = json_get_string(env["data"], "output");
  }
  std::string patch_check_out;
  std::string patch_apply_out;
  if (has_env && tool == "file_apply_patch" && env.isMember("data") && env["data"].isObject()) {
    const auto& d = env["data"];
    const auto& check = d["check"];
    const auto& apply = d["apply"];
    patch_check_out = check.isObject() ? json_get_string(check, "output") : "";
    patch_apply_out = apply.isObject() ? json_get_string(apply, "output") : "";
  }
  const bool will_print_some_output = !primary_output.empty() || !patch_check_out.empty() || !patch_apply_out.empty() || !content.empty();

  if (!ok) {
    const std::string reason = summarize_failure_reason(tool, content);
    print_error_line(reason.empty() ? "ERROR" : ("ERROR: " + reason));
  } else if (!will_print_some_output) {
    // Avoid noisy "OK" when there's already useful output.
    print_ok_line("OK");
  }

  // Prefer printing captured "output" field; otherwise print tool-specific fields.
  if (has_env && env.isMember("data") && env["data"].isObject()) {
    const auto& d = env["data"];
    if (!primary_output.empty()) {
      print_output_block(*opt_.out, opt_.style, primary_output);
      return;
    }

    if (tool == "file_apply_patch") {
      if (!patch_check_out.empty()) {
        if (opt_.style.use_color) write_styled(*opt_.out, opt_.style, kAnsiDim, "check:\n");
        else (*opt_.out) << "check:\n";
        print_output_block(*opt_.out, opt_.style, patch_check_out);
      }
      if (!patch_apply_out.empty()) {
        if (opt_.style.use_color) write_styled(*opt_.out, opt_.style, kAnsiDim, "apply:\n");
        else (*opt_.out) << "apply:\n";
        print_output_block(*opt_.out, opt_.style, patch_apply_out);
      }
      return;
    }

    // Some tools stash their primary human output in other fields; show a compact summary.
    if (!ok) {
      // For failures, try to surface a small amount of context.
      const std::string cmd = json_get_string(d, "cmd");
      const std::string argv = d.isMember("argv") ? join_argv(d["argv"]) : "";
      if (!cmd.empty()) {
        print_dim("  cmd: " + cmd + "\n");
      } else if (!argv.empty()) {
        print_dim("  argv: " + argv + "\n");
      }
    }
    return;
  }

  // Fallback: print raw tool output (already bounded by tool limits).
  if (!content.empty()) {
    print_output_block(*opt_.out, opt_.style, content);
  }
}

void CliPrettyConsoleWriter::on_event(const char* type, const char* data_json) {
  if (!type) return;
  const std::string t(type);

  // Streaming assistant deltas go to stdout.
  if (t == "assistant_delta") {
    if (!opt_.stream_deltas || !opt_.assistant_delta_out) return;
    Json::Value data;
    if (data_json && data_json[0] && parse_json_any(std::string(data_json), &data)) {
      if (data.isObject() && data.isMember("delta") && data["delta"].isString()) {
        (*opt_.assistant_delta_out) << data["delta"].asString() << std::flush;
      }
    }
    return;
  }

  if (!opt_.out) return;

  // Keep the default transcript compact. Only print tool calls/results and explicit errors.
  Json::Value data;
  if (data_json && data_json[0]) {
    (void)parse_json_any(std::string(data_json), &data);
  }

  if (t == "tool_call") {
    if (data.isObject()) on_tool_call(data);
    return;
  }
  if (t == "tool_result") {
    if (data.isObject()) on_tool_result(data);
    return;
  }
  if (t == "error") {
    if (data.isObject()) {
      const std::string reason = json_get_string(data, "reason");
      const std::string err = json_get_string(data, "error");
      if (!reason.empty() || !err.empty()) {
        print_error_line("ERROR: " + (err.empty() ? reason : err));
      } else {
        print_error_line("ERROR");
      }
    } else {
      print_error_line("ERROR");
    }
    return;
  }

  // Suppress: start/llm_request/llm_response/assistant_message/done/compaction/retry/cancelled/etc.
}
#endif  // AGENT_HAVE_JSONCPP

CliJsonlFileWriter::CliJsonlFileWriter(const std::string& path) : path_(path) {
  if (path_.empty()) return;
  fp_ = std::fopen(path_.c_str(), "ab");
}

CliJsonlFileWriter::~CliJsonlFileWriter() {
  if (fp_) {
    std::fclose((FILE*)fp_);
    fp_ = nullptr;
  }
}

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void CliJsonlFileWriter::on_event(const char* type, const char* data_json) {
  if (!fp_) return;
  if (!type) return;

#if defined(AGENT_HAVE_JSONCPP)
  Json::Value root(Json::objectValue);
  root["ts_unix_ms"] = (Json::Int64)now_unix_ms();
  root["type"] = std::string(type);
  Json::Value data;
  if (data_json && data_json[0]) {
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss{std::string(data_json)};
    if (Json::parseFromStream(rb, iss, &data, &errs)) {
      root["data"] = data;
    } else {
      root["data_json"] = std::string(data_json);
      if (!errs.empty()) root["parse_error"] = errs;
    }
  } else {
    root["data"] = Json::Value(Json::objectValue);
  }

  const std::string line = json_stringify_compact(root) + "\n";
  (void)std::fwrite(line.data(), 1, line.size(), (FILE*)fp_);
#else
  // Best-effort line format without JSON.
  std::ostringstream oss;
  oss << "{\"ts_unix_ms\":" << (long long)now_unix_ms() << ",\"type\":\"" << type << "\"";
  if (data_json && data_json[0]) oss << ",\"data_json\":\"(unavailable without jsoncpp)\"";
  oss << "}\n";
  const std::string line = oss.str();
  (void)std::fwrite(line.data(), 1, line.size(), (FILE*)fp_);
#endif
}

void CliJsonlFileWriter::flush() {
  if (!fp_) return;
  std::fflush((FILE*)fp_);
}
