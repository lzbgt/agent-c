#include "blob_endpoints.h"

#include "agent_sha256.h"
#include "base64.h"
#include "blob_object_store.h"
#include "blob_tier_policy.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

bool blob_id_is_safe(const std::string& s) {
  const std::string prefix = "sha256:";
  if (s.size() != prefix.size() + 64) return false;
  if (s.rfind(prefix, 0) != 0) return false;
  for (size_t i = prefix.size(); i < s.size(); i++) {
    const char c = s[i];
    const bool is_hex =
      (c >= '0' && c <= '9') ||
      (c >= 'a' && c <= 'f') ||
      (c >= 'A' && c <= 'F');
    if (!is_hex) return false;
  }
  return true;
}

std::string blob_rel_path_from_hex(const std::string& hex) {
  if (hex.size() < 4) return "";
  std::string rel = "blobs/sha256/";
  rel.reserve(7 + hex.size());
  rel.append(hex.substr(0, 2));
  rel.push_back('/');
  rel.append(hex.substr(2, 2));
  rel.push_back('/');
  rel.append(hex);
  return rel;
}

bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& p) {
  std::error_code ec;
  const std::filesystem::path abs_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) return false;
  const std::filesystem::path abs_p = std::filesystem::weakly_canonical(p, ec);
  if (ec) return false;
  auto it = abs_root.begin();
  auto jt = abs_p.begin();
  for (; it != abs_root.end() && jt != abs_p.end(); ++it, ++jt) {
    if (*it != *jt) return false;
  }
  return it == abs_root.end();
}

bool write_bytes_atomic(const std::filesystem::path& dst, const std::string& bytes, std::string* out_error) {
  if (out_error) out_error->clear();
  std::error_code ec;
  std::filesystem::create_directories(dst.parent_path(), ec);
  if (ec) {
    if (out_error) *out_error = std::string("create_directories failed: ") + ec.message();
    return false;
  }

  std::mt19937_64 rng((uint64_t)now_utc_ms());
  const std::string suffix = std::to_string((unsigned long long)rng());
  const std::filesystem::path tmp = dst.parent_path() / (dst.filename().string() + ".tmp." + suffix);

  std::ofstream os(tmp, std::ios::binary);
  if (!os.is_open()) {
    if (out_error) *out_error = "failed to open temp file";
    return false;
  }
  os.write(bytes.data(), (std::streamsize)bytes.size());
  if (!os) {
    if (out_error) *out_error = "failed to write temp file";
    return false;
  }
  os.close();

  ec.clear();
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    if (out_error) *out_error = std::string("rename failed: ") + ec.message();
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

struct ByteRange {
  bool valid = false;
  bool partial = false;
  int64_t start = 0;
  int64_t end = 0;
};

ByteRange parse_range_header(const std::string& header, int64_t size) {
  ByteRange out;
  if (header.empty() || size <= 0) return out;
  const std::string prefix = "bytes=";
  if (header.rfind(prefix, 0) != 0) return out;
  std::string spec = header.substr(prefix.size());
  const auto dash = spec.find('-');
  if (dash == std::string::npos) return out;

  const std::string a = trim_copy(spec.substr(0, dash));
  const std::string b = trim_copy(spec.substr(dash + 1));

  int64_t start = 0;
  int64_t end = size - 1;
  if (a.empty()) {
    // suffix length: "-N"
    if (b.empty()) return out;
    try {
      const int64_t suffix = std::stoll(b);
      if (suffix <= 0) return out;
      start = size - suffix;
      if (start < 0) start = 0;
    } catch (...) {
      return out;
    }
  } else {
    try {
      start = std::stoll(a);
    } catch (...) {
      return out;
    }
    if (!b.empty()) {
      try {
        end = std::stoll(b);
      } catch (...) {
        return out;
      }
    }
  }

  if (start < 0 || end < 0 || start > end || start >= size) return out;
  if (end >= size) end = size - 1;

  out.valid = true;
  out.partial = true;
  out.start = start;
  out.end = end;
  return out;
}

bool parse_blob_upload_json(const HttpRequest& req, std::string* out_bytes, std::string* out_mime, bool* out_retain) {
  if (out_bytes) out_bytes->clear();
  if (out_mime) out_mime->clear();
  if (out_retain) *out_retain = true;

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    return false;
  }
  const std::string data_b64 =
    body.isMember("data_base64") && body["data_base64"].isString() ? body["data_base64"].asString() : "";
  if (data_b64.empty()) {
    return false;
  }
  std::string bytes;
  std::string berr;
  if (!base64_decode(data_b64, &bytes, &berr)) {
    return false;
  }
  if (out_bytes) *out_bytes = std::move(bytes);
  if (out_mime && body.isMember("mime") && body["mime"].isString()) {
    *out_mime = body["mime"].asString();
  }
  if (out_retain && body.isMember("retain") && body["retain"].isBool()) {
    *out_retain = body["retain"].asBool();
  }
  return true;
}

