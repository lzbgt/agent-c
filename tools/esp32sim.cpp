#include "agent/agent.h"
#include "agent/session_codec.h"
#include "agent/tool_loop.h"
#include "agent/tool_provider.h"
#include "agent/tools.h"

#include "openai_tool_provider.h"
#include "base64.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(AGENT_HAVE_JSONCPP)
#include <json/json.h>
#endif

static int64_t unix_ms_now() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string getenv_str(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? std::string(v) : std::string();
}

static bool read_file_all(const std::string& path, std::string* out) {
  if (!out) return false;
  out->clear();
  std::ifstream f(path, std::ios::in | std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static bool write_file_all(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), (std::streamsize)data.size());
  return (bool)f;
}

static std::string json_escape_string(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char tmp[8];
          std::snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
          out += tmp;
        } else {
          out.push_back((char)c);
        }
    }
  }
  return out;
}

struct LogSink {
  std::ofstream f;
  bool enabled = false;

  void open(const std::string& path, bool append) {
    if (path.empty()) return;
    f.open(path, std::ios::out | std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    enabled = (bool)f;
  }

  void write_event(const char* type, const char* data_json_or_null) {
    if (!enabled) return;
    const int64_t ts = unix_ms_now();
    const std::string t = type ? type : "";
    const std::string d = data_json_or_null ? data_json_or_null : "{}";
    f << "{\"ts_unix_ms\":" << ts << ",\"type\":\"" << json_escape_string(t) << "\",";
    // Most events provide an object-shaped JSON payload. Store it as JSON (not a quoted string) so `jq` works.
    if (!d.empty() && (d[0] == '{' || d[0] == '[')) {
      f << "\"data\":" << d;
    } else {
      f << "\"data_str\":\"" << json_escape_string(d) << "\"";
    }
    f << "}\n";
    f.flush();
  }
};

static bool json_extract_string_field(const char* json, const char* key, std::string* out) {
  if (!json || !key || !out) return false;
  out->clear();
  const std::string needle = std::string("\"") + key + "\"";
  const char* p = std::strstr(json, needle.c_str());
  if (!p) return false;
  p += needle.size();
  while (*p && *p != ':') p++;
  if (*p != ':') return false;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != '"') return false;
  p++;
  std::string s;
  for (; *p; p++) {
    const unsigned char c = (unsigned char)*p;
    if (c == '"') {
      *out = s;
      return true;
    }
    if (c == '\\') {
      p++;
      if (!*p) break;
      const unsigned char e = (unsigned char)*p;
      switch (e) {
        case '\\': s.push_back('\\'); break;
        case '"': s.push_back('"'); break;
        case 'n': s.push_back('\n'); break;
        case 'r': s.push_back('\r'); break;
        case 't': s.push_back('\t'); break;
        case 'u': {
          // Best-effort: decode \u00XX to a single byte if ASCII; otherwise emit '?'.
          unsigned v = 0;
          for (int i = 0; i < 4; i++) {
            p++;
            if (!*p) { v = 0; break; }
            const unsigned char h = (unsigned char)*p;
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (h - '0');
            else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
            else { v = 0; }
          }
          if (v <= 0x7Fu) s.push_back((char)v);
          else s.push_back('?');
          break;
        }
        default:
          s.push_back((char)e);
      }
      continue;
    }
    s.push_back((char)c);
  }
  return false;
}

static std::string truncate_preview(const std::string& s, size_t max_chars) {
  if (s.size() <= max_chars) return s;
  return s.substr(0, max_chars) + "...(truncated)";
}

#if defined(AGENT_HAVE_JSONCPP)
static const char* kMultimodalPrefix = "__AGENT_MM_V1__";

static std::string lower_copy(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string content_type_from_path(const std::filesystem::path& p) {
  const std::string ext = lower_copy(p.extension().string());
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".txt" || ext == ".md") return "text/plain";
  if (ext == ".json") return "application/json";
  if (ext == ".yaml" || ext == ".yml") return "text/yaml";
  return "application/octet-stream";
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
  return bad * 20 < bytes.size(); // <5% suspicious bytes
}

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

