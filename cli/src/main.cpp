#include "agent/agent.h"
#include "agent/runner.h"

#include "file_persistor.h"
#include "openai_client.h"
#include "openai_provider.h"
#include "openai_stream_adapter.h"
#include "default_system_prompt.h"
#include "session_store.h"
#include "summary_compaction.h"
#include "summary_llm.h"
#include "tool_loop.h"
#include "toolset_basic.h"
#include "toolset_host.h"
#include "base64.h"
#include "cli_transcript_writer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include <chrono>
#include <cstring>
#if !defined(_WIN32)
#include <pwd.h>
#endif

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static bool is_tty_file(FILE* f) {
#if defined(_WIN32)
  if (!f) return false;
  return _isatty(_fileno(f)) != 0;
#else
  if (!f) return false;
  return isatty(fileno(f)) != 0;
#endif
}

static std::string lower_copy(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static bool url_contains_ci(const std::string& url, const std::string& needle) {
  if (needle.empty()) return false;
  const std::string u = lower_copy(url);
  const std::string n = lower_copy(needle);
  return u.find(n) != std::string::npos;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
#if !defined(_WIN32)
  if (struct passwd* pw = getpwuid(getuid())) {
    if (pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
  }
#endif
  // Fallback to current directory.
  return std::filesystem::current_path().string();
}

static bool looks_like_key(const std::string& s) {
  if (s.size() < 6) return false;
  if (s.rfind("sk-", 0) != 0) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static std::string try_load_key_from_dotenv_best_effort(const std::vector<std::string>& env_vars) {
  if (env_vars.empty()) return "";
  const std::filesystem::path p = std::filesystem::path(home_dir_best_effort()) / ".env";
  std::ifstream in(p);
  if (!in.is_open()) return "";

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string s = trim_copy(line);
    if (s.empty() || s[0] == '#') continue;
    if (s.rfind("export ", 0) == 0) s = trim_copy(s.substr(std::strlen("export ")));
    const size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trim_copy(s.substr(0, eq));
    const std::string v = trim_copy(s.substr(eq + 1));
    if (k.empty() || v.empty()) continue;
    for (const auto& want : env_vars) {
      if (k == want) {
        // Basic de-quoting.
        std::string vv = v;
        if (vv.size() >= 2 && ((vv.front() == '"' && vv.back() == '"') || (vv.front() == '\'' && vv.back() == '\''))) {
          vv = vv.substr(1, vv.size() - 2);
        }
        vv = trim_copy(vv);
        if (looks_like_key(vv)) return vv;
      }
    }
  }
  return "";
}

static bool has_env_key_best_effort(const std::vector<std::string>& env_vars) {
  for (const auto& k : env_vars) {
    const char* v = getenv_s(k.c_str());
    if (v && looks_like_key(v)) return true;
  }
  return false;
}

static bool has_env_or_dotenv_key_best_effort(const std::vector<std::string>& env_vars) {
  if (env_vars.empty()) return false;
  if (has_env_key_best_effort(env_vars)) return true;
  return !try_load_key_from_dotenv_best_effort(env_vars).empty();
}

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

#if defined(AGENT_HAVE_JSONCPP)
static void cli_transcript_on_tool_loop_event(void* vctx, const char* type, const char* data_json) {
  auto* w = static_cast<CliTranscriptWriter*>(vctx);
  if (!w) return;
  w->on_event(type, data_json);
}
#endif

#if defined(AGENT_HAVE_JSONCPP)
static const char* kMultimodalPrefix = "__AGENT_MM_V1__";

static bool read_file_bytes_capped(
  const std::filesystem::path& p,
  size_t max_bytes,
  std::string* out_bytes,
  bool* out_truncated = nullptr
) {
  if (!out_bytes) return false;
  out_bytes->clear();
  if (out_truncated) *out_truncated = false;

  std::error_code ec;
  const auto sz = std::filesystem::file_size(p, ec);
  if (!ec && max_bytes > 0 && sz > max_bytes) {
    if (out_truncated) *out_truncated = true;
  }

  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) return false;
  std::string buf;
  buf.resize(max_bytes);
  in.read(buf.data(), (std::streamsize)buf.size());
  const std::streamsize n = in.gcount();
  if (n <= 0) return false;
  buf.resize((size_t)n);
  *out_bytes = std::move(buf);
  return true;
}

static bool looks_texty(const std::string& bytes) {
  if (bytes.empty()) return false;
  size_t bad = 0;
  for (unsigned char c : bytes) {
    if (c == 0) return false;
    if (c == '\n' || c == '\r' || c == '\t') continue;
    if (c >= 32 && c < 127) continue;
    bad++;
  }
  // Allow a small amount of non-ASCII noise (e.g., UTF-8) without trying to decode.
  return bad * 20 < bytes.size(); // <5% suspicious bytes
}

static std::string content_type_from_path(const std::filesystem::path& p) {
  const std::string ext = lower_copy(p.extension().string());
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".wav") return "audio/wav";
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".webm") return "video/webm";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".txt" || ext == ".md") return "text/plain";
  return "application/octet-stream";
}

static bool try_parse_multimodal_prefix(
  const std::string& content,
  std::string* out_text,
  Json::Value* out_mm
) {
  if (out_text) *out_text = content;
  if (out_mm) *out_mm = Json::Value(Json::nullValue);
  if (!out_text || !out_mm) return false;

  if (content.rfind(kMultimodalPrefix, 0) != 0) return false;
  const size_t nl = content.find('\n');
  if (nl == std::string::npos) return false;
  const size_t prefix_len = std::strlen(kMultimodalPrefix);
  if (nl < prefix_len) return false;
  const std::string json_part = content.substr(prefix_len, nl - prefix_len);
  if (json_part.empty()) return false;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(json_part);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) return false;

  *out_mm = v;
  *out_text = content.substr(nl + 1);
  return true;
}

static Json::Value multimodal_content_from_parts(const std::string& text, const Json::Value& mm) {
  const bool have_images = mm.isMember("images") && mm["images"].isArray() && !mm["images"].empty();
  const bool have_files = mm.isMember("files") && mm["files"].isArray() && !mm["files"].empty();
  if (!have_images && !have_files) return Json::Value(text);

  Json::Value arr(Json::arrayValue);
  if (!text.empty()) {
    Json::Value t(Json::objectValue);
    t["type"] = "text";
    t["text"] = text;
    arr.append(t);
  }

  if (have_files) {
    for (const auto& f : mm["files"]) {
      if (!f.isObject()) continue;
      const std::string name = f.isMember("name") && f["name"].isString() ? f["name"].asString() : "";
      const std::string mime = f.isMember("mime") && f["mime"].isString() ? f["mime"].asString() : "";
      const std::string ft = f.isMember("text") && f["text"].isString() ? f["text"].asString() : "";
      const bool trunc = f.isMember("truncated") && f["truncated"].isBool() ? f["truncated"].asBool() : false;
      if (ft.empty()) continue;
      std::string block;
      block += "[Attachment";
      if (!name.empty()) block += ": " + name;
      if (!mime.empty()) block += " (" + mime + ")";
      block += "]\n";
      block += ft;
      if (trunc) block += "\n...(truncated)";

      Json::Value t(Json::objectValue);
      t["type"] = "text";
      t["text"] = block;
      arr.append(t);
    }
  }

  if (have_images) {
    for (const auto& im : mm["images"]) {
      if (!im.isObject()) continue;
      const std::string mime = im.isMember("mime") && im["mime"].isString() ? im["mime"].asString() : "image/png";
      const std::string b64 = im.isMember("b64") && im["b64"].isString() ? im["b64"].asString() : "";
      if (b64.empty()) continue;
      const std::string url = std::string("data:") + mime + ";base64," + b64;

      Json::Value part(Json::objectValue);
      part["type"] = "image_url";
      Json::Value iu(Json::objectValue);
      iu["url"] = url;
      part["image_url"] = iu;
      arr.append(part);
    }
  }
  return arr;
}

