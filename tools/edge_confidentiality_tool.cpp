#include "edge_confidentiality.h"
#include "json_util.h"

#include <json/json.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

[[noreturn]] static void die(const char* msg) {
  std::fprintf(stderr, "%s\n", msg ? msg : "error");
  std::exit(2);
}

static std::string read_all_stdin() {
  std::string out;
  std::vector<char> buf(4096);
  while (true) {
    const size_t n = std::fread(buf.data(), 1, buf.size(), stdin);
    if (n > 0) out.append(buf.data(), n);
    if (n < buf.size()) break;
  }
  return out;
}

static bool split_key_arg(const std::string& arg, std::string* out_kid, std::string* out_secret) {
  if (out_kid) out_kid->clear();
  if (out_secret) out_secret->clear();
  const size_t eq = arg.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= arg.size()) return false;
  if (out_kid) *out_kid = arg.substr(0, eq);
  if (out_secret) *out_secret = arg.substr(eq + 1);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool seal = false;
  bool open = false;
  std::string kid;
  std::string secret;
  std::map<std::string, std::string> keys;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--seal") {
      seal = true;
    } else if (a == "--open") {
      open = true;
    } else if (a == "--kid") {
      if (i + 1 >= argc) die("missing value for --kid");
      kid = argv[++i] ? argv[i] : "";
    } else if (a == "--secret") {
      if (i + 1 >= argc) die("missing value for --secret");
      secret = argv[++i] ? argv[i] : "";
    } else if (a == "--key") {
      if (i + 1 >= argc) die("missing value for --key");
      std::string k;
      std::string s;
      if (!split_key_arg(argv[++i] ? argv[i] : "", &k, &s)) die("invalid --key (expected <kid>=<secret>)");
      keys[k] = s;
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
        "Usage:\n"
        "  edge_confidentiality_tool --seal --kid <kid> --secret <secret> < body.json > body_enc.json\n"
        "  edge_confidentiality_tool --open --key <kid>=<secret> [--key <kid>=<secret> ...] < body_enc.json > body.json\n");
      return 0;
    } else {
      die("unknown arg");
    }
  }

  if (seal == open) die("choose exactly one of --seal or --open");
  const std::string input = read_all_stdin();

  Json::Value v;
  std::string perr;
  if (!agentd::json_parse_any(input, &v, &perr)) die(("invalid JSON: " + perr).c_str());

  if (seal) {
    if (kid.empty() || secret.empty()) die("--seal requires --kid and --secret");
    if (!v.isObject()) die("--seal expects a JSON object on stdin");
    Json::Value enc;
    std::string err;
    if (!agentd::edge_confidentiality_seal_json_object(kid, secret, v, &enc, &err)) {
      die(err.empty() ? "seal failed" : err.c_str());
    }
    std::printf("%s\n", agentd::json_stringify(enc).c_str());
    return 0;
  }

  if (keys.empty() && !kid.empty() && !secret.empty()) keys[kid] = secret;
  if (keys.empty()) die("--open requires at least one --key <kid>=<secret> or --kid/--secret");
  Json::Value body;
  std::string out_kid;
  std::string error_code;
  std::string err;
  if (!agentd::edge_confidentiality_open_json_object(v, keys, &body, &out_kid, &error_code, &err)) {
    die(err.empty() ? (error_code.empty() ? "open failed" : error_code.c_str()) : err.c_str());
  }
  std::printf("%s\n", agentd::json_stringify(body).c_str());
  return 0;
}