bool parse_blob_ids_body(
  const HttpRequest& req,
  std::vector<std::string>* out_ids,
  Json::Value* out_body,
  std::string* out_error
) {
  if (out_ids) out_ids->clear();
  if (out_body) *out_body = Json::Value(Json::objectValue);
  if (out_error) out_error->clear();
  if (!out_ids) return false;

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    if (out_error) *out_error = "invalid JSON body";
    return false;
  }
  if (out_body) *out_body = body;

  std::unordered_set<std::string> seen;
  auto maybe_add = [&](const std::string& v) {
    if (v.empty()) return;
    if (!blob_id_is_safe(v)) return;
    if (seen.insert(v).second) out_ids->push_back(v);
  };

  if (body.isMember("blob_id") && body["blob_id"].isString()) {
    maybe_add(body["blob_id"].asString());
  }
  if (body.isMember("blob_ids") && body["blob_ids"].isArray()) {
    for (const auto& v : body["blob_ids"]) {
      if (v.isString()) maybe_add(v.asString());
    }
  }

  if (out_ids->empty()) {
    if (out_error) *out_error = "missing blob_id(s)";
    return false;
  }
  if (out_ids->size() > 200) {
    if (out_error) *out_error = "too many blob_ids (max 200)";
    return false;
  }
  return true;
}

}  // namespace