static std::string wrap_prompt_with_attachments_v1(
  const std::string& prompt,
  const std::vector<std::string>& attach_paths,
  size_t max_file_bytes,
  size_t max_image_bytes
) {
  if (attach_paths.empty()) return prompt;

  Json::Value mm(Json::objectValue);
  Json::Value images(Json::arrayValue);
  Json::Value files(Json::arrayValue);

  for (const auto& raw : attach_paths) {
    if (raw.empty()) continue;
    const std::filesystem::path p = std::filesystem::path(raw);
    const std::string name = p.filename().string();
    const std::string mime = content_type_from_path(p);
    const bool is_image = (mime.rfind("image/", 0) == 0);

    bool trunc = false;
    std::string bytes;
    const size_t cap = is_image ? max_image_bytes : max_file_bytes;
    if (!read_file_bytes_capped(p, cap, &bytes, &trunc)) {
      Json::Value f(Json::objectValue);
      f["name"] = name.empty() ? raw : name;
      f["mime"] = mime;
      f["text"] = std::string("[Attachment read failed] ") + raw;
      files.append(f);
      continue;
    }

    if (is_image) {
      Json::Value im(Json::objectValue);
      im["mime"] = mime;
      im["b64"] = base64_encode(bytes.data(), bytes.size());
      images.append(im);
      continue;
    }

    // Non-image attachments are sent as text blocks (best-effort).
    Json::Value f(Json::objectValue);
    f["name"] = name.empty() ? raw : name;
    f["mime"] = mime;
    f["truncated"] = trunc;
    if (looks_texty(bytes)) {
      f["text"] = bytes;
    } else {
      // Keep embedded-friendly behavior: represent binary as a short base64 hint.
      const std::string b64 = base64_encode(bytes.data(), bytes.size());
      std::string note = "[Binary attachment; base64 preview]\n";
      note += truncate_preview(b64, 4000);
      if (trunc) note += "\n...(truncated)";
      f["text"] = note;
    }
    files.append(f);
  }

  if (!images.empty()) mm["images"] = images;
  if (!files.empty()) mm["files"] = files;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string json_line = Json::writeString(wb, mm);
  return std::string(kMultimodalPrefix) + json_line + "\n" + prompt;
}
#endif

struct SimEventCtx {
  LogSink log;
  bool print_cli = true;
  bool print_llm = true;
  bool stream_assistant = false;
  bool saw_any_cli_output = false;
};

static void on_any_event(void* vctx, const char* type, const char* data_json) {
  auto* ctx = static_cast<SimEventCtx*>(vctx);
  if (!ctx) return;
  ctx->log.write_event(type, data_json);

  if (!ctx->print_cli) return;
  const std::string t = type ? type : "";
  const char* d = data_json ? data_json : "{}";

  if (t == "assistant_delta") {
    std::string delta;
    if (json_extract_string_field(d, "delta", &delta) && !delta.empty()) {
      std::cout << delta;
      std::cout.flush();
      ctx->saw_any_cli_output = true;
      return;
    }
  }

  if (t == "assistant_message") {
    // If the provider is already streaming deltas, avoid duplicating the whole message.
    if (ctx->stream_assistant) return;
    std::string content;
    if (json_extract_string_field(d, "assistant_content", &content) && !content.empty()) {
      std::cout << content << "\n";
      std::cout.flush();
      ctx->saw_any_cli_output = true;
      return;
    }
  }

  if (t == "tool_call") {
    std::string tool;
    std::string args;
    (void)json_extract_string_field(d, "tool_name", &tool);
    (void)json_extract_string_field(d, "arguments_json", &args);
    std::cerr << "tool_call: " << (tool.empty() ? "(unknown)" : tool);
    if (!args.empty()) std::cerr << " args=" << truncate_preview(args, 400);
    std::cerr << "\n";
    ctx->saw_any_cli_output = true;
    return;
  }

  if (t == "tool_result") {
    std::string tool;
    std::string content;
    (void)json_extract_string_field(d, "tool_name", &tool);
    (void)json_extract_string_field(d, "content", &content);
    std::cerr << "tool_result: " << (tool.empty() ? "(unknown)" : tool);
    if (!content.empty()) std::cerr << " -> " << truncate_preview(content, 400);
    std::cerr << "\n";
    ctx->saw_any_cli_output = true;
    return;
  }

  if (t == "llm_request") {
    if (!ctx->print_llm) return;
    std::cerr << "llm_request...\n";
    ctx->saw_any_cli_output = true;
    return;
  }

  if (t == "llm_response") {
    if (!ctx->print_llm) return;
    std::cerr << "llm_response\n";
    ctx->saw_any_cli_output = true;
    return;
  }

  if (t == "error" || t == "cancelled") {
    std::cerr << t << ": " << truncate_preview(std::string(d), 600) << "\n";
    ctx->saw_any_cli_output = true;
    return;
  }
}

