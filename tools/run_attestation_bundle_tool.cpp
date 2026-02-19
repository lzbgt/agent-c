#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"

#include "base64.h"

#include <json/json.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s --replay-json <path>|--stdin [options]\n"
    "\n"
    "options:\n"
    "  --out <path>               write output to file (default: stdout)\n"
    "  --run-id <id>              optional run_id for metadata\n"
    "  --session-id <id>          optional session_id for metadata\n"
    "  --agent-id <id>            optional agent_id for metadata\n"
    "  --deployment-id <id>       optional deployment_id for metadata\n"
    "  --issuer <id>              optional issuer id\n"
    "  --created-utc-ms <ms>      override created timestamp (int64)\n"
    "  --kid <id>                 key id for signing (required when signing)\n"
    "  --hmac-key-hex <hex>       HMAC key (hex string)\n"
    "  --hmac-key <ascii>         HMAC key (raw ASCII)\n"
    "  --hmac-key-file <path>     HMAC key (raw bytes)\n"
    "  --no-sign                  emit unsigned attestation bundle\n"
    "  --help                     show this help\n",
    argv0
  );
}

static bool read_file(const std::string& path, std::string* out) {
  if (!out) return false;
  std::ifstream f(path, std::ios::in | std::ios::binary);
  if (!f.is_open()) return false;
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  *out = std::move(data);
  return true;
}

static bool parse_json(const std::string& text, Json::Value* out, std::string* err) {
  if (!out) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  if (!reader->parse(text.data(), text.data() + text.size(), out, &errs)) {
    if (err) *err = errs;
    return false;
  }
  return true;
}

static bool canonical_json_bytes(const Json::Value& v, std::string* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string raw = Json::writeString(wb, v);

  char* canon = nullptr;
  size_t canon_len = 0;
  std::array<char, 256> errbuf{};
  const agent_status_t st =
    agent_json_c14n_canonicalize(raw.data(), raw.size(), &canon, &canon_len, errbuf.data(), errbuf.size());
  if (st != AGENT_OK || !canon) {
    if (out_err) *out_err = std::string("canonicalize failed: ") + (errbuf[0] ? errbuf.data() : "unknown");
    if (canon) agent_free(canon);
    return false;
  }
  out->assign(canon, canon_len);
  agent_free(canon);
  return true;
}

static bool sha256_token_of_json(const Json::Value& v, std::string* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string raw = Json::writeString(wb, v);
  char token[80];
  std::array<char, 256> errbuf{};
  const agent_status_t st = agent_json_c14n_sha256_token(raw.data(), raw.size(), token, errbuf.data(), errbuf.size());
  if (st != AGENT_OK) {
    if (out_err) *out_err = std::string("sha256 token failed: ") + (errbuf[0] ? errbuf.data() : "unknown");
    return false;
  }
  *out = token;
  return true;
}

static bool parse_hex_bytes(const std::string& hex, std::vector<uint8_t>* out, std::string* err) {
  if (err) err->clear();
  if (!out) return false;
  out->clear();
  if (hex.size() % 2 != 0) {
    if (err) *err = "hex string must have even length";
    return false;
  }
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const char c1 = hex[i];
    const char c2 = hex[i + 1];
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
      return -1;
    };
    const int n1 = nibble(c1);
    const int n2 = nibble(c2);
    if (n1 < 0 || n2 < 0) {
      if (err) *err = "hex string contains non-hex characters";
      return false;
    }
    out->push_back(static_cast<uint8_t>((n1 << 4) | n2));
  }
  return true;
}

static int64_t now_utc_ms() {
  using namespace std::chrono;
  const auto now = time_point_cast<milliseconds>(system_clock::now());
  return static_cast<int64_t>(now.time_since_epoch().count());
}