void handle_blob_upload_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }
  if (cfg.state_dir.empty()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"state_dir not configured"})";
    return;
  }

  std::string bytes;
  std::string mime;
  bool retain = true;

  const std::string content_type = header_get_ci(req.headers, "content-type");
  if (content_type.rfind("application/json", 0) == 0) {
    if (!parse_blob_upload_json(req, &bytes, &mime, &retain)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid JSON body (expected data_base64)\"}";
      return;
    }
  } else {
    bytes = req.body;
    if (mime.empty()) mime = content_type;
    if (const auto r = query_get(req.query, "retain")) {
      retain = string_to_bool(*r);
    }
  }

  if (bytes.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing blob bytes"})";
    return;
  }

  if (cfg.upload_max_bytes > 0 && bytes.size() > cfg.upload_max_bytes) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"blob too large"})";
    return;
  }

  char hex[65];
  agent_sha256_hex_of_bytes(bytes.data(), bytes.size(), hex);
  const std::string hex_s(hex);
  const std::string blob_id = std::string("sha256:") + hex_s;
  const std::string rel_path = blob_rel_path_from_hex(hex_s);
  if (rel_path.empty()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"failed to compute blob path"})";
    return;
  }
  const std::filesystem::path abs_path = (std::filesystem::path(cfg.state_dir) / rel_path).lexically_normal();
  const bool want_object = (cfg.blob_store_mode == "object");
  const bool cache_none = (cfg.blob_store_cache_mode == "none");
  const bool keep_local = !want_object || !cache_none;
  std::string object_key;
  if (want_object) {
    std::string oerr;
    if (!blob_object_store_is_configured(cfg, &oerr)) {
      resp->status = 500;
      resp->body = std::string("{\"ok\":false,\"error\":\"") + (oerr.empty() ? "object store not configured" : oerr) + "\"}";
      return;
    }
    object_key = blob_object_store_key_for_hex(cfg, hex_s);
    if (object_key.empty()) {
      resp->status = 500;
      resp->body = R"({"ok":false,"error":"failed to compute object store key"})";
      return;
    }
  }

  AgentDb::BlobManifestRow existing;
  std::string db_err;
  const bool found = db_or_null->get_blob_manifest(blob_id, &existing, &db_err);
  if (!db_err.empty()) {
    resp->status = 500;
    resp->body = std::string("{\"ok\":false,\"error\":\"") + db_err + "\"}";
    return;
  }

  const int64_t now_ms = now_utc_ms();
  std::string tier = "local";
  std::string location = rel_path;
  std::string etag;
  std::string storage_class;
  auto ensure_local = [&](std::string* out_err) -> bool {
    if (!keep_local) return true;
    if (std::filesystem::exists(abs_path)) return true;
    return write_bytes_atomic(abs_path, bytes, out_err);
  };
  auto cleanup_local = [&]() {
    if (keep_local) return;
    std::error_code ec;
    if (std::filesystem::exists(abs_path, ec)) {
      std::filesystem::remove(abs_path, ec);
    }
  };
  if (found) {
    if (existing.size_bytes != (int64_t)bytes.size()) {
      resp->status = 409;
      resp->body = R"({"ok":false,"error":"blob already exists with different size"})";
      return;
    }
    std::string werr;
    if (!ensure_local(&werr)) {
      resp->status = 500;
      resp->body = std::string("{\"ok\":false,\"error\":\"") + werr + "\"}";
      return;
    }
    const std::string next_mime = !existing.mime.empty() ? existing.mime : mime;
    if (want_object) {
      std::string perr;
      if (existing.tier != "object" || existing.location != object_key) {
        if (!blob_object_store_put(cfg, object_key, bytes, next_mime, now_ms, &etag, &perr)) {
          cleanup_local();
          resp->status = 502;
          resp->body = std::string("{\"ok\":false,\"error\":\"") + perr + "\"}";
          return;
        }
      }
      tier = "object";
      location = object_key;
      if (etag.empty()) etag = existing.etag;
      storage_class = existing.storage_class;
      (void)db_or_null->update_blob_manifest_location(
        blob_id,
        next_mime,
        tier,
        location,
        etag,
        storage_class,
        now_ms,
        nullptr);
    } else {
      tier = "local";
      location = rel_path;
      (void)db_or_null->update_blob_manifest_location(
        blob_id,
        next_mime,
        tier,
        location,
        existing.etag,
        existing.storage_class,
        now_ms,
        nullptr);
    }
  } else {
    std::string werr;
    if (!ensure_local(&werr)) {
      resp->status = 500;
      resp->body = std::string("{\"ok\":false,\"error\":\"") + werr + "\"}";
      return;
    }
    if (want_object) {
      std::string perr;
      if (!blob_object_store_put(cfg, object_key, bytes, mime, now_ms, &etag, &perr)) {
        cleanup_local();
        resp->status = 502;
        resp->body = std::string("{\"ok\":false,\"error\":\"") + perr + "\"}";
        return;
      }
      tier = "object";
      location = object_key;
    }
    AgentDb::BlobManifestRow row;
    row.blob_id = blob_id;
    row.size_bytes = (int64_t)bytes.size();
    row.mime = mime;
    row.sha256_hex = hex_s;
    row.created_utc_ms = now_ms;
    row.last_access_utc_ms = now_ms;
    row.ref_count = retain ? 1 : 0;
    row.tier = tier;
    row.location = location;
    row.etag = etag;
    row.storage_class = storage_class;
    if (!db_or_null->insert_blob_manifest(row, &db_err)) {
      cleanup_local();
      resp->status = 500;
      resp->body = std::string("{\"ok\":false,\"error\":\"") + db_err + "\"}";
      return;
    }
  }
  cleanup_local();

  int64_t ref_count = 0;
  if (found && retain) {
    (void)db_or_null->adjust_blob_ref_count(blob_id, 1, &ref_count, nullptr);
  } else if (!found) {
    ref_count = retain ? 1 : 0;
  } else {
    ref_count = existing.ref_count;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["blob_id"] = blob_id;
  out["sha256_hex"] = hex_s;
  out["size_bytes"] = (Json::Int64)bytes.size();
  out["mime"] = mime.empty() ? Json::Value(Json::nullValue) : Json::Value(mime);
  out["tier"] = tier;
  out["location"] = location;
  out["etag"] = etag.empty() ? Json::Value(Json::nullValue) : Json::Value(etag);
  out["storage_class"] = storage_class.empty() ? Json::Value(Json::nullValue) : Json::Value(storage_class);
  out["ref_count"] = (Json::Int64)ref_count;
  out["created_utc_ms"] = (Json::Int64)now_ms;
  out["last_access_utc_ms"] = (Json::Int64)now_ms;
  out["already_present"] = found;
  resp->status = 200;
  resp->body = json_stringify(out);
}