struct BudgetAlloc {
  size_t limit_bytes = 0;
  size_t used_bytes = 0;
};

static BudgetAlloc* g_budget_alloc = nullptr;

static void* budget_malloc(size_t n) {
  // This allocator is installed globally via agent_set_allocator.
  // The simulator is intentionally single-threaded; this is sufficient.
  BudgetAlloc* g = g_budget_alloc;
  if (!g) {
    // If not initialized, fall back to libc.
    return std::malloc(n);
  }
  if (n == 0) n = 1;
  // Crude overhead accounting: store the allocation size just before the returned pointer.
  const size_t overhead = sizeof(size_t);
  const size_t want = n + overhead;
  if (g->limit_bytes > 0 && g->used_bytes + want > g->limit_bytes) {
    return nullptr;
  }
  void* raw = std::malloc(want);
  if (!raw) return nullptr;
  *(size_t*)raw = want;
  g->used_bytes += want;
  return (void*)((unsigned char*)raw + overhead);
}

static void budget_free(void* p) {
  BudgetAlloc* g = g_budget_alloc;
  if (!p) return;
  if (!g) {
    std::free(p);
    return;
  }
  const size_t overhead = sizeof(size_t);
  void* raw = (void*)((unsigned char*)p - overhead);
  const size_t want = *(size_t*)raw;
  if (g->used_bytes >= want) g->used_bytes -= want;
  std::free(raw);
}

static void budget_alloc_init(BudgetAlloc* a, size_t limit_bytes) {
  if (!a) return;
  a->limit_bytes = limit_bytes;
  a->used_bytes = 0;
  // Install allocator.
  // NOTE: the core does not expose a way to pass allocator context, so we use a static pointer.
  // Keep this simulator single-threaded to avoid surprises.
  g_budget_alloc = a;
  agent_allocator_t al{};
  al.malloc_fn = budget_malloc;
  al.free_fn = budget_free;
  (void)agent_set_allocator(&al);
}

struct SimDevice {
  int gpio[64]{};
};

static bool json_get_int_field(const char* json, const char* key, int* out) {
  if (!json || !key || !out) return false;
  const std::string needle = std::string("\"") + key + "\"";
  const char* p = std::strstr(json, needle.c_str());
  if (!p) return false;
  p += needle.size();
  while (*p && *p != ':') p++;
  if (*p != ':') return false;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  char* end = nullptr;
  const long v = std::strtol(p, &end, 10);
  if (end == p) return false;
  *out = (int)v;
  return true;
}

