#include "run_replay_endpoint.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include "agent/hmac_sha256.h"
#include "agent/ed25519.h"
#include "agent/json_c14n.h"

#include "base64.h"

#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace agentd {
namespace {

struct DbHandle {
#if defined(AGENT_HAVE_SQLITE3)
  sqlite3* db = nullptr;
#endif
  ~DbHandle() {
#if defined(AGENT_HAVE_SQLITE3)
    if (db) sqlite3_close(db);
#endif
  }
};

static Json::Value open_db_or_error(const AgentDb* db_or_null, DbHandle* out) {
  Json::Value o(Json::objectValue);
  if (!db_or_null || !db_or_null->is_open()) {
    o["ok"] = false;
    o["rpc_status"] = 404;
    o["error"] = "db disabled";
    return o;
  }
#if !defined(AGENT_HAVE_SQLITE3)
  (void)out;
  o["ok"] = false;
  o["rpc_status"] = 500;
  o["error"] = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return o;
#else
  if (!out) {
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = "internal error";
    return o;
  }
  const std::string path = db_or_null->path();
  if (path.empty()) {
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = "db path is empty";
    return o;
  }
  sqlite3* db = nullptr;
  const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = db ? sqlite3_errmsg(db) : "sqlite open failed";
    if (db) sqlite3_close(db);
    return o;
  }
  out->db = db;
  o["ok"] = true;
  return o;
#endif
}

static std::string stmt_text(sqlite3_stmt* st, int idx) {
  const unsigned char* t = sqlite3_column_text(st, idx);
  return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

static bool parse_i64_param(const std::optional<std::string>& v, int64_t* out) {
  if (!out) return false;
  if (!v.has_value()) return false;
  try {
    *out = (int64_t)std::stoll(*v);
    return true;
  } catch (...) {
    return false;
  }
}

static int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
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
  char errbuf[128] = {0};
  const agent_status_t st =
    agent_json_c14n_canonicalize(raw.data(), raw.size(), &canon, &canon_len, errbuf, sizeof(errbuf));
  if (st != AGENT_OK || !canon) {
    if (out_err) *out_err = errbuf[0] ? std::string(errbuf) : "c14n_failed";
    if (canon) agent_free(canon);
    return false;
  }
  out->assign(canon, canon_len);
  agent_free(canon);
  return true;
}

