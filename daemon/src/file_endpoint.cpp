#include "file_endpoint.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "session_id_util.h"
#include "session_paths.h"
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
  if (!path_q || path_q->empty()) {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"missing path"})";
    return;
  }

  const std::filesystem::path user_path(*path_q);
  std::filesystem::path effective_root;
  {
    const auto session_q = query_get(req.query, "session_id");
    const std::string session_id = session_q && !session_q->empty() ? *session_q : "";

    if (!session_id.empty()) {
      if (!session_id_is_safe(session_id)) {
        resp->status = 400;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"invalid session_id"})";
        return;
      }
      // Session-scoped file serving:
      // - Artifacts are registered relative to the per-session folder (session-relative artifact paths).
      // - This avoids global artifacts and enables multi-session isolation in a single agentd instance.
      //
      // Layout (rolling / no backwards-compat burden):
      //   <sessions_root_dir>/session_<session_id>/{out,work}/...
      //
      // NOTE: session_id is still sanitized to prevent path traversal.
      if (cfg.sessions_root_dir.empty()) {
        resp->status = 500;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"sessions_root_dir not configured"})";
        return;
      }
      effective_root = session_root_path(cfg.sessions_root_dir, session_id);
      if (effective_root.empty()) {
        resp->status = 400;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"invalid session_id"})";
        return;
      }
    } else {
      std::error_code ec;
      effective_root = std::filesystem::current_path(ec);
      if (ec) {
        resp->status = 500;
        resp->headers["Content-Type"] = "application/json; charset=utf-8";
        resp->body = R"({"ok":false,"error":"failed to resolve daemon working directory"})";
        return;
      }
    }
  }

  std::filesystem::path resolved = user_path.is_absolute() ? user_path : (effective_root / user_path);
  resolved = resolved.lexically_normal();

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