static agent_status_t sim_exec_tool(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  if (!vctx || !tool_name || !arguments_json || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  auto* dev = static_cast<SimDevice*>(vctx);

  const std::string name(tool_name);
  if (name == "gpio_write") {
    int pin = -1;
    int value = -1;
    if (!json_get_int_field(arguments_json, "pin", &pin) || !json_get_int_field(arguments_json, "value", &value)) {
      const char* err = "{\"ok\":false,\"error\":\"missing pin/value\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    if (pin < 0 || pin >= 64 || (value != 0 && value != 1)) {
      const char* err = "{\"ok\":false,\"error\":\"invalid pin/value\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    dev->gpio[pin] = value;
    const char* ok = "{\"ok\":true}";
    return agent_string_set_copy(out_result, ok, std::strlen(ok));
  }

  if (name == "gpio_read") {
    int pin = -1;
    if (!json_get_int_field(arguments_json, "pin", &pin)) {
      const char* err = "{\"ok\":false,\"error\":\"missing pin\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    if (pin < 0 || pin >= 64) {
      const char* err = "{\"ok\":false,\"error\":\"invalid pin\"}";
      return agent_string_set_copy(out_result, err, std::strlen(err));
    }
    const int v = dev->gpio[pin];
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"pin\":%d,\"value\":%d}", pin, v);
    return (n > 0) ? agent_string_set_copy(out_result, buf, (size_t)n) : AGENT_ERR_INTERNAL;
  }

  const char* err = "{\"ok\":false,\"error\":\"unknown tool\"}";
  return agent_string_set_copy(out_result, err, std::strlen(err));
}

static void usage() {
  std::cerr
    << "esp32sim (host harness for embedded agent_core)\n\n"
    << "Required:\n"
    << "  --model <id>\n"
    << "  --api-key <key>        (or set OPENAI_API_KEY)\n\n"
    << "Optional:\n"
    << "  --base-url <url>       default: https://api.openai.com/v1 (or set OPENAI_API_BASE)\n"
    << "  --proxy-url <url>      overrides HTTPS_PROXY env (or set OPENAI_PROXY_URL)\n"
    << "  --timeout-ms <n>       default: 60000\n"
    << "  --openrouter-referer <s> (or set OPENROUTER_HTTP_REFERER)\n"
    << "  --openrouter-title <s>   (or set OPENROUTER_X_TITLE)\n"
    << "  --prompt <text>        if omitted, reads stdin\n"
    << "  --session <path>       load/save session codec v1\n"
    << "  --log <path>           JSONL event log (default: out/esp32sim.jsonl)\n"
    << "  --log-append          append logs instead of truncating\n"
    << "  --quiet               suppress CLI prints (still logs)\n"
    << "  --print-llm 0|1       print llm_request/llm_response markers (default: 1)\n"
    << "  --max-steps <n>        default: 6\n"
    << "  --max-chars <n>        default: 8000\n"
    << "  --keep-last <n>        default: 12\n"
    << "  --max-tool-result <n>  default: 1200\n"
    << "  --heap-limit <bytes>   caps agent_malloc/agent_free allocations (default: 0=unlimited)\n"
    << "  --stream-assistant 0|1 default: 0\n"
    << "  --attach <path>        Attach a local file (repeatable; images are sent as base64)\n"
    << "  --max-attach-bytes <n> default: 65536 (non-image)\n"
    << "  --max-image-bytes <n>  default: 262144 (image)\n";
}

static bool parse_u64(const std::string& s, uint64_t* out) {
  if (!out) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (!end || end == s.c_str()) return false;
  *out = (uint64_t)v;
  return true;
}

int main(int argc, char** argv) {
  std::string base_url;
  std::string api_key;
  std::string model;
  std::string proxy_url;
  std::string openrouter_referer;
  std::string openrouter_title;
  std::string prompt;
  std::string session_path;
  std::string log_path = "out/esp32sim.jsonl";
  uint64_t max_steps = 6;
  uint64_t max_chars = 8000;
  uint64_t keep_last = 12;
  uint64_t max_tool_result = 1200;
  uint64_t heap_limit = 0;
  uint64_t timeout_ms = 60'000;
  uint64_t max_attach_bytes = 65'536;
  uint64_t max_image_bytes = 262'144;
  bool stream_assistant = false;
  bool quiet = false;
  bool print_llm = true;
  bool log_append = false;
  std::vector<std::string> attach_paths;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    auto need = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << flag << "\n";
        std::exit(2);
      }
      return std::string(argv[++i] ? argv[i] : "");
    };

    if (a == "--base-url") base_url = need("--base-url");
    else if (a == "--api-key") api_key = need("--api-key");
    else if (a == "--model") model = need("--model");
    else if (a == "--proxy-url") proxy_url = need("--proxy-url");
    else if (a == "--openrouter-referer") openrouter_referer = need("--openrouter-referer");
    else if (a == "--openrouter-title") openrouter_title = need("--openrouter-title");
    else if (a == "--prompt") prompt = need("--prompt");
    else if (a == "--session") session_path = need("--session");
    else if (a == "--log") log_path = need("--log");
    else if (a == "--log-append") log_append = true;
    else if (a == "--max-steps") { (void)parse_u64(need("--max-steps"), &max_steps); }
    else if (a == "--max-chars") { (void)parse_u64(need("--max-chars"), &max_chars); }
    else if (a == "--keep-last") { (void)parse_u64(need("--keep-last"), &keep_last); }
    else if (a == "--max-tool-result") { (void)parse_u64(need("--max-tool-result"), &max_tool_result); }
    else if (a == "--heap-limit") { (void)parse_u64(need("--heap-limit"), &heap_limit); }
    else if (a == "--timeout-ms") { (void)parse_u64(need("--timeout-ms"), &timeout_ms); }
    else if (a == "--attach") attach_paths.push_back(need("--attach"));
    else if (a == "--max-attach-bytes") { (void)parse_u64(need("--max-attach-bytes"), &max_attach_bytes); }
    else if (a == "--max-image-bytes") { (void)parse_u64(need("--max-image-bytes"), &max_image_bytes); }
    else if (a == "--stream-assistant") {
      const std::string v = need("--stream-assistant");
      stream_assistant = (v == "1" || v == "true" || v == "yes");
    } else if (a == "--quiet") {
      quiet = true;
    } else if (a == "--print-llm") {
      const std::string v = need("--print-llm");
      print_llm = (v == "1" || v == "true" || v == "yes");
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      usage();
      return 2;
    }
  }

  if (base_url.empty()) base_url = getenv_str("OPENAI_API_BASE");
  if (base_url.empty()) base_url = "https://api.openai.com/v1";
  if (api_key.empty()) api_key = getenv_str("OPENAI_API_KEY");
  if (model.empty()) model = getenv_str("OPENAI_MODEL");
  if (proxy_url.empty()) proxy_url = getenv_str("OPENAI_PROXY_URL");
  if (openrouter_referer.empty()) openrouter_referer = getenv_str("OPENROUTER_HTTP_REFERER");
  if (openrouter_title.empty()) openrouter_title = getenv_str("OPENROUTER_X_TITLE");

  if (prompt.empty()) {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    prompt = ss.str();
    // Trim trailing whitespace for convenience.
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r' || prompt.back() == ' ' || prompt.back() == '\t')) {
      prompt.pop_back();
    }
  }

  if (model.empty() || api_key.empty()) {
    std::cerr << "missing required --model/--api-key (or OPENAI_MODEL/OPENAI_API_KEY)\n";
    usage();
    return 2;
  }
  if (prompt.empty()) {
    std::cerr << "missing prompt (use --prompt or provide stdin)\n";
    return 2;
  }

  BudgetAlloc alloc{};
  budget_alloc_init(&alloc, (size_t)heap_limit);

  SimEventCtx ev{};
  ev.print_cli = !quiet;
  ev.print_llm = print_llm;
  ev.stream_assistant = stream_assistant;
  ev.log.open(log_path, log_append);
  if (!ev.log.enabled && !quiet) {
    std::cerr << "warning: failed to open log file: " << log_path << "\n";
  }
  ev.log.write_event("sim_start", "{\"ok\":true}");

  if (!quiet) {
    std::cerr << "esp32sim: model=" << model << " base_url=" << base_url << " log=" << log_path << "\n";
    std::cerr.flush();
  }

  // Tool registry: keep it minimal and strict (embedded-friendly).
  agent_tool_registry_t* reg = nullptr;
  if (agent_tool_registry_create(&reg) != AGENT_OK || !reg) {
    std::cerr << "failed to create tool registry\n";
    return 1;
  }
  (void)agent_tool_registry_add(
    reg,
    "gpio_write",
    "Set a GPIO pin high/low",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":63},\"value\":{\"type\":\"integer\",\"enum\":[0,1]}},\"required\":[\"pin\",\"value\"]}"
  );
  (void)agent_tool_registry_add(
    reg,
    "gpio_read",
    "Read a GPIO pin digital value (simulated)",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":63}},\"required\":[\"pin\"]}"
  );

  // Session: load optional prior transcript (codec v1).
  agent_session_t* session = nullptr;
  if (!session_path.empty()) {
    std::string raw;
    if (read_file_all(session_path, &raw) && !raw.empty()) {
      agent_session_t* loaded = nullptr;
      const agent_status_t st = agent_session_codec_decode_v1(raw.data(), raw.size(), &loaded);
      if (st == AGENT_OK && loaded) {
        session = loaded;
      } else {
        ev.log.write_event("session_load_failed", "{\"ok\":false}");
      }
    }
  }
  if (!session) {
    (void)agent_session_create(&session);
    (void)agent_session_add_message(session, AGENT_ROLE_SYSTEM,
                                   "You are an embedded agent controlling peripherals via tools. "
                                   "Be concise and safe. Use tools when needed.");
  }

  // Provider: OpenAI-compatible tool calling adapter (host-side).
  OpenAIToolProviderCtx pctx{};
  pctx.cfg.base_url = base_url;
  pctx.cfg.api_key = api_key;
  pctx.cfg.model = model;
  if (!proxy_url.empty()) pctx.cfg.proxy_url = proxy_url;
  if (!openrouter_referer.empty()) pctx.cfg.openrouter_http_referer = openrouter_referer;
  if (!openrouter_title.empty()) pctx.cfg.openrouter_x_title = openrouter_title;
  pctx.cfg.timeout_ms = (long)timeout_ms;
  pctx.on_event = on_any_event;
  pctx.on_event_ctx = &ev;
  pctx.stream_assistant = stream_assistant;
  pctx.verbose_events = true;
  pctx.max_event_chars = 64 * 1024;
  pctx.max_capture_bytes = 128 * 1024;
  agent_tool_provider_t provider = openai_make_tool_provider(&pctx);

  // Simulated device.
  SimDevice dev{};
  agent_tool_executor_t exec{};
  exec.ctx = &dev;
  exec.execute = sim_exec_tool;

  agent_tool_loop_hooks_t hooks{};
  hooks.on_event = on_any_event;
  hooks.on_event_ctx = &ev;

  agent_tool_loop_options_t opt{};
  opt.model = model.c_str();
  opt.max_steps = (size_t)max_steps;
  opt.max_chars = (size_t)max_chars;
  opt.keep_last_messages = (size_t)keep_last;
  opt.max_tool_result_chars = (size_t)max_tool_result;
  opt.max_tool_calls_total = 24;
  opt.max_repeated_tool_calls = 6;
  opt.disable_tool_records = 1;
  opt.verbose_events = 1;
  opt.max_capture_chars = 256 * 1024;
  opt.max_context_too_long_retries = 2;

  std::string prompt_for_llm = prompt;