void handle_blob_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto blob_q = query_get(req.query, "blob_id");
  if (!blob_q || blob_q->empty()) {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"missing blob_id"})";
    return;
  }
  const std::string blob_id = *blob_q;
  if (!blob_id_is_safe(blob_id)) {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"invalid blob_id"})";
    return;
  }
  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }
  AgentDb::BlobManifestRow row;
  std::string err;
  if (!db_or_null->get_blob_manifest(blob_id, &row, &err)) {
    if (!err.empty()) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = std::string("{\"ok\":false,\"error\":\"") + err + "\"}";
      return;
    }
    resp->status = 404;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"blob not found"})";
    return;
  }

  if (row.tier == "archive") {
    Json::Value out(Json::objectValue);
    out["ok"] = false;
    out["error"] = "blob archived; restore required";
    out["blob_id"] = row.blob_id;
    out["tier"] = row.tier;
    resp->status = 409;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = json_stringify(out);
    return;
  }

  const std::string range_h = header_get_ci(req.headers, "range");

  auto serve_local = [&](const std::filesystem::path& abs, const std::string& mime) -> bool {
    std::error_code ec;
    if (!std::filesystem::exists(abs, ec) || !std::filesystem::is_regular_file(abs, ec)) {
      resp->status = 404;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"blob missing"})";
      return false;
    }
    const int64_t size = (int64_t)std::filesystem::file_size(abs, ec);
    if (ec || size < 0) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to stat blob"})";
      return false;
    }
    ByteRange range = parse_range_header(range_h, size);
    if (!range_h.empty() && !range.valid) {
      resp->status = 416;
      resp->headers["Content-Range"] = "bytes */" + std::to_string(size);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"invalid range"})";
      return false;
    }
    int64_t start = 0;
    int64_t end = size - 1;
    if (range.valid) {
      start = range.start;
      end = range.end;
    }
    const int64_t len = end - start + 1;
    if (len < 0) {
      resp->status = 416;
      resp->headers["Content-Range"] = "bytes */" + std::to_string(size);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"invalid range"})";
      return false;
    }
    std::ifstream in(abs, std::ios::binary);
    if (!in.is_open()) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to open blob"})";
      return false;
    }
    if (start > 0) {
      in.seekg((std::streamoff)start, std::ios::beg);
      if (!in) {
        resp->status = 500;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"failed to seek blob"})";
        return false;
      }
    }
    std::string out_bytes;
    out_bytes.resize((size_t)len);
    in.read(out_bytes.data(), (std::streamsize)len);
    if (!in) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to read blob"})";
      return false;
    }

    resp->status = range.valid ? 206 : 200;
    resp->headers["Content-Type"] = mime.empty() ? "application/octet-stream" : mime;
    resp->headers["Accept-Ranges"] = "bytes";
    resp->headers["Content-Length"] = std::to_string((unsigned long long)out_bytes.size());
    resp->headers["ETag"] = "\"" + blob_id + "\"";
    if (range.valid) {
      resp->headers["Content-Range"] =
        "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size);
    }
    resp->body = std::move(out_bytes);
    return true;
  };

  auto respond_from_object = [&](const HttpClientResult& res, const std::string& mime_hint) {
    resp->status = (int)res.http_status;
    const auto ct_it = res.response_headers.find("content-type");
    const std::string ct = !mime_hint.empty()
      ? mime_hint
      : (ct_it != res.response_headers.end() ? ct_it->second : "application/octet-stream");
    resp->headers["Content-Type"] = ct;
    resp->headers["Accept-Ranges"] = "bytes";
    resp->headers["Content-Length"] = std::to_string((unsigned long long)res.response_body.size());
    resp->headers["ETag"] = "\"" + blob_id + "\"";
    const auto cr_it = res.response_headers.find("content-range");
    if (cr_it != res.response_headers.end()) {
      resp->headers["Content-Range"] = cr_it->second;
    }
    resp->body = res.response_body;
  };

  if (row.tier == "local") {
    if (cfg.state_dir.empty()) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"state_dir not configured"})";
      return;
    }
    const std::filesystem::path root(cfg.state_dir);
    const std::filesystem::path abs = (root / row.location).lexically_normal();
    if (!path_is_within_root(root, abs)) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"blob path invalid"})";
      return;
    }
    if (!serve_local(abs, row.mime)) return;
  } else if (row.tier == "object") {
    const bool want_cache = (cfg.blob_store_cache_mode == "read-through");
    const bool want_proxy = (cfg.blob_store_read_mode == "proxy");
    std::filesystem::path cache_abs;
    bool cache_valid = false;
    if (!cfg.state_dir.empty()) {
      const std::string cache_rel = blob_rel_path_from_hex(row.sha256_hex);
      if (!cache_rel.empty()) {
        const std::filesystem::path root(cfg.state_dir);
        const std::filesystem::path abs = (root / cache_rel).lexically_normal();
        if (path_is_within_root(root, abs)) {
          cache_abs = abs;
          cache_valid = true;
        }
      }
    }
    if (cache_valid) {
      std::error_code ec;
      if (std::filesystem::exists(cache_abs, ec) && std::filesystem::is_regular_file(cache_abs, ec)) {
        if (!serve_local(cache_abs, row.mime)) return;
        (void)db_or_null->update_blob_manifest_access(blob_id, now_utc_ms(), nullptr);
        return;
      }
    }

    const size_t max_bytes = cfg.blob_store_cache_max_bytes > 0 ? cfg.blob_store_cache_max_bytes : cfg.upload_max_bytes;
    if (want_cache && range_h.empty() && cache_valid && max_bytes > 0) {
      HttpClientResult res;
      std::string perr;
      if (blob_object_store_get(cfg, row.location, /*range_header=*/"", max_bytes, now_utc_ms(), &res, &perr)) {
        std::string werr;
        if (!write_bytes_atomic(cache_abs, res.response_body, &werr)) {
          // Best-effort cache; still return data.
        }
        respond_from_object(res, row.mime);
        (void)db_or_null->update_blob_manifest_access(blob_id, now_utc_ms(), nullptr);
        return;
      }
    }

    if (want_proxy) {
      HttpClientResult res;
      std::string perr;
      if (!blob_object_store_get(cfg, row.location, range_h, max_bytes, now_utc_ms(), &res, &perr)) {
        resp->status = 502;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = std::string("{\"ok\":false,\"error\":\"") + perr + "\"}";
        return;
      }
      respond_from_object(res, row.mime);
      (void)db_or_null->update_blob_manifest_access(blob_id, now_utc_ms(), nullptr);
      return;
    }

    std::string url;
    std::string perr;
    if (!blob_object_store_presign_url(cfg, "GET", row.location, now_utc_ms(), cfg.blob_store_presign_ttl_sec, &url, &perr)) {
      resp->status = 502;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = std::string("{\"ok\":false,\"error\":\"") + perr + "\"}";
      return;
    }
    resp->status = 302;
    resp->headers["Location"] = url;
    resp->headers["Cache-Control"] = "private, max-age=0, no-cache";
    return;
  } else {
    resp->status = 500;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"unsupported blob tier"})";
    return;
  }

  (void)db_or_null->update_blob_manifest_access(blob_id, now_utc_ms(), nullptr);
}