static bool parse_hex_bytes(const std::string& hex, std::vector<uint8_t>* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  if (hex.size() % 2 != 0) {
    if (out_err) *out_err = "hex string must have even length";
    return false;
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      if (out_err) *out_err = "hex string contains non-hex characters";
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

static bool parse_seed_bytes(const std::string& text, std::vector<uint8_t>* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  const std::string s = trim_copy(text);
  if (s.empty()) {
    if (out_err) *out_err = "empty seed";
    return false;
  }
  bool hex_candidate = (s.size() == 64);
  if (hex_candidate) {
    for (char c : s) {
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!ok) {
        hex_candidate = false;
        break;
      }
    }
  }
  if (hex_candidate) {
    std::string herr;
    if (!parse_hex_bytes(s, out, &herr)) {
      if (out_err) *out_err = herr;
      return false;
    }
  } else {
    std::string raw;
    std::string berr;
    if (!base64_decode(s, &raw, &berr)) {
      if (out_err) *out_err = berr.empty() ? "invalid base64 seed" : berr;
      return false;
    }
    out->assign(raw.begin(), raw.end());
  }
  if (out->size() != 32) {
    if (out_err) *out_err = "seed must be 32 bytes";
    return false;
  }
  return true;
}

#if defined(AGENT_HAVE_SQLITE3)
struct ReplayBundleLoad {
  bool ok = false;
  int status = 500;
  Json::Value error_body = Json::Value(Json::objectValue);
  std::string session_id;
  Json::Value bundle = Json::Value(Json::objectValue);
  std::string replay_sha256;
  std::string replay_sha256_alg;
  std::string replay_sha256_schema;
  std::string replay_error;
};

static ReplayBundleLoad replay_error(
  int status,
  const std::string& error,
  const std::string& details = std::string()
) {
  ReplayBundleLoad out;
  out.ok = false;
  out.status = status;
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["rpc_status"] = status;
  o["error"] = error;
  if (!details.empty()) o["details"] = details;
  out.error_body = o;
  return out;
}

static ReplayBundleLoad load_replay_bundle(sqlite3* db, int64_t run_id) {
  if (!db) return replay_error(500, "internal error");

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT session_id, request_json, response_json, replay_sha256, replay_sha256_alg, replay_sha256_schema, replay_error "
    "FROM runs WHERE run_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
    return replay_error(500, sqlite3_errmsg(db));
  }
  (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)run_id);
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    return replay_error(404, "run not found");
  }

  const std::string session_id = stmt_text(st, 0);
  const std::string request_json = stmt_text(st, 1);
  const std::string response_json = stmt_text(st, 2);
  std::string replay_sha256 = stmt_text(st, 3);
  std::string replay_sha256_alg = stmt_text(st, 4);
  std::string replay_sha256_schema = stmt_text(st, 5);
  std::string replay_error_text = stmt_text(st, 6);
  sqlite3_finalize(st);

  if (request_json.empty() || response_json.empty()) {
    ReplayBundleLoad out = replay_error(409, "replay_not_available");
    out.error_body["run_id"] = (Json::Int64)run_id;
    if (!session_id.empty()) out.error_body["session_id"] = session_id;
    if (!replay_error_text.empty()) out.error_body["replay_error"] = replay_error_text;
    return out;
  }

  Json::Value request_v;
  Json::Value response_v;
  std::string perr;
  if (!json_parse_any(request_json, &request_v, &perr)) {
    return replay_error(500, "replay_request_parse_failed", perr);
  }
  perr.clear();
  if (!json_parse_any(response_json, &response_v, &perr)) {
    return replay_error(500, "replay_response_parse_failed", perr);
  }

  Json::Value tool_records(Json::arrayValue);
  sqlite3_stmt* st2 = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT tool_name, tool_call_id, arguments_json, result_text, result_for_prompt_text, result_truncated_for_prompt "
                         "FROM tool_records WHERE run_id=? ORDER BY id;",
                         -1, &st2, nullptr) == SQLITE_OK && st2) {
    (void)sqlite3_bind_int64(st2, 1, (sqlite3_int64)run_id);
    while (sqlite3_step(st2) == SQLITE_ROW) {
      Json::Value tr(Json::objectValue);
      tr["tool_name"] = stmt_text(st2, 0);
      const std::string tool_call_id = stmt_text(st2, 1);
      if (!tool_call_id.empty()) tr["tool_call_id"] = tool_call_id;
      const std::string args_json = stmt_text(st2, 2);
      if (!args_json.empty()) tr["arguments_json"] = args_json;
      const std::string result_text = stmt_text(st2, 3);
      if (!result_text.empty()) tr["result_text"] = result_text;
      const std::string result_prompt = stmt_text(st2, 4);
      if (!result_prompt.empty()) tr["result_for_prompt_text"] = result_prompt;
      tr["result_truncated_for_prompt"] = sqlite3_column_int(st2, 5) ? true : false;
      tool_records.append(tr);
    }
    sqlite3_finalize(st2);
  } else if (st2) {
    sqlite3_finalize(st2);
  }

  Json::Value bundle(Json::objectValue);
  bundle["schema"] = "run_replay_bundle_v1";
  bundle["request"] = request_v;
  bundle["response"] = response_v;
  bundle["tool_records"] = tool_records;

  if (replay_sha256.empty()) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string bundle_json = Json::writeString(wb, bundle);
    char token[80] = {0};
    char err_buf[128] = {0};
    const agent_status_t st_hash =
      agent_json_c14n_sha256_token(bundle_json.data(), bundle_json.size(), token, err_buf, sizeof(err_buf));
    if (st_hash == AGENT_OK) {
      replay_sha256 = std::string(token);
      replay_sha256_alg = "agent_json_c14n_v1";
      replay_sha256_schema = "run_replay_bundle_v1";
    } else {
      const std::string err = err_buf[0] ? std::string(err_buf) : "replay_hash_failed";
      if (replay_error_text.empty()) replay_error_text = err;
      else replay_error_text.append(";").append(err);
    }
  }

  ReplayBundleLoad out;
  out.ok = true;
  out.status = 200;
  out.session_id = session_id;
  out.bundle = bundle;
  out.replay_sha256 = replay_sha256;
  out.replay_sha256_alg = replay_sha256_alg;
  out.replay_sha256_schema = replay_sha256_schema;
  out.replay_error = replay_error_text;
  return out;
}
#endif

}  // namespace

