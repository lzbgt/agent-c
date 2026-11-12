#include "session_endpoints.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "session_paths.h"
#include "work_ledger.h"
#include <filesystem>

namespace agentd {
void handle_session_work_endpoint(const DaemonConfig& cfg, const CorsConfig& cors_cfg,
                                  const HttpRequest& req, HttpResponse* resp) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->headers["Cache-Control"] = "no-store";
  if (!daemon_require_auth(cfg, req, resp)) return;
  auto fail = [&](int status, const char* message) {
    resp->status = status;
    resp->body = json_error_body(message);
  };
  const auto sid = query_get(req.query, "session_id");
  if (!sid || !session_id_is_safe(*sid)) { fail(400, "invalid session_id"); return; }
  if (cfg.sessions_root_dir.empty()) { fail(503, "sessions_root_dir not configured"); return; }
  const auto session = session_root_path(cfg.sessions_root_dir, *sid);
  const auto path = session / "work" / "work.sqlite";
  // The configured root is trusted; session/ledger and sidecar symlinks are not.
  for (const auto& item : {session, session / "work", path,
                           std::filesystem::path(path.string() + "-wal"),
                           std::filesystem::path(path.string() + "-shm"),
                           std::filesystem::path(path.string() + "-journal")}) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(item, ec);
    const bool parent = item == session || item == session / "work";
    if ((std::filesystem::exists(status) && (parent ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status))) ||
        std::filesystem::is_symlink(status) ||
        (ec && ec != std::errc::no_such_file_or_directory)) {
      fail(409, "unsafe or inaccessible session work path"); return;
    }
  }
  Json::Value request(Json::objectValue), result;
  request["op"] = "read";
  std::string error;
  if (!work_ledger_apply(path, request, &result, &error)) {
    fail(409, "session work ledger unavailable or invalid"); return;
  }
  result["session_id"] = *sid;
  resp->body = json_stringify(result);
}
}  // namespace agentd