void handle_blob_meta_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto blob_q = query_get(req.query, "blob_id");
  if (!blob_q || blob_q->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing blob_id"})";
    return;
  }
  const std::string blob_id = *blob_q;
  if (!blob_id_is_safe(blob_id)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid blob_id"})";
    return;
  }
  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

  AgentDb::BlobManifestRow row;
  std::string err;
  if (!db_or_null->get_blob_manifest(blob_id, &row, &err)) {
    if (!err.empty()) {
      resp->status = 500;
      resp->body = std::string("{\"ok\":false,\"error\":\"") + err + "\"}";
      return;
    }
    resp->status = 404;
    resp->body = R"({"ok":false,"error":"blob not found"})";
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["blob_id"] = row.blob_id;
  out["sha256_hex"] = row.sha256_hex;
  out["size_bytes"] = (Json::Int64)row.size_bytes;
  out["mime"] = row.mime.empty() ? Json::Value(Json::nullValue) : Json::Value(row.mime);
  out["created_utc_ms"] = (Json::Int64)row.created_utc_ms;
  out["last_access_utc_ms"] = (Json::Int64)row.last_access_utc_ms;
  out["ref_count"] = (Json::Int64)row.ref_count;
  out["tier"] = row.tier;
  out["location"] = row.location;
  out["etag"] = row.etag.empty() ? Json::Value(Json::nullValue) : Json::Value(row.etag);
  out["storage_class"] = row.storage_class.empty() ? Json::Value(Json::nullValue) : Json::Value(row.storage_class);
  resp->body = json_stringify(out);

  (void)db_or_null->update_blob_manifest_access(blob_id, now_utc_ms(), nullptr);
}

void handle_blob_retain_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid JSON body"})";
    return;
  }
  const std::string blob_id = body.isMember("blob_id") && body["blob_id"].isString() ? body["blob_id"].asString() : "";
  if (!blob_id_is_safe(blob_id)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid blob_id"})";
    return;
  }
  int64_t delta = 0;
  if (body.isMember("delta") && (body["delta"].isInt64() || body["delta"].isInt())) {
    delta = body["delta"].asInt64();
  }
  if (delta == 0) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing delta"})";
    return;
  }
  int64_t ref_count = 0;
  std::string err;
  if (!db_or_null->adjust_blob_ref_count(blob_id, delta, &ref_count, &err)) {
    resp->status = err == "blob not found" ? 404 : 500;
    resp->body = std::string("{\"ok\":false,\"error\":\"") + err + "\"}";
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["blob_id"] = blob_id;
  out["ref_count"] = (Json::Int64)ref_count;
  out["delta"] = (Json::Int64)delta;
  resp->body = json_stringify(out);
}