void handle_run_replay_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  int64_t run_id = 0;
  if (!parse_i64_param(query_get(req.query, "run_id"), &run_id) || run_id <= 0) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid run_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  DbHandle h;
  Json::Value db_open = open_db_or_error(db_or_null, &h);
  if (!db_open.isObject() || !db_open.get("ok", false).asBool()) {
    const int st = db_open.isMember("rpc_status") && db_open["rpc_status"].isInt() ? db_open["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(db_open);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = json_error_body("sqlite disabled");
  return;
#else
  ReplayBundleLoad load = load_replay_bundle(h.db, run_id);
  if (!load.ok) {
    resp->status = load.status;
    resp->body = json_stringify(load.error_body);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["run_id"] = (Json::Int64)run_id;
  if (!load.session_id.empty()) out["session_id"] = load.session_id;
  out["bundle"] = load.bundle;
  if (!load.replay_sha256.empty()) out["replay_sha256"] = load.replay_sha256;
  if (!load.replay_sha256_alg.empty()) out["replay_sha256_alg"] = load.replay_sha256_alg;
  if (!load.replay_sha256_schema.empty()) out["replay_sha256_schema"] = load.replay_sha256_schema;
  if (!load.replay_error.empty()) out["replay_error"] = load.replay_error;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

void handle_run_attestation_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  int64_t run_id = 0;
  if (!parse_i64_param(query_get(req.query, "run_id"), &run_id) || run_id <= 0) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid run_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  DbHandle h;
  Json::Value db_open = open_db_or_error(db_or_null, &h);
  if (!db_open.isObject() || !db_open.get("ok", false).asBool()) {
    const int st = db_open.isMember("rpc_status") && db_open["rpc_status"].isInt() ? db_open["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(db_open);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = json_error_body("sqlite disabled");
  return;
#else
  ReplayBundleLoad load = load_replay_bundle(h.db, run_id);
  if (!load.ok) {
    resp->status = load.status;
    resp->body = json_stringify(load.error_body);
    return;
  }

  Json::Value att(Json::objectValue);
  att["schema"] = "run_attestation_bundle_v1";
  att["created_utc_ms"] = (Json::Int64)now_utc_ms();
  att["replay_sha256"] = load.replay_sha256;
  att["replay_sha256_alg"] = load.replay_sha256_alg;
  att["replay_sha256_schema"] = load.replay_sha256_schema;
  att["run_id"] = (Json::Int64)run_id;
  if (!load.session_id.empty()) att["session_id"] = load.session_id;

  const bool hmac_kid_set = !cfg.run_attest_hmac_kid.empty();
  const bool hmac_key_set = !cfg.run_attest_hmac_key.empty();
  const bool ed_kid_set = !cfg.run_attest_ed25519_kid.empty();
  const bool ed_seed_set = !cfg.run_attest_ed25519_seed.empty();
  const bool any_hmac = hmac_kid_set || hmac_key_set;
  const bool any_ed = ed_kid_set || ed_seed_set;

  if (any_hmac || any_ed) {
    if (any_hmac && any_ed) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "attest_sign_config_invalid";
      o["details"] = "multiple signing modes configured";
      resp->status = 500;
      resp->body = json_stringify(o);
      return;
    }
    if (any_hmac && (!hmac_kid_set || !hmac_key_set)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "attest_sign_config_invalid";
      o["details"] = "missing AGENTD_RUN_ATTEST_HMAC_KID or AGENTD_RUN_ATTEST_HMAC_KEY";
      resp->status = 500;
      resp->body = json_stringify(o);
      return;
    }
    if (any_ed && (!ed_kid_set || !ed_seed_set)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "attest_sign_config_invalid";
      o["details"] = "missing AGENTD_RUN_ATTEST_ED25519_KID or AGENTD_RUN_ATTEST_ED25519_SEED";
      resp->status = 500;
      resp->body = json_stringify(o);
      return;
    }
    std::string canon;
    std::string cerr;
    if (!canonical_json_bytes(att, &canon, &cerr)) {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["rpc_status"] = 500;
      o["error"] = "attest_sign_failed";
      o["details"] = cerr.empty() ? "c14n_failed" : cerr;
      resp->status = 500;
      resp->body = json_stringify(o);
      return;
    }
    Json::Value sig(Json::objectValue);
    if (any_hmac) {
      uint8_t mac[32];
      agent_hmac_sha256(cfg.run_attest_hmac_key.data(), cfg.run_attest_hmac_key.size(), canon.data(), canon.size(), mac);
      sig["alg"] = "hmac-sha256";
      sig["kid"] = cfg.run_attest_hmac_kid;
      sig["sig"] = base64_encode(mac, sizeof(mac));
    } else {
      std::vector<uint8_t> seed;
      std::string serr;
      if (!parse_seed_bytes(cfg.run_attest_ed25519_seed, &seed, &serr)) {
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["rpc_status"] = 500;
        o["error"] = "attest_sign_failed";
        o["details"] = serr.empty() ? "invalid_ed25519_seed" : serr;
        resp->status = 500;
        resp->body = json_stringify(o);
        return;
      }
      uint8_t pk[32];
      agent_ed25519_publickey(seed.data(), pk);
      uint8_t sig_bytes[64];
      agent_ed25519_sign(canon.data(), canon.size(), seed.data(), pk, sig_bytes);
      sig["alg"] = "ed25519";
      sig["kid"] = cfg.run_attest_ed25519_kid;
      sig["sig"] = base64_encode(sig_bytes, sizeof(sig_bytes));
    }
    sig["ts_utc_ms"] = att["created_utc_ms"];
    sig["hash_alg"] = "agent_json_c14n_v1";
    sig["signing_schema"] = "run_attestation_bundle_v1";
    att["attest"] = sig;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["run_id"] = (Json::Int64)run_id;
  if (!load.session_id.empty()) out["session_id"] = load.session_id;
  out["attestation"] = att;
  if (!load.replay_error.empty()) out["replay_error"] = load.replay_error;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

}  // namespace agentd
