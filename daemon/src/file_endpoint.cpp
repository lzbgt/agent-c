#include "file_endpoint.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "sandbox_policy.h"
#include "string_util.h"

#include <filesystem>
#include <fstream>

namespace agentd {

void handle_file_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto path_q = query_get(req.query, "path");
  const auto yolo_q = query_get(req.query, "yolo");
  const bool requested_yolo_set = yolo_q.has_value();
  const bool requested_yolo = requested_yolo_set ? string_to_bool(*yolo_q) : cfg.yolo_default;
  const bool yolo = sandbox_tighten_yolo(cfg.yolo_default, requested_yolo, requested_yolo_set);
  if (!path_q || path_q->empty()) {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"missing path"})";
    return;
  }

  const std::filesystem::path user_path(*path_q);
  std::string effective_root_str;
  {
    std::string root_err;
    // Resolve an effective root for relative paths. In YOLO mode this does NOT restrict access
    // (absolute paths are still allowed), but it provides a stable anchor for relative file fetches.
    if (!sandbox_resolve_tools_root(cfg.host_scope_root, yolo, cfg.tools_root, "", false, &effective_root_str, &root_err)) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"invalid daemon tools_root/host_scope_root"})";
      return;
    }
  }
  const std::filesystem::path effective_root = std::filesystem::path(effective_root_str);

  std::filesystem::path resolved;
  std::filesystem::path canon_root;
  std::filesystem::path canon_file;
  if (yolo) {
    resolved = user_path.is_absolute() ? user_path : (effective_root / user_path);
  } else {
    if (user_path.is_absolute()) {
      resp->status = 403;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = "{\"ok\":false,\"error\":\"absolute paths disabled (daemon yolo disabled)\"}";
      return;
    }
    resolved = (effective_root / user_path);
  }
  resolved = resolved.lexically_normal();

  // Containment check when not yolo.
  if (!yolo) {
    std::error_code ec;
    canon_root = std::filesystem::weakly_canonical(effective_root, ec);
    if (ec) {
      resp->status = 500;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"failed to canonicalize host scope root"})";
      return;
    }
    ec.clear();
    canon_file = std::filesystem::weakly_canonical(resolved, ec);
    if (ec) {
      resp->status = 404;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"file not found"})";
      return;
    }
    // Component-wise prefix check (safer than string prefix).
    auto it_r = canon_root.begin();
    auto it_p = canon_file.begin();
    bool within = true;
    for (; it_r != canon_root.end(); ++it_r, ++it_p) {
      if (it_p == canon_file.end() || *it_r != *it_p) {
        within = false;
        break;
      }
    }
    if (!within) {
      resp->status = 403;
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = R"({"ok":false,"error":"path escapes host scope"})";
      return;
    }

    // Use the canonical path for the actual read to avoid symlink TOCTOU between
    // containment check and file open/read.
    resolved = canon_file;
  }

  std::error_code ec;
  if (!std::filesystem::exists(resolved, ec) || !std::filesystem::is_regular_file(resolved, ec)) {
    resp->status = 404;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"file not found"})";
    return;
  }

  const uintmax_t max_bytes = 10ULL * 1024ULL * 1024ULL;
  const uintmax_t sz = std::filesystem::file_size(resolved, ec);
  if (ec || sz > max_bytes) {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"file too large"})";
    return;
  }

  std::ifstream in(resolved, std::ios::binary);
  if (!in.is_open()) {
    resp->status = 500;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"failed to open file"})";
    return;
  }
  std::string bytes;
  bytes.resize((size_t)sz);
  in.read(bytes.data(), (std::streamsize)bytes.size());
  if (!in) {
    resp->status = 500;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"failed to read file"})";
    return;
  }

  resp->status = 200;
  resp->headers["Content-Type"] = content_type_from_path(resolved);
  resp->body = std::move(bytes);
}

}  // namespace agentd