#if defined(AGENT_HAVE_JSONCPP)
  if (!attach_paths.empty()) {
    prompt_for_llm = wrap_prompt_with_attachments_v1(
      prompt,
      attach_paths,
      (size_t)max_attach_bytes,
      (size_t)max_image_bytes
    );
  }
#endif

  agent_tool_loop_result_t r{};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, session, prompt_for_llm.c_str(), &opt, &hooks, &r);

  {
    std::ostringstream ss;
    ss << "{\"ok\":" << ((st == AGENT_OK) ? "true" : "false");
    ss << ",\"status\":" << (int)st;
    ss << ",\"heap_used_bytes\":" << (unsigned long long)alloc.used_bytes;
    ss << ",\"heap_limit_bytes\":" << (unsigned long long)alloc.limit_bytes;
    ss << "}";
    const std::string payload = ss.str();
    ev.log.write_event("sim_done", payload.c_str());
  }

  if (st != AGENT_OK) {
    const std::string err = (r.error_message.data && r.error_message.len) ? std::string(r.error_message.data, r.error_message.len) : "unknown error";
    std::cerr << "run failed status=" << (int)st << " err=" << err << "\n";
  }

  // Print assistant text (for human verification).
  if (!quiet) {
    if (r.final_assistant_text.data && r.final_assistant_text.len) {
      // If we streamed deltas, the user already saw it; still end with a newline for a clean prompt.
      if (stream_assistant) std::cout << "\n";
      std::cout << std::string(r.final_assistant_text.data, r.final_assistant_text.len) << "\n";
      std::cout.flush();
    } else if (!ev.saw_any_cli_output) {
      std::cerr << "(no CLI output emitted; see log: " << log_path << ")\n";
    }
  }

  // Persist session if requested.
  if (!session_path.empty()) {
    agent_string_t encoded{};
    if (agent_session_codec_encode_v1(session, &encoded) == AGENT_OK) {
      (void)write_file_all(session_path, std::string(encoded.data ? encoded.data : "", encoded.len));
    }
    agent_string_free(&encoded);
  }

  agent_tool_loop_result_free(&r);
  agent_tool_registry_destroy(reg);
  agent_session_destroy(session);
  return (st == AGENT_OK) ? 0 : 1;
}