int main(int argc, char** argv) {
  std::string replay_path;
  bool use_stdin = false;
  std::string out_path;
  std::string run_id;
  std::string session_id;
  std::string agent_id;
  std::string deployment_id;
  std::string issuer_id;
  std::string kid;
  std::vector<uint8_t> hmac_key;
  bool sign = true;
  int64_t created_utc_ms = 0;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--replay-json" && i + 1 < argc) {
      replay_path = argv[++i];
    } else if (arg == "--stdin") {
      use_stdin = true;
    } else if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--run-id" && i + 1 < argc) {
      run_id = argv[++i];
    } else if (arg == "--session-id" && i + 1 < argc) {
      session_id = argv[++i];
    } else if (arg == "--agent-id" && i + 1 < argc) {
      agent_id = argv[++i];
    } else if (arg == "--deployment-id" && i + 1 < argc) {
      deployment_id = argv[++i];
    } else if (arg == "--issuer" && i + 1 < argc) {
      issuer_id = argv[++i];
    } else if (arg == "--created-utc-ms" && i + 1 < argc) {
      created_utc_ms = std::stoll(argv[++i]);
    } else if (arg == "--kid" && i + 1 < argc) {
      kid = argv[++i];
    } else if (arg == "--hmac-key-hex" && i + 1 < argc) {
      std::string hex = argv[++i];
      std::string err;
      if (!parse_hex_bytes(hex, &hmac_key, &err)) {
        std::fprintf(stderr, "invalid --hmac-key-hex: %s\n", err.c_str());
        return 2;
      }
    } else if (arg == "--hmac-key" && i + 1 < argc) {
      std::string key = argv[++i];
      hmac_key.assign(key.begin(), key.end());
    } else if (arg == "--hmac-key-file" && i + 1 < argc) {
      std::string path = argv[++i];
      std::string data;
      if (!read_file(path, &data)) {
        std::fprintf(stderr, "failed to read --hmac-key-file\n");
        return 2;
      }
      hmac_key.assign(data.begin(), data.end());
    } else if (arg == "--no-sign") {
      sign = false;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      usage(argv[0]);
      return 2;
    }
  }

  if (!use_stdin && replay_path.empty()) {
    std::fprintf(stderr, "missing --replay-json or --stdin\n");
    usage(argv[0]);
    return 2;
  }
  if (use_stdin && !replay_path.empty()) {
    std::fprintf(stderr, "use either --replay-json or --stdin, not both\n");
    return 2;
  }

  if (created_utc_ms <= 0) created_utc_ms = now_utc_ms();

  std::string replay_text;
  if (use_stdin) {
    replay_text.assign((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  } else if (!read_file(replay_path, &replay_text)) {
    std::fprintf(stderr, "failed to read replay json: %s\n", replay_path.c_str());
    return 2;
  }

  Json::Value replay_root;
  std::string jerr;
  if (!parse_json(replay_text, &replay_root, &jerr)) {
    std::fprintf(stderr, "invalid json: %s\n", jerr.c_str());
    return 2;
  }

  Json::Value bundle = replay_root;
  std::string replay_sha256;
  std::string replay_sha256_alg;
  std::string replay_sha256_schema;

  if (replay_root.isObject() && replay_root.isMember("bundle") && replay_root["bundle"].isObject()) {
    bundle = replay_root["bundle"];
    if (replay_root.isMember("replay_sha256") && replay_root["replay_sha256"].isString()) {
      replay_sha256 = replay_root["replay_sha256"].asString();
    }
    if (replay_root.isMember("replay_sha256_alg") && replay_root["replay_sha256_alg"].isString()) {
      replay_sha256_alg = replay_root["replay_sha256_alg"].asString();
    }
    if (replay_root.isMember("replay_sha256_schema") && replay_root["replay_sha256_schema"].isString()) {
      replay_sha256_schema = replay_root["replay_sha256_schema"].asString();
    }
  }

  if (replay_sha256.empty()) {
    std::string err;
    if (!sha256_token_of_json(bundle, &replay_sha256, &err)) {
      std::fprintf(stderr, "failed to compute replay_sha256: %s\n", err.c_str());
      return 2;
    }
    replay_sha256_alg = "agent_json_c14n_v1";
  }
  if (replay_sha256_alg.empty()) replay_sha256_alg = "agent_json_c14n_v1";
  if (replay_sha256_schema.empty()) {
    if (bundle.isObject() && bundle.isMember("schema") && bundle["schema"].isString()) {
      replay_sha256_schema = bundle["schema"].asString();
    } else {
      replay_sha256_schema = "run_replay_bundle_v1";
    }
  }

  Json::Value att_bundle(Json::objectValue);
  att_bundle["schema"] = "run_attestation_bundle_v1";
  att_bundle["created_utc_ms"] = Json::Int64(created_utc_ms);
  att_bundle["replay_sha256"] = replay_sha256;
  att_bundle["replay_sha256_alg"] = replay_sha256_alg;
  att_bundle["replay_sha256_schema"] = replay_sha256_schema;
  if (!run_id.empty()) att_bundle["run_id"] = run_id;
  if (!session_id.empty()) att_bundle["session_id"] = session_id;
  if (!agent_id.empty()) att_bundle["agent_id"] = agent_id;
  if (!deployment_id.empty()) att_bundle["deployment_id"] = deployment_id;
  if (!issuer_id.empty()) {
    Json::Value issuer(Json::objectValue);
    issuer["id"] = issuer_id;
    att_bundle["issuer"] = issuer;
  }

  if (sign) {
    if (hmac_key.empty()) {
      std::fprintf(stderr, "signing requested but no HMAC key provided\n");
      return 2;
    }
    if (kid.empty()) {
      std::fprintf(stderr, "signing requested but --kid not provided\n");
      return 2;
    }
    std::string canon;
    std::string cerr;
    if (!canonical_json_bytes(att_bundle, &canon, &cerr)) {
      std::fprintf(stderr, "failed to canonicalize bundle: %s\n", cerr.c_str());
      return 2;
    }
    uint8_t mac[32];
    agent_hmac_sha256(hmac_key.data(), hmac_key.size(), canon.data(), canon.size(), mac);
    const std::string sig_b64 = base64_encode(mac, sizeof(mac));

    Json::Value attest(Json::objectValue);
    attest["alg"] = "hmac-sha256";
    attest["kid"] = kid;
    attest["sig"] = sig_b64;
    attest["ts_utc_ms"] = Json::Int64(created_utc_ms);
    attest["hash_alg"] = "agent_json_c14n_v1";
    attest["signing_schema"] = "run_attestation_bundle_v1";
    att_bundle["attest"] = attest;
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  const std::string out_json = Json::writeString(wb, att_bundle);

  if (out_path.empty()) {
    std::cout << out_json;
  } else {
    std::ofstream out(out_path, std::ios::out | std::ios::binary);
    if (!out.is_open()) {
      std::fprintf(stderr, "failed to write output: %s\n", out_path.c_str());
      return 2;
    }
    out << out_json;
  }

  return 0;
}