void handle_blob_gc_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }
  if (cfg.state_dir.empty()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"state_dir not configured"})";
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!req.body.empty() && !json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid JSON body"})";
    return;
  }
  int64_t min_age_ms = 0;
  if (body.isMember("min_age_ms") && (body["min_age_ms"].isInt64() || body["min_age_ms"].isUInt64())) {
    min_age_ms = body["min_age_ms"].asInt64();
    if (min_age_ms < 0) min_age_ms = 0;
  }
  size_t max_rows = 1000;
  if (body.isMember("max_rows") && (body["max_rows"].isInt64() || body["max_rows"].isUInt64())) {
    const int64_t v = body["max_rows"].asInt64();
    if (v > 0) max_rows = (size_t)v;
  }
  bool dry_run = false;
  if (body.isMember("dry_run") && body["dry_run"].isBool()) {
    dry_run = body["dry_run"].asBool();
  }

  const int64_t now_ms = now_utc_ms();
  const int64_t threshold = min_age_ms > 0 ? (now_ms - min_age_ms) : now_ms;
  std::vector<AgentDb::BlobManifestRow> candidates;
  std::string err;
  if (!db_or_null->list_blob_gc_candidates(threshold, max_rows, &candidates, &err)) {
    resp->status = 500;
    resp->body = std::string("{\"ok\":false,\"error\":\"") + err + "\"}";
    return;
  }

  Json::Value deleted(Json::arrayValue);
  Json::Value errors(Json::arrayValue);
  const std::filesystem::path root(cfg.state_dir);

  for (const auto& row : candidates) {
    Json::Value item(Json::objectValue);
    item["blob_id"] = row.blob_id;
    item["location"] = row.location;
    item["tier"] = row.tier;
    if (dry_run) {
      deleted.append(item);
      continue;
    }
    if (row.tier == "local") {
      const std::filesystem::path abs = (root / row.location).lexically_normal();
      if (!path_is_within_root(root, abs)) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = "invalid path";
        errors.append(e);
        continue;
      }
      std::error_code ec;
      if (std::filesystem::exists(abs, ec)) {
        std::filesystem::remove(abs, ec);
        if (ec) {
          Json::Value e(Json::objectValue);
          e["blob_id"] = row.blob_id;
          e["error"] = std::string("remove failed: ") + ec.message();
          errors.append(e);
          continue;
        }
      }
      (void)db_or_null->delete_blob_manifest(row.blob_id, nullptr);
      deleted.append(item);
      continue;
    }
    if (row.tier == "object") {
      if (row.location.empty()) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = "missing object location";
        errors.append(e);
        continue;
      }
      std::string perr;
      if (!blob_object_store_is_configured(cfg, &perr)) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = perr.empty() ? "object store not configured" : perr;
        errors.append(e);
        continue;
      }
      if (!blob_object_store_delete(cfg, row.location, now_ms, &perr)) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = perr.empty() ? "object store delete failed" : perr;
        errors.append(e);
        continue;
      }
      if (!cfg.state_dir.empty()) {
        const std::string cache_rel = blob_rel_path_from_hex(row.sha256_hex);
        if (!cache_rel.empty()) {
          const std::filesystem::path abs = (root / cache_rel).lexically_normal();
          if (path_is_within_root(root, abs)) {
            std::error_code ec;
            if (std::filesystem::exists(abs, ec)) {
              std::filesystem::remove(abs, ec);
            }
          }
        }
      }
      (void)db_or_null->delete_blob_manifest(row.blob_id, nullptr);
      deleted.append(item);
      continue;
    }
    if (row.tier == "archive") {
      if (row.location.empty()) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = "missing object location";
        errors.append(e);
        continue;
      }
      std::string perr;
      if (!blob_object_store_is_configured(cfg, &perr)) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = perr.empty() ? "object store not configured" : perr;
        errors.append(e);
        continue;
      }
      if (!blob_object_store_delete(cfg, row.location, now_ms, &perr)) {
        Json::Value e(Json::objectValue);
        e["blob_id"] = row.blob_id;
        e["error"] = perr.empty() ? "object store delete failed" : perr;
        errors.append(e);
        continue;
      }
      if (!cfg.state_dir.empty()) {
        const std::string cache_rel = blob_rel_path_from_hex(row.sha256_hex);
        if (!cache_rel.empty()) {
          const std::filesystem::path abs = (root / cache_rel).lexically_normal();
          if (path_is_within_root(root, abs)) {
            std::error_code ec;
            if (std::filesystem::exists(abs, ec)) {
              std::filesystem::remove(abs, ec);
            }
          }
        }
      }
      (void)db_or_null->delete_blob_manifest(row.blob_id, nullptr);
      deleted.append(item);
      continue;
    }
    Json::Value e(Json::objectValue);
    e["blob_id"] = row.blob_id;
    e["error"] = "unsupported tier";
    errors.append(e);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["deleted"] = deleted;
  out["deleted_count"] = (Json::Int64)deleted.size();
  if (!errors.empty()) out["errors"] = errors;
  out["dry_run"] = dry_run;
  out["candidates"] = (Json::Int64)candidates.size();
  resp->body = json_stringify(out);
}