static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

static std::string wrap_prompt_with_attachments(
  const std::string& prompt,
  const std::vector<std::string>& attach_paths,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (attach_paths.empty()) return prompt;

  Json::Value mm(Json::objectValue);
  Json::Value images(Json::arrayValue);
  Json::Value files(Json::arrayValue);

  const size_t kMaxAttachments = 16;
  for (size_t i = 0; i < attach_paths.size() && i < kMaxAttachments; i++) {
    const std::string raw = attach_paths[i];
    if (raw.empty()) continue;
    const std::filesystem::path p = std::filesystem::path(raw);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) {
      if (out_error) *out_error = "attachment not found: " + raw;
      continue;
    }
    const std::string name = p.filename().string();
    const std::string mime = content_type_from_path(p);
    const std::string kind = (mime.rfind("image/", 0) == 0) ? "image" : "file";

    if (kind == "image") {
      const size_t kMaxImageBytes = 6u * 1024u * 1024u;
      std::string bytes;
      bool trunc = false;
      if (!read_file_bytes_capped(p, kMaxImageBytes, &bytes, &trunc) || bytes.empty() || trunc) {
        // Avoid sending partial image bytes (invalid base64/image). Treat as a non-inlined file hint instead.
        Json::Value f(Json::objectValue);
        f["name"] = name;
        f["mime"] = mime;
        f["text"] = std::string("Image attachment too large to inline: ") + raw;
        f["truncated"] = true;
        files.append(f);
      } else {
        const std::string b64 = base64_encode(bytes.data(), bytes.size());
        Json::Value im(Json::objectValue);
        im["name"] = name;
        im["mime"] = mime.empty() ? std::string("image/png") : mime;
        im["b64"] = b64;
        images.append(im);
      }
      continue;
    }

    // Non-image: inline small text files, otherwise add a hint.
    const size_t kMaxFileBytes = 256u * 1024u;
    std::string bytes;
    bool trunc = false;
    if (read_file_bytes_capped(p, kMaxFileBytes, &bytes, &trunc) && looks_texty(bytes)) {
      Json::Value f(Json::objectValue);
      f["name"] = name;
      f["mime"] = mime;
      f["text"] = bytes;
      f["truncated"] = trunc;
      files.append(f);
    } else {
      Json::Value f(Json::objectValue);
      f["name"] = name;
      f["mime"] = mime;
      f["text"] = std::string("Attachment available at path: ") + raw;
      f["truncated"] = true;
      files.append(f);
    }
  }

  if (!images.empty()) mm["images"] = images;
  if (!files.empty()) mm["files"] = files;
  if (mm.empty()) return prompt;
  return std::string(kMultimodalPrefix) + json_stringify(mm) + "\n" + prompt;
}
#endif  // AGENT_HAVE_JSONCPP

#if defined(AGENT_HAVE_JSONCPP)
// CLI output is designed to be human-readable and audit-friendly (Codex/Gemini style):
// - show prompts, tool calls, tool outputs, and final answers
// - avoid JSON in the UI output (unless the tool output itself is JSON)
// - do not print chain-of-thought / reasoning tokens
#endif  // AGENT_HAVE_JSONCPP

static void usage() {
  std::cerr
    << "Usage:\n"
    << "  agent run \"prompt text\" [options]\n"
    << "  agent chat [options]\n\n"
    << "Options:\n"
    << "  --model <name>            Model name (default: gpt-4o-mini)\n"
    << "  --base-url <url>          API base url (default: https://api.openai.com/v1)\n"
    << "  --api-key <key>           API key (default: OPENAI_API_KEY)\n"
    << "  --proxy <url>             Optional HTTP proxy override (else env HTTPS_PROXY/http_proxy)\n"
    << "  --timeout-ms <n>          HTTP timeout in ms (default: 60000)\n"
    << "  --trace                   Print full request/response/tool transcript to stderr (default: on)\n"
    << "  --quiet                   Suppress transcript; print assistant text only\n"
    << "  --session <id>            Session id to load/save (default: default)\n"
    << "  --no-session              Disable persistence (ephemeral run)\n"
    << "  --system <text>           Add a system message at the start (one time)\n"
    << "  --system-profile <name>   Select built-in host system prompt (default: default)\n"
    << "  --no-default-system       Disable the default host system hint (host tools only)\n"
    << "  --max-chars <n>           Auto-compact when session exceeds n chars (default: 20000)\n"
    << "  --keep-last <n>           Keep last n messages during compaction (default: 16)\n"
    << "  --summary-model <name>    Optional model used to summarize dropped messages during compaction (tools=none)\n"
    << "  --summary-max-chars <n>   Max chars for inserted summary system message (default: 1200)\n"
    << "  --tools none|basic|host   Select toolset (default: host)\n"
    << "  --tools-root <path>       Root/working dir for host file edits (file_apply_patch) (default: current dir)\n"
    << "  --host-policy full|readonly  Host tool safety policy (default: full; host tools only)\n"
    << "  --force-tool <name>       Force a tool call on first step (verification)\n"
    << "  --require-tool-call       Fail if no tool call occurred\n"
    << "  --max-steps <n>           Max tool loop steps (default: unlimited; 0 means unlimited)\n"
    << "  --max-repeated-tool-calls <n>  Stop runaway loops when repeating identical tool calls (default: 0; 0 disables)\n"
    << "  --max-tool-calls-total <n>     Max total tool calls (default: unlimited; 0 means unlimited)\n"
    << "  --max-tool-calls-per-tool <n>  Max tool calls per tool name (default: unlimited; 0 means unlimited)\n"
    << "  --max-tool-call-args-chars <n> Max tool call arguments JSON length (default: unlimited; 0 means unlimited)\n"
    << "  --tool-call-limit <tool>=<n>  Per-tool call cap (repeatable; 0 means unlimited for that tool)\n"
    << "  --attach <path>           Attach a local file (repeatable; images are sent as base64)\n"
    << "  --stream-assistant        Stream assistant deltas (tools=none|basic|host; provider-dependent)\n"
    << "  --transcript-jsonl <path> Write tool-loop events as JSONL (append)\n";
}

static bool take_switch(std::vector<std::string>& args, const std::string& flag, bool* out_enabled) {
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == flag) {
      *out_enabled = true;
      args.erase(args.begin() + (long)i);
      return true;
    }
  }
  return true;
}

static bool take_flag(std::vector<std::string>& args, const std::string& flag, std::string* out_value) {
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == flag) {
      if (i + 1 >= args.size()) {
        return false;
      }
      *out_value = args[i + 1];
      args.erase(args.begin() + (long)i, args.begin() + (long)i + 2);
      return true;
    }
  }
  return true;
}