void handle_blob_tier_enforce_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  BlobTierPolicy policy;
  policy.local_max_bytes = cfg.blob_tier_local_max_bytes;
  policy.local_max_age_ms = cfg.blob_tier_local_max_age_ms;
  policy.promote_after_ms = cfg.blob_tier_promote_after_ms;
  policy.promote_max_bytes = cfg.blob_tier_promote_max_bytes;
  policy.force_evict_all = (cfg.blob_store_cache_mode == "none");
  policy.max_rows = 5000;

  if (!req.body.empty()) {
    Json::Value body(Json::objectValue);
    std::string jerr;
    if (!json_parse_object(req.body, &body, &jerr)) {
      resp->status = 400;
      resp->body = std::string("{\"ok\":false,\"error\":\"invalid JSON body\"}");
      return;
    }
    if (body.isMember("dry_run") && body["dry_run"].isBool()) {
      policy.dry_run = body["dry_run"].asBool();
    }
    auto parse_i64 = [&](const char* key, int64_t* out) {
      if (!key || !out) return;
      if (!body.isMember(key)) return;
      const Json::Value& v = body[key];
      if (!v.isInt64() && !v.isUInt64()) return;
      int64_t n = v.isInt64() ? v.asInt64() : (int64_t)v.asUInt64();
      if (n < 0) n = 0;
      *out = n;
    };
    parse_i64("local_max_bytes", &policy.local_max_bytes);
    parse_i64("local_max_age_ms", &policy.local_max_age_ms);
    parse_i64("promote_after_ms", &policy.promote_after_ms);
    parse_i64("promote_max_bytes", &policy.promote_max_bytes);
    if (body.isMember("max_rows") && (body["max_rows"].isInt() || body["max_rows"].isUInt())) {
      const int64_t n = body["max_rows"].isInt() ? body["max_rows"].asInt() : (int64_t)body["max_rows"].asUInt();
      if (n > 0) policy.max_rows = (size_t)std::min<int64_t>(10000, n);
    }
  }

  BlobTierEnforceStats stats;
  std::string err;
  if (!blob_tier_enforce(cfg, db_or_null, policy, &stats, &err)) {
    resp->status = 500;
    resp->body = std::string("{\"ok\":false,\"error\":\"") + (err.empty() ? "tier enforce failed" : err) + "\"}";
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["generated_utc_ms"] = (Json::Int64)stats.generated_utc_ms;
  out["dry_run"] = policy.dry_run;
  out["local_max_bytes"] = (Json::Int64)policy.local_max_bytes;
  out["local_max_age_ms"] = (Json::Int64)policy.local_max_age_ms;
  out["promote_after_ms"] = (Json::Int64)policy.promote_after_ms;
  out["promote_max_bytes"] = (Json::Int64)policy.promote_max_bytes;
  out["promoted_count"] = (Json::Int64)stats.promoted_count;
  out["promoted_bytes"] = (Json::Int64)stats.promoted_bytes;
  out["evicted_count"] = (Json::Int64)stats.evicted_count;
  out["evicted_bytes"] = (Json::Int64)stats.evicted_bytes;
  out["total_local_bytes_before"] = (Json::Int64)stats.total_local_bytes_before;
  out["total_local_bytes_after"] = (Json::Int64)stats.total_local_bytes_after;
  if (!stats.errors.empty()) {
    Json::Value errs(Json::arrayValue);
    for (const auto& e : stats.errors) errs.append(e);
    out["errors"] = errs;
  }
  resp->status = 200;
  resp->body = json_stringify(out);
}

void handle_blob_archive_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }
  if (cfg.blob_store_mode != "object") {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"archive requires blob_store_mode=object"})";
    return;
  }
  std::string cfg_err;
  if (!blob_object_store_is_configured(cfg, &cfg_err)) {
    resp->status = 400;
    resp->body = std::string("{\"ok\":false,\"error\":\"") +
      (cfg_err.empty() ? "object store not configured" : cfg_err) + "\"}";
    return;
  }

  std::vector<std::string> blob_ids;
  Json::Value body(Json::objectValue);
  std::string parse_err;
  if (!parse_blob_ids_body(req, &blob_ids, &body, &parse_err)) {
    resp->status = 400;
    resp->body = std::string("{\"ok\":false,\"error\":\"") + parse_err + "\"}";
    return;
  }

  std::string storage_class;
  if (body.isMember("storage_class") && body["storage_class"].isString()) {
    storage_class = body["storage_class"].asString();
  }

  Json::Value results(Json::arrayValue);
  const int64_t now_ms = now_utc_ms();
  for (const auto& blob_id : blob_ids) {
    Json::Value r(Json::objectValue);
    r["blob_id"] = blob_id;
    AgentDb::BlobManifestRow row;
    std::string err;
    if (!db_or_null->get_blob_manifest(blob_id, &row, &err)) {
      r["ok"] = false;
      r["error"] = err.empty() ? "blob not found" : err;
      results.append(r);
      continue;
    }
    if (row.tier == "local") {
      r["ok"] = false;
      r["error"] = "blob is local; promote to object before archive";
      results.append(r);
      continue;
    }
    std::string next_storage = storage_class.empty() ? row.storage_class : storage_class;
    std::string uerr;
    (void)db_or_null->update_blob_manifest_location(
      row.blob_id,
      row.mime,
      "archive",
      row.location,
      row.etag,
      next_storage,
      now_ms,
      &uerr);
    if (!uerr.empty()) {
      r["ok"] = false;
      r["error"] = uerr;
      results.append(r);
      continue;
    }
    r["ok"] = true;
    r["tier"] = "archive";
    r["storage_class"] = next_storage.empty() ? Json::Value(Json::nullValue) : Json::Value(next_storage);
    results.append(r);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["count"] = (Json::UInt64)results.size();
  out["results"] = results;
  resp->body = json_stringify(out);
}

void handle_blob_restore_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }
  if (cfg.blob_store_mode != "object") {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"restore requires blob_store_mode=object"})";
    return;
  }
  std::string cfg_err;
  if (!blob_object_store_is_configured(cfg, &cfg_err)) {
    resp->status = 400;
    resp->body = std::string("{\"ok\":false,\"error\":\"") +
      (cfg_err.empty() ? "object store not configured" : cfg_err) + "\"}";
    return;
  }

  std::vector<std::string> blob_ids;
  Json::Value body(Json::objectValue);
  std::string parse_err;
  if (!parse_blob_ids_body(req, &blob_ids, &body, &parse_err)) {
    resp->status = 400;
    resp->body = std::string("{\"ok\":false,\"error\":\"") + parse_err + "\"}";
    return;
  }

  bool clear_storage_class = false;
  if (body.isMember("clear_storage_class") && body["clear_storage_class"].isBool()) {
    clear_storage_class = body["clear_storage_class"].asBool();
  }
  std::string storage_class;
  if (body.isMember("storage_class") && body["storage_class"].isString()) {
    storage_class = body["storage_class"].asString();
  }

  Json::Value results(Json::arrayValue);
  const int64_t now_ms = now_utc_ms();
  for (const auto& blob_id : blob_ids) {
    Json::Value r(Json::objectValue);
    r["blob_id"] = blob_id;
    AgentDb::BlobManifestRow row;
    std::string err;
    if (!db_or_null->get_blob_manifest(blob_id, &row, &err)) {
      r["ok"] = false;
      r["error"] = err.empty() ? "blob not found" : err;
      results.append(r);
      continue;
    }
    if (row.tier == "local") {
      r["ok"] = false;
      r["error"] = "blob is local; restore not applicable";
      results.append(r);
      continue;
    }
    std::string next_storage = row.storage_class;
    if (clear_storage_class) next_storage.clear();
    if (!storage_class.empty()) next_storage = storage_class;
    std::string uerr;
    (void)db_or_null->update_blob_manifest_location(
      row.blob_id,
      row.mime,
      "object",
      row.location,
      row.etag,
      next_storage,
      now_ms,
      &uerr);
    if (!uerr.empty()) {
      r["ok"] = false;
      r["error"] = uerr;
      results.append(r);
      continue;
    }
    r["ok"] = true;
    r["tier"] = "object";
    r["storage_class"] = next_storage.empty() ? Json::Value(Json::nullValue) : Json::Value(next_storage);
    results.append(r);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["count"] = (Json::UInt64)results.size();
  out["results"] = results;
  resp->body = json_stringify(out);
}

}  // namespace agentd