static bool take_multi_flag(std::vector<std::string>& args, const std::string& flag, std::vector<std::string>* out_values) {
  if (!out_values) return false;
  for (size_t i = 0; i < args.size();) {
    if (args[i] == flag) {
      if (i + 1 >= args.size()) {
        return false;
      }
      out_values->push_back(args[i + 1]);
      args.erase(args.begin() + (long)i, args.begin() + (long)i + 2);
      continue;
    }
    i++;
  }
  return true;
}

static bool parse_tool_call_limit_spec(const std::string& spec, ToolCallLimit* out_limit) {
  if (!out_limit) return false;
  *out_limit = ToolCallLimit{};
  const std::string s = trim_copy(spec);
  const size_t eq = s.find('=');
  if (eq == std::string::npos) return false;
  const std::string tool = trim_copy(s.substr(0, eq));
  const std::string num = trim_copy(s.substr(eq + 1));
  if (tool.empty() || num.empty()) return false;
  try {
    out_limit->tool = tool;
    out_limit->max_calls = (size_t)std::stoull(num);
    return true;
  } catch (...) {
    return false;
  }
}

static bool take_flag_u64(std::vector<std::string>& args, const std::string& flag, size_t* out_value) {
  std::string v;
  if (!take_flag(args, flag, &v)) {
    return false;
  }
  if (v.empty()) {
    return true;
  }
  try {
    *out_value = static_cast<size_t>(std::stoull(v));
  } catch (...) {
    return false;
  }
  return true;
}

static bool take_enum(std::vector<std::string>& args, const std::string& flag, std::string* out_value) {
  std::string v;
  if (!take_flag(args, flag, &v)) {
    return false;
  }
  if (!v.empty()) {
    *out_value = v;
  }
  return true;
}

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve((size_t)argc);
  for (int i = 1; i < argc; i++) {
    args.emplace_back(argv[i]);
  }

  if (args.empty()) {
    usage();
    return 2;
  }

  const std::string cmd = args[0];
  args.erase(args.begin());
  if (cmd != "run" && cmd != "chat") {
    usage();
    return 2;
  }

  std::string model = "gpt-4o-mini";
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string proxy_url;
  std::string session_id = "default";
  bool no_session = false;
  std::string system_msg;
  std::string system_profile = "default";
  bool no_default_system = false;
  size_t max_chars = 20000;
  size_t keep_last = 16;
  std::string summary_model;
  size_t summary_max_chars = 1200;
  size_t timeout_ms = 60000;
  std::string tools_mode = "host";
  std::string tools_root; // empty => unrestricted (YOLO)
  std::string host_policy = "full";
  std::string force_tool;
  bool require_tool_call = false;
  size_t max_steps = 0; // unlimited unless explicitly set
  size_t max_repeated_tool_calls = 0;
  size_t max_tool_calls_total = 0;
  size_t max_tool_calls_per_tool = 0;
  size_t max_tool_call_args_chars = 0;
  std::vector<std::string> tool_call_limit_specs;
  std::vector<std::string> attach_paths;
  bool stream_assistant = false;
  bool trace = true;
  bool quiet = false;
  std::string transcript_jsonl;

  if (!take_flag(args, "--model", &model)) {
    std::cerr << "Missing value for --model\n";
    return 2;
  }
  if (!take_flag(args, "--base-url", &base_url)) {
    std::cerr << "Missing value for --base-url\n";
    return 2;
  }
  if (!take_flag(args, "--api-key", &api_key)) {
    std::cerr << "Missing value for --api-key\n";
    return 2;
  }
  if (!take_flag(args, "--proxy", &proxy_url)) {
    std::cerr << "Missing value for --proxy\n";
    return 2;
  }
  if (!take_flag_u64(args, "--timeout-ms", &timeout_ms)) {
    std::cerr << "Invalid value for --timeout-ms\n";
    return 2;
  }
  if (!take_switch(args, "--trace", &trace)) {
    std::cerr << "Invalid flag: --trace\n";
    return 2;
  }
  if (!take_switch(args, "--quiet", &quiet)) {
    std::cerr << "Invalid flag: --quiet\n";
    return 2;
  }
  if (quiet) {
    trace = false;
  }
  if (!take_switch(args, "--no-session", &no_session)) {
    std::cerr << "Invalid flag: --no-session\n";
    return 2;
  }
  if (!take_flag(args, "--session", &session_id)) {
    std::cerr << "Missing value for --session\n";
    return 2;
  }
  if (!take_flag(args, "--system", &system_msg)) {
    std::cerr << "Missing value for --system\n";
    return 2;
  }
  if (!take_flag(args, "--system-profile", &system_profile)) {
    std::cerr << "Missing value for --system-profile\n";
    return 2;
  }
  system_profile = trim_copy(system_profile);
  if (!system_profile.empty() && system_profile != "default" && system_profile != "jules_codex") {
    std::cerr << "Invalid --system-profile (expected: default|jules_codex)\n";
    return 2;
  }
  if (!take_switch(args, "--no-default-system", &no_default_system)) {
    std::cerr << "Invalid flag: --no-default-system\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-chars", &max_chars)) {
    std::cerr << "Invalid value for --max-chars\n";
    return 2;
  }
  if (!take_flag_u64(args, "--keep-last", &keep_last)) {
    std::cerr << "Invalid value for --keep-last\n";
    return 2;
  }
  if (!take_flag(args, "--summary-model", &summary_model)) {
    std::cerr << "Missing value for --summary-model\n";
    return 2;
  }
  if (!take_flag_u64(args, "--summary-max-chars", &summary_max_chars)) {
    std::cerr << "Invalid value for --summary-max-chars\n";
    return 2;
  }
  if (!take_enum(args, "--tools", &tools_mode)) {
    std::cerr << "Missing value for --tools\n";
    return 2;
  }
  if (!take_flag(args, "--tools-root", &tools_root)) {
    std::cerr << "Missing value for --tools-root\n";
    return 2;
  }
  if (!take_enum(args, "--host-policy", &host_policy)) {
    std::cerr << "Missing value for --host-policy\n";
    return 2;
  }
  if (!take_flag(args, "--force-tool", &force_tool)) {
    std::cerr << "Missing value for --force-tool\n";
    return 2;
  }
  if (!take_switch(args, "--require-tool-call", &require_tool_call)) {
    std::cerr << "Invalid flag: --require-tool-call\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-steps", &max_steps)) {
    std::cerr << "Invalid value for --max-steps\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-repeated-tool-calls", &max_repeated_tool_calls)) {
    std::cerr << "Invalid value for --max-repeated-tool-calls\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-tool-calls-total", &max_tool_calls_total)) {
    std::cerr << "Invalid value for --max-tool-calls-total\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-tool-calls-per-tool", &max_tool_calls_per_tool)) {
    std::cerr << "Invalid value for --max-tool-calls-per-tool\n";
    return 2;
  }
  if (!take_flag_u64(args, "--max-tool-call-args-chars", &max_tool_call_args_chars)) {
    std::cerr << "Invalid value for --max-tool-call-args-chars\n";
    return 2;
  }
  if (!take_multi_flag(args, "--tool-call-limit", &tool_call_limit_specs)) {
    std::cerr << "Missing value for --tool-call-limit\n";
    return 2;
  }
  if (!take_multi_flag(args, "--attach", &attach_paths)) {
    std::cerr << "Missing value for --attach\n";
    return 2;
  }
  if (!take_switch(args, "--stream-assistant", &stream_assistant)) {
    std::cerr << "Invalid flag: --stream-assistant\n";
    return 2;
  }
  if (!take_flag(args, "--transcript-jsonl", &transcript_jsonl)) {
    std::cerr << "Missing value for --transcript-jsonl\n";
    return 2;
  }

  std::string prompt;
  if (cmd == "run") {
    // Remaining args should be the prompt (single token or quoted string as one arg).
    if (args.empty()) {
      std::cerr << "Missing prompt\n";
      return 2;
    }
    prompt = args[0];
  }

  std::vector<ToolCallLimit> tool_call_limits;
  tool_call_limits.reserve(tool_call_limit_specs.size());
  for (const auto& spec : tool_call_limit_specs) {
    ToolCallLimit lim;
    if (!parse_tool_call_limit_spec(spec, &lim) || lim.tool.empty()) {
      std::cerr << "Invalid --tool-call-limit (expected: tool=max_calls): " << spec << "\n";
      return 2;
    }
    // Upsert (last wins).
    bool replaced = false;
    for (auto& x : tool_call_limits) {
      if (x.tool == lim.tool) {
        x.max_calls = lim.max_calls;
        replaced = true;
        break;
      }
    }
    if (!replaced) tool_call_limits.push_back(std::move(lim));
  }

  // Host-side env defaults (core remains env-free).
  if (base_url == "https://api.openai.com/v1") {
    if (const char* b = getenv_s("OPENAI_API_BASE")) {
      base_url = b;
    } else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) {
      base_url = b2;
    } else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) {
      base_url = b3;
    } else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) {
      base_url = b4;
    } else if (const char* b5 = getenv_s("MOONSHOT_API_BASE")) {
      base_url = b5;
    }
  }
  // Pick the API key that matches the chosen base URL. This prevents accidental mix-ups when the
  // host environment exports multiple provider keys.
  if (api_key.empty()) {
    if (url_contains_ci(base_url, "deepseek")) {
      if (const char* k = getenv_s("DEEPSEEK_API_KEY")) api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) api_key = k3;
    } else if (url_contains_ci(base_url, "openrouter")) {
      if (const char* k = getenv_s("OPENROUTER_API_KEY")) api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) api_key = k3;
    } else if (url_contains_ci(base_url, "moonshot")) {
      if (const char* k = getenv_s("KIMI_API_KEY_CN")) api_key = k;
      else if (const char* k2 = getenv_s("MOONSHOT_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("OPENAI_API_KEY")) api_key = k3;
    } else {
      if (const char* k = getenv_s("OPENAI_API_KEY")) api_key = k;
      else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) api_key = k3;
    }
  }

  if (api_key.empty()) {
    // Best-effort developer convenience: allow non-exported keys stored in ~/.env.
    // (Many setups keep `KIMI_API_KEY_CN=...` without `export`.)
    if (url_contains_ci(base_url, "moonshot")) {
      api_key = try_load_key_from_dotenv_best_effort({"KIMI_API_KEY_CN", "MOONSHOT_API_KEY", "MOONSHOT_API_KEY_CN"});
    } else if (url_contains_ci(base_url, "deepseek")) {
      api_key = try_load_key_from_dotenv_best_effort({"DEEPSEEK_API_KEY"});
    } else if (url_contains_ci(base_url, "openrouter")) {
      api_key = try_load_key_from_dotenv_best_effort({"OPENROUTER_API_KEY"});
    } else {
      api_key = try_load_key_from_dotenv_best_effort({"OPENAI_API_KEY"});
    }
  }

  if (api_key.empty()) {
    std::cerr << "Missing API key for base URL: " << base_url << "\n";
    if (url_contains_ci(base_url, "openrouter")) {
      std::cerr << "Provide --api-key or set OPENROUTER_API_KEY";
      if (has_env_or_dotenv_key_best_effort({"KIMI_API_KEY_CN", "MOONSHOT_API_KEY", "MOONSHOT_API_KEY_CN"})) {
        std::cerr << " (note: Moonshot key detected; OpenRouter requires OPENROUTER_API_KEY)";
      }
      std::cerr << ".\n";
    } else if (url_contains_ci(base_url, "moonshot")) {
      std::cerr << "Provide --api-key or set KIMI_API_KEY_CN / MOONSHOT_API_KEY / MOONSHOT_API_KEY_CN.\n";
    } else if (url_contains_ci(base_url, "deepseek")) {
      std::cerr << "Provide --api-key or set DEEPSEEK_API_KEY.\n";
    } else {
      std::cerr << "Provide --api-key or set OPENAI_API_KEY.\n";
    }
    return 2;
  }

  // Session load (mandatory for CLI/daemon use cases, but optional via flag).
  agent_session_t* session = nullptr;
  SessionStoreConfig store_cfg;
  store_cfg.root_dir = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();

  agent_persistor_t persistor{};
  if (agent_file_persistor_create(store_cfg.root_dir.c_str(), &persistor) != AGENT_OK) {
    std::cerr << "Failed to initialize persistor\n";
    return 1;
  }

  if (!no_session && !session_id.empty()) {
    const agent_status_t st = persistor.load(persistor.ctx, session_id.c_str(), &session);
    if (st != AGENT_OK) {
      std::cerr << "Failed to load session: " << (int)st << "\n";
      agent_persistor_destroy(&persistor);
      return 1;
    }
  } else {
    const agent_status_t st = agent_session_create(&session);
    if (st != AGENT_OK) {
      std::cerr << "Failed to create session: " << (int)st << "\n";
      agent_persistor_destroy(&persistor);
      return 1;
    }
  }

  // Add one-time system message if requested and session is empty.
  if (!system_msg.empty() && agent_session_message_count(session) == 0) {
    agent_session_add_message(session, AGENT_ROLE_SYSTEM, system_msg.c_str());
  }

  OpenAIProviderCtx pctx;
  pctx.cfg.base_url = base_url;
  pctx.cfg.api_key = api_key;
  pctx.cfg.model = model;
  pctx.cfg.proxy_url = proxy_url;
  pctx.cfg.timeout_ms = (long)timeout_ms;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) {
    pctx.cfg.openrouter_http_referer = r;
  }
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) {
    pctx.cfg.openrouter_x_title = t;
  }

  if (tools_mode == "yolo") {
    tools_mode = "host";
  }
  if (host_policy != "full" && host_policy != "readonly") {
    std::cerr << "Unsupported --host-policy: " << host_policy << "\n";
    return 2;
  }

  // Add host-only default system message (one-time) when using host tools and the session is empty.
  // This encourages incremental inspection (rg/head/awk) instead of full file dumps.
  if (!no_default_system && system_msg.empty() && tools_mode == "host" && agent_session_message_count(session) == 0) {
    const std::string p = system_profile.empty() ? "default" : system_profile;
    agent_session_add_message(session, AGENT_ROLE_SYSTEM, host_system_prompt_for_profile(p.c_str()));
  }

  if (tools_mode != "none") {
    agent_tool_registry_t* registry = nullptr;
    agent_tool_executor_t executor{};
    bool need_destroy_executor = false;
    if (tools_mode == "basic") {
      if (toolset_basic_create(&registry, &executor) != AGENT_OK) {
        std::cerr << "Failed to initialize toolset\n";
        agent_session_destroy(session);
        return 1;
      }
    } else if (tools_mode == "host") {
      HostToolsetConfig cfg;
      cfg.root_dir = tools_root;
      cfg.policy = (host_policy == "readonly") ? HostToolsetPolicyMode::ReadOnly : HostToolsetPolicyMode::Full;
      cfg.enable_process_exec = true;
      cfg.allow_symlinks = true;
      if (toolset_host_create(cfg, &registry, &executor) != AGENT_OK) {
        std::cerr << "Failed to initialize host toolset\n";
        agent_session_destroy(session);
        return 1;
      }
      need_destroy_executor = true;
    } else {
      std::cerr << "Unsupported --tools mode: " << tools_mode << "\n";
      agent_session_destroy(session);
      return 2;
    }

    ToolLoopOptions opt;
    opt.force_tool = force_tool;
    opt.require_tool_call = require_tool_call;
    opt.max_steps = max_steps;
    opt.max_repeated_tool_calls = max_repeated_tool_calls;
    opt.max_tool_calls_total = max_tool_calls_total;
    opt.max_tool_calls_per_tool = max_tool_calls_per_tool;
    opt.max_tool_call_args_chars = max_tool_call_args_chars;
    opt.tool_call_limits = std::move(tool_call_limits);
	    opt.max_chars = max_chars;
	    opt.keep_last_messages = keep_last;
	    opt.stream_assistant = stream_assistant;
	    // For CLI trace output, we prefer human-friendly (non-JSON) printing based on tool loop events.
	    // Enable verbose events so we have tool arguments + tool outputs available to print.
	    opt.verbose = (trace && !quiet) || !transcript_jsonl.empty();
	    opt.max_capture_bytes = (size_t)1024u * 1024u;

	    auto run_one = [&](const std::string& user_prompt) -> int {
	      std::string prompt_for_llm = user_prompt;
#if defined(AGENT_HAVE_JSONCPP)
	      if (!attach_paths.empty()) {
        std::string mm_err;
        prompt_for_llm = wrap_prompt_with_attachments(user_prompt, attach_paths, &mm_err);
        (void)mm_err;
      }
#endif
	      ToolLoopResult r;
	      std::string err;
	      long http_status = 0;
	      std::string http_body;
	      std::ostream* trace_stream = nullptr; // avoid dumping raw JSON traces by default
#if defined(AGENT_HAVE_JSONCPP)
	      CliTranscriptWriterMux mux;
	      if (stream_assistant || (trace && !quiet)) {
	        CliPrettyConsoleWriter::Options wopt;
	        wopt.out = (trace && !quiet) ? &std::cerr : nullptr;
	        wopt.assistant_delta_out = &std::cout;
	        wopt.stream_deltas = stream_assistant;
	        wopt.style = cli_default_transcript_style(is_tty_file(stderr));
	        mux.add(std::make_unique<CliPrettyConsoleWriter>(wopt));
	      }
	      if (!transcript_jsonl.empty()) {
	        mux.add(std::make_unique<CliJsonlFileWriter>(transcript_jsonl));
	      }

	      // Important: even in --quiet, we may need on_event enabled for assistant deltas / JSONL.
	      const bool want_events = stream_assistant || (trace && !quiet) || !transcript_jsonl.empty();
	      opt.on_event = want_events ? cli_transcript_on_tool_loop_event : nullptr;
	      opt.on_event_ctx = want_events ? (void*)&mux : nullptr;
#endif
	      if (!run_tool_loop(pctx.cfg, session, prompt_for_llm, registry, &executor, opt, trace_stream, &r, &err, &http_status, &http_body)) {
	        if (http_status) {
	          std::cerr << openai_format_http_error(http_status, http_body) << "\n";
        } else if (!err.empty()) {
          std::cerr << "Tool loop failed: " << err << "\n";
        } else {
          std::cerr << "Tool loop failed\n";
        }
        return 1;
      }

      // Persist a portable transcript into the session:
      // - user prompt
      // - final assistant message
      agent_session_add_message(session, AGENT_ROLE_USER, user_prompt.c_str());
      agent_session_add_message(session, AGENT_ROLE_ASSISTANT, r.final_assistant_text.c_str());

      // Persist detailed tool timeline to the per-session audit log (host-only).
      if (!no_session && !session_id.empty()) {
#if defined(AGENT_HAVE_JSONCPP)
        Json::Value record(Json::objectValue);
        record["ts_unix_ms"] = (Json::Int64)now_unix_ms();
        record["prompt"] = user_prompt;
        record["assistant_text"] = r.final_assistant_text;
        record["tools"] = tools_mode;
        record["model"] = model;
        record["base_url"] = pctx.cfg.base_url;
        if (!r.events_json.empty()) {
          Json::CharReaderBuilder rb;
          std::string errs;
          std::istringstream iss(r.events_json);
          Json::Value ev;
          if (Json::parseFromStream(rb, iss, &ev, &errs) && ev.isArray()) {
            record["events"] = ev;
          } else {
            record["events_json"] = r.events_json;
          }
        }
        if (!r.tool_records.empty()) {
          Json::Value tr(Json::arrayValue);
          for (const auto& rec : r.tool_records) {
            Json::Value t(Json::objectValue);
            t["tool_name"] = rec.tool_name;
            if (!rec.tool_call_id.empty()) t["tool_call_id"] = rec.tool_call_id;
            if (!rec.arguments_json.empty()) t["arguments_json"] = rec.arguments_json;
            const std::string out_for_audit = rec.result_string_for_prompt.empty() ? rec.result_string : rec.result_string_for_prompt;
            if (!out_for_audit.empty()) t["result"] = out_for_audit;
            tr.append(t);
          }
          record["tool_records"] = tr;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        (void)session_store_append_audit_jsonl(store_cfg, session_id, Json::writeString(wb, record));
#endif
      }
#if defined(AGENT_HAVE_JSONCPP)
	      if (stream_assistant) {
	        // Deltas were already printed; terminate with a newline for shell ergonomics.
	        std::cout << "\n";
	      } else {
	        std::cout << r.final_assistant_text << "\n";
	      }
#else
	      std::cout << r.final_assistant_text << "\n";
#endif
	      return 0;
	    };

    int rc = 0;
    if (cmd == "run") {
      rc = run_one(prompt);
    } else {
      std::string line;
      while (true) {
        std::cerr << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
          break;
        }
        if (line == "/exit" || line == "/quit") {
          break;
        }
        if (line.empty()) {
          continue;
        }
        rc = run_one(line);
        if (rc != 0) {
          break;
        }
      }
    }

    if (!no_session && !session_id.empty()) {
      const agent_status_t st = persistor.save(persistor.ctx, session_id.c_str(), session);
      if (st != AGENT_OK) {
        std::cerr << "Failed to save session: " << (int)st << "\n";
        rc = 1;
      }
    }

    agent_tool_registry_destroy(registry);
    if (need_destroy_executor) {
      toolset_host_destroy(&executor);
    }
    agent_session_destroy(session);
    agent_persistor_destroy(&persistor);
    return rc;
  } else {
    auto run_one = [&](const std::string& user_prompt) -> int {
#if defined(AGENT_HAVE_JSONCPP)
      std::string prompt_for_llm = user_prompt;
      if (!attach_paths.empty()) {
        std::string mm_err;
        prompt_for_llm = wrap_prompt_with_attachments(user_prompt, attach_paths, &mm_err);
      }
      const bool have_mm = (prompt_for_llm != user_prompt);
#endif
      if (stream_assistant) {
#if !defined(AGENT_HAVE_JSONCPP)
        std::cerr << "--stream-assistant requires jsoncpp (AGENT_HAVE_JSONCPP)\n";
        return 2;
#else
        agent_session_add_message(session, AGENT_ROLE_USER, user_prompt.c_str());

        const OpenAIClientConfig run_cfg = pctx.cfg;
        const size_t keep = (keep_last == 0 ? 16 : keep_last);

        size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
        int final_attempt = -1;
        bool ok = false;
        long http_status = 0;
        std::string http_body;
        std::string err;
        std::string assistant_text;
        agent_compact_report_t compact_final{};
        bool saw_stream_delta = false;

        for (int attempt = 0; attempt < 3; attempt++) {
          final_attempt = attempt;

          const char* summary_or_null = nullptr;
          std::string summary_buf;
          if (attempt == 0 && !summary_model.empty() && agent_session_estimated_chars(session) > attempt_max_chars) {
            SummaryCompactionInput input = build_summary_compaction_input(session, keep);
            if (input.dropped_messages > 0 && !input.excerpt.empty()) {
              const size_t max_out = summary_max_chars == 0 ? 1200 : summary_max_chars;
              CompactionSummaryResult sr = generate_compaction_summary_via_llm(run_cfg, summary_model, input, max_out);
              if (sr.ok && !sr.summary_text.empty()) {
                summary_buf = std::string(AGENT_SESSION_SUMMARY_PREFIX) + "\n" + sr.summary_text;
                summary_or_null = summary_buf.c_str();
                if (trace) {
                  std::cerr << "=== SUMMARY MODEL ===\n";
                  std::cerr << "model=" << summary_model << " dropped_messages=" << input.dropped_messages
                            << " excerpt_truncated=" << (input.truncated ? "true" : "false") << "\n";
                }
              } else if (trace) {
                std::cerr << "=== SUMMARY MODEL FAILED ===\n";
                std::cerr << "model=" << summary_model << " http_status=" << sr.http_status << "\n";
                if (!sr.error.empty()) std::cerr << sr.error << "\n";
              }
            }
          }

          // Apply the same compaction policy as the non-stream `agent_run_once` path, but then issue an explicit
          // OpenAI-compatible `stream: true` request so we can emit incremental stdout deltas.
          agent_compact_report_t compact{};
          const agent_status_t cst = agent_session_compact_char_budget(session, attempt_max_chars, keep, summary_or_null, &compact);
          if (cst != AGENT_OK) {
            ok = false;
            err = std::string("session compaction failed: ") + std::to_string((int)cst);
            break;
          }

          // Build OpenAI-compatible request JSON from the compacted session.
          std::string request_json;
          {
            Json::Value root(Json::objectValue);
            root["model"] = run_cfg.model;
            root["stream"] = true;
            Json::Value messages(Json::arrayValue);
            const size_t n = agent_session_message_count(session);
            for (size_t i = 0; i < n; i++) {
              agent_message_view_t v{};
              if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
              Json::Value m(Json::objectValue);
              m["role"] = agent_role_to_string(v.role);
              std::string content(v.content, v.content_len);
              if (have_mm && i + 1 == n && v.role == AGENT_ROLE_USER) {
                // Substitute multimodal-wrapped prompt for the provider call, while keeping
                // the persisted session prompt clean.
                content = prompt_for_llm;
              }
              std::string text = content;
              Json::Value mm(Json::nullValue);
              if (try_parse_multimodal_prefix(content, &text, &mm) && mm.isObject()) {
                m["content"] = multimodal_content_from_parts(text, mm);
              } else {
                m["content"] = content;
              }
              messages.append(m);
            }
            root["messages"] = messages;
            request_json = json_stringify(root);
          }

          struct StreamCtx {
            std::ostream* out = nullptr;
            OpenAIStreamCoreAdapter core;
          } sctx;
          sctx.out = &std::cout;

          OpenAIStreamCoreConfig scfg{};
          scfg.max_tool_calls_total = 0;
          scfg.max_tool_call_args_chars = 0;
          scfg.max_events_per_feed = 0;
          scfg.delta_flush_bytes = 64;
          scfg.step = 0;
          scfg.epoch = (uint64_t)attempt;

          auto on_delta = [](void* vctx, const char* delta, size_t delta_len, uint64_t step, uint64_t epoch) {
            auto* s = static_cast<StreamCtx*>(vctx);
            if (!s || !s->out || !delta || delta_len == 0) return;
            (void)step;
            (void)epoch;
            (*s->out) << std::string(delta, delta_len) << std::flush;
          };

          openai_stream_core_init(&sctx.core, &scfg, on_delta, &sctx);

          auto on_chunk = [](void* vctx, const char* chunk_json, size_t chunk_len) {
            auto* s = static_cast<StreamCtx*>(vctx);
            if (!s || !chunk_json || chunk_len == 0) return;
            (void)openai_stream_core_feed_chunk(&s->core, chunk_json, chunk_len);
          };

          OpenAIStreamResult sr = openai_chat_completions_raw_stream(run_cfg, request_json, on_chunk, &sctx);
          http_status = sr.http_status;
          http_body = sr.response_body;
          compact_final = compact;

          if (trace) {
            std::cerr << "=== REQUEST (stream=true attempt=" << attempt << ") ===\n";
            std::cerr << request_json << "\n";
            std::cerr << "=== RESPONSE (stream capture) ===\n";
            if (!sr.response_body.empty()) std::cerr << sr.response_body << "\n";
            std::cerr << "=== COMPACTION ===\n";
            std::cerr << "before_chars=" << compact.before_chars
                      << " after_chars=" << compact.after_chars
                      << " dropped=" << compact.dropped_messages
                      << " inserted_summary=" << (int)compact.inserted_summary
                      << "\n";
          }

          // Flush buffered stdout deltas.
          openai_stream_core_flush(&sctx.core);
          saw_stream_delta = saw_stream_delta || (sctx.core.saw_delta != 0);

          if (sr.http_status < 200 || sr.http_status >= 300) {
            // Retry on context-too-long rejections by compacting more aggressively (session rotation).
            if (attempt < 2 && openai_is_context_too_long_error(sr.http_status, sr.response_body)) {
              const size_t next = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
              if (trace) {
                std::cerr << "=== RETRY (context too long) ===\n";
                std::cerr << "attempt=" << attempt << " http_status=" << sr.http_status
                          << " max_chars_before=" << attempt_max_chars
                          << " max_chars_after=" << next << "\n";
              }
              attempt_max_chars = next;
              openai_stream_core_free(&sctx.core);
              continue;
            }
            ok = false;
            err = (!sr.error_message.empty() ? sr.error_message : openai_format_http_error(sr.http_status, sr.response_body));
            openai_stream_core_free(&sctx.core);
            break;
          }

          // Provider may have ignored streaming; fall back to extracting assistant content from a normal JSON completion.
          assistant_text = (sctx.core.assistant.data && sctx.core.assistant.len)
            ? std::string(sctx.core.assistant.data, sctx.core.assistant.len)
            : std::string();
          if (assistant_text.empty() && !sr.response_body.empty() && sr.response_body.size() < (4u * 1024u * 1024u) &&
              sr.response_body[0] == '{') {
            Json::CharReaderBuilder rb;
            std::string errs;
            std::istringstream iss(sr.response_body);
            Json::Value parsed;
            if (Json::parseFromStream(rb, iss, &parsed, &errs) && parsed.isObject()) {
              const auto& choices = parsed["choices"];
              if (choices.isArray() && !choices.empty()) {
                const auto& msg = choices[0]["message"];
                const auto& content = msg["content"];
                if (content.isString()) {
                  assistant_text = content.asString();
                } else {
                  const auto& text = choices[0]["text"];
                  if (text.isString()) assistant_text = text.asString();
                }
              }
            }
          }

          if (assistant_text.empty()) {
            ok = false;
            err = "streamed completion returned no assistant content";
            openai_stream_core_free(&sctx.core);
            break;
          }

          agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
          openai_stream_core_free(&sctx.core);
          ok = true;
          break;
        }

        // Append a per-run audit record for tools=none runs (host-only; session messages remain clean).
        if (!no_session && !session_id.empty()) {
          Json::Value record(Json::objectValue);
          record["ts_unix_ms"] = (Json::Int64)now_unix_ms();
          record["prompt"] = user_prompt;
          record["ok"] = ok;
          record["tools"] = "none";
          record["model"] = model;
          record["base_url"] = run_cfg.base_url;
          record["stream_assistant"] = true;
          record["attempt"] = final_attempt;
          record["http_status"] = (Json::Int64)http_status;
          if (!ok) record["error"] = err;
          if (ok) record["assistant_text"] = assistant_text;
          {
            Json::Value c(Json::objectValue);
            c["before_chars"] = (Json::UInt64)compact_final.before_chars;
            c["after_chars"] = (Json::UInt64)compact_final.after_chars;
            c["dropped_messages"] = (Json::UInt64)compact_final.dropped_messages;
            c["inserted_summary"] = (bool)compact_final.inserted_summary;
            record["compaction"] = c;
          }
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          (void)session_store_append_audit_jsonl(store_cfg, session_id, Json::writeString(wb, record));
        }

        if (!ok) {
          if (!err.empty()) std::cerr << err << "\n";
          if (!http_body.empty()) std::cerr << http_body << "\n";
          return 1;
        }

        if (saw_stream_delta) {
          std::cout << "\n";
        } else {
          std::cout << assistant_text << "\n";
        }
        return 0;
#endif
      }

#if defined(AGENT_HAVE_JSONCPP)
      // Non-stream tools=none + multimodal: bypass agent_run_once so we can override the request body
      // without persisting the multimodal wrapper into the session transcript.
      if (have_mm) {
        agent_session_add_message(session, AGENT_ROLE_USER, user_prompt.c_str());

        const OpenAIClientConfig run_cfg = pctx.cfg;
        const size_t keep = (keep_last == 0 ? 16 : keep_last);

        size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
        int final_attempt = -1;
        bool ok = false;
        long http_status = 0;
        std::string http_body;
        std::string err;
        std::string assistant_text;
        agent_compact_report_t compact_final{};

        for (int attempt = 0; attempt < 3; attempt++) {
          final_attempt = attempt;

          const char* summary_or_null = nullptr;
          std::string summary_buf;
          if (attempt == 0 && !summary_model.empty() && agent_session_estimated_chars(session) > attempt_max_chars) {
            SummaryCompactionInput input = build_summary_compaction_input(session, keep);
            if (input.dropped_messages > 0 && !input.excerpt.empty()) {
              const size_t max_out = summary_max_chars == 0 ? 1200 : summary_max_chars;
              CompactionSummaryResult sr = generate_compaction_summary_via_llm(run_cfg, summary_model, input, max_out);
              if (sr.ok && !sr.summary_text.empty()) {
                summary_buf = std::string(AGENT_SESSION_SUMMARY_PREFIX) + "\n" + sr.summary_text;
                summary_or_null = summary_buf.c_str();
              }
            }
          }

          agent_compact_report_t compact{};
          const agent_status_t cst = agent_session_compact_char_budget(session, attempt_max_chars, keep, summary_or_null, &compact);
          if (cst != AGENT_OK) {
            ok = false;
            err = std::string("session compaction failed: ") + std::to_string((int)cst);
            break;
          }

          std::string request_json;
          {
            Json::Value root(Json::objectValue);
            root["model"] = run_cfg.model;
            root["stream"] = false;
            Json::Value messages(Json::arrayValue);
            const size_t n = agent_session_message_count(session);
            for (size_t i = 0; i < n; i++) {
              agent_message_view_t v{};
              if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
              Json::Value m(Json::objectValue);
              m["role"] = agent_role_to_string(v.role);
              std::string content(v.content, v.content_len);
              if (i + 1 == n && v.role == AGENT_ROLE_USER) {
                content = prompt_for_llm;
              }
              std::string text = content;
              Json::Value mm(Json::nullValue);
              if (try_parse_multimodal_prefix(content, &text, &mm) && mm.isObject()) {
                m["content"] = multimodal_content_from_parts(text, mm);
              } else {
                m["content"] = content;
              }
              messages.append(m);
            }
            root["messages"] = messages;
            request_json = json_stringify(root);
          }

          OpenAIRawResult raw = openai_chat_completions_raw(run_cfg, request_json);
          http_status = raw.http_status;
          http_body = raw.response_body;
          compact_final = compact;

          if (trace) {
            std::cerr << "=== REQUEST (stream=false attempt=" << attempt << ") ===\n";
            std::cerr << request_json << "\n";
            std::cerr << "=== RESPONSE ===\n";
            if (!raw.response_body.empty()) std::cerr << raw.response_body << "\n";
          }

          if (raw.http_status < 200 || raw.http_status >= 300) {
            if (attempt < 2 && openai_is_context_too_long_error(raw.http_status, raw.response_body)) {
              attempt_max_chars = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
              continue;
            }
            ok = false;
            err = openai_format_http_error(raw.http_status, raw.response_body);
            break;
          }

          // Extract assistant content.
          Json::CharReaderBuilder rb;
          std::string errs;
          std::istringstream iss(raw.response_body);
          Json::Value parsed;
          if (Json::parseFromStream(rb, iss, &parsed, &errs) && parsed.isObject()) {
            const auto& choices = parsed["choices"];
            if (choices.isArray() && !choices.empty()) {
              const auto& msg = choices[0]["message"];
              const auto& content = msg["content"];
              if (content.isString()) assistant_text = content.asString();
              else {
                const auto& text = choices[0]["text"];
                if (text.isString()) assistant_text = text.asString();
              }
            }
          }
          if (assistant_text.empty()) {
            ok = false;
            err = "completion returned no assistant content";
            break;
          }
          agent_session_add_message(session, AGENT_ROLE_ASSISTANT, assistant_text.c_str());
          ok = true;
          break;
        }

        if (!no_session && !session_id.empty()) {
          Json::Value record(Json::objectValue);
          record["ts_unix_ms"] = (Json::Int64)now_unix_ms();
          record["prompt"] = user_prompt;
          record["ok"] = ok;
          record["tools"] = "none";
          record["model"] = model;
          record["base_url"] = run_cfg.base_url;
          record["stream_assistant"] = false;
          record["attempt"] = final_attempt;
          record["http_status"] = (Json::Int64)http_status;
          if (!ok) record["error"] = err;
          if (ok) record["assistant_text"] = assistant_text;
          {
            Json::Value c(Json::objectValue);
            c["before_chars"] = (Json::UInt64)compact_final.before_chars;
            c["after_chars"] = (Json::UInt64)compact_final.after_chars;
            c["dropped_messages"] = (Json::UInt64)compact_final.dropped_messages;
            c["inserted_summary"] = (bool)compact_final.inserted_summary;
            record["compaction"] = c;
          }
          Json::StreamWriterBuilder wb;
          wb["indentation"] = "";
          (void)session_store_append_audit_jsonl(store_cfg, session_id, Json::writeString(wb, record));
        }

        if (!ok) {
          if (!err.empty()) std::cerr << err << "\n";
          if (!http_body.empty()) std::cerr << http_body << "\n";
          return 1;
        }
        std::cout << assistant_text << "\n";
        return 0;
      }
#endif

      agent_session_add_message(session, AGENT_ROLE_USER, user_prompt.c_str());

      const agent_provider_t provider = openai_make_provider(&pctx);

      agent_run_options_t run_opt;
      run_opt.model = model.c_str();
      run_opt.keep_last_messages = keep_last;
      run_opt.summary_or_null = nullptr;

      size_t attempt_max_chars = (max_chars == 0 ? 20000 : max_chars);
      int final_attempt = -1;
      bool ok = false;
      long http_status = 0;
      std::string err;
      std::string assistant_text;
      agent_run_report_t rep_final{};
      agent_status_t last_st = AGENT_ERR_INTERNAL;

      for (int attempt = 0; attempt < 3; attempt++) {
        final_attempt = attempt;
        run_opt.max_chars = attempt_max_chars;
        run_opt.summary_or_null = nullptr;

        std::string summary_buf;
        if (attempt == 0 && !summary_model.empty() && agent_session_estimated_chars(session) > attempt_max_chars) {
          SummaryCompactionInput input = build_summary_compaction_input(session, keep_last);
          if (input.dropped_messages > 0 && !input.excerpt.empty()) {
            const size_t max_out = summary_max_chars == 0 ? 1200 : summary_max_chars;
            CompactionSummaryResult sr = generate_compaction_summary_via_llm(pctx.cfg, summary_model, input, max_out);
            if (sr.ok && !sr.summary_text.empty()) {
              summary_buf = std::string(AGENT_SESSION_SUMMARY_PREFIX) + "\n" + sr.summary_text;
              run_opt.summary_or_null = summary_buf.c_str();
              if (trace) {
                std::cerr << "=== SUMMARY MODEL ===\n";
                std::cerr << "model=" << summary_model << " dropped_messages=" << input.dropped_messages
                          << " excerpt_truncated=" << (input.truncated ? "true" : "false") << "\n";
              }
            } else if (trace) {
              std::cerr << "=== SUMMARY MODEL FAILED ===\n";
              std::cerr << "model=" << summary_model << " http_status=" << sr.http_status << "\n";
              if (!sr.error.empty()) std::cerr << sr.error << "\n";
            }
          }
        }

        agent_run_report_t rep{};
        const agent_status_t st = agent_run_once(session, &provider, &run_opt, &rep);
        last_st = st;
        if (st == AGENT_OK) {
          ok = true;
          rep_final = rep;
          assistant_text = std::string(rep.assistant_view.content, rep.assistant_view.content_len);
          break;
        }

        http_status = pctx.last_http_status;
        if (!pctx.last_error.empty()) {
          err = pctx.last_error;
        } else {
          err = std::string("agent_run_once failed: ") + std::to_string((int)st);
        }

        // Retry on context-too-long rejections by compacting more aggressively (session rotation).
        if (attempt < 2 && st == AGENT_ERR_CONTEXT_TOO_LONG) {
          const size_t next = std::max<size_t>(2000, (attempt_max_chars * 3) / 4);
          if (trace) {
            std::cerr << "=== RETRY (context too long) ===\n";
            std::cerr << "attempt=" << attempt << " http_status=" << pctx.last_http_status
                      << " max_chars_before=" << attempt_max_chars
                      << " max_chars_after=" << next << "\n";
          }
          attempt_max_chars = next;
          continue;
        }
        break;
      }

      // Append a per-run audit record for tools=none runs (host-only; session messages remain clean).
      if (!no_session && !session_id.empty()) {
#if defined(AGENT_HAVE_JSONCPP)
        Json::Value record(Json::objectValue);
        record["ts_unix_ms"] = (Json::Int64)now_unix_ms();
        record["prompt"] = user_prompt;
        record["ok"] = ok;
        record["tools"] = "none";
        record["model"] = model;
        record["base_url"] = pctx.cfg.base_url;
        record["attempt"] = final_attempt;
        record["http_status"] = (Json::Int64)(ok ? pctx.last_http_status : http_status);
        if (!ok) record["error"] = err;
        if (ok) record["assistant_text"] = assistant_text;
        {
          Json::Value c(Json::objectValue);
          c["before_chars"] = (Json::UInt64)rep_final.compact.before_chars;
          c["after_chars"] = (Json::UInt64)rep_final.compact.after_chars;
          c["dropped_messages"] = (Json::UInt64)rep_final.compact.dropped_messages;
          c["inserted_summary"] = (bool)rep_final.compact.inserted_summary;
          record["compaction"] = c;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        (void)session_store_append_audit_jsonl(store_cfg, session_id, Json::writeString(wb, record));
#endif
      }

      if (ok) {
        if (trace) {
          std::cerr << "=== REQUEST (attempt=" << final_attempt << ") ===\n";
          if (!pctx.last_request_body.empty()) {
            std::cerr << pctx.last_request_body << "\n";
          } else {
            std::cerr << "(request body unavailable)\n";
          }
          std::cerr << "=== RESPONSE ===\n";
          if (!pctx.last_body.empty()) {
            std::cerr << pctx.last_body << "\n";
          }
          std::cerr << "=== COMPACTION ===\n";
          std::cerr << "before_chars=" << rep_final.compact.before_chars
                    << " after_chars=" << rep_final.compact.after_chars
                    << " dropped=" << rep_final.compact.dropped_messages
                    << " inserted_summary=" << (int)rep_final.compact.inserted_summary
                    << "\n";
        }
        std::cout << assistant_text << "\n";
        return 0;
      }

      if (!err.empty()) {
        std::cerr << err << "\n";
      }
      if (!pctx.last_body.empty()) {
        std::cerr << pctx.last_body << "\n";
      } else if (final_attempt >= 2 && last_st == AGENT_ERR_CONTEXT_TOO_LONG) {
        std::cerr << "agent_run_once failed after retries\n";
      }
      return 1;
    };

    int rc = 0;
    if (cmd == "run") {
      rc = run_one(prompt);
    } else {
      std::string line;
      while (true) {
        std::cerr << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
          break;
        }
        if (line == "/exit" || line == "/quit") {
          break;
        }
        if (line.empty()) {
          continue;
        }
        rc = run_one(line);
        if (rc != 0) {
          break;
        }
      }
    }

    if (!no_session && !session_id.empty()) {
      const agent_status_t st = persistor.save(persistor.ctx, session_id.c_str(), session);
      if (st != AGENT_OK) {
        std::cerr << "Failed to save session: " << (int)st << "\n";
        rc = 1;
      }
    }

    agent_session_destroy(session);
    agent_persistor_destroy(&persistor);
    return rc;
  }

  // Unreachable.
}
