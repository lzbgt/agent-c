#include "session_endpoints.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "session_paths.h"
#include "work_ledger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

namespace agentd {
namespace {
constexpr int kMaxWorkWatches = 32;
std::atomic<int> active_work_watches{0};
struct WatchSlot {
  bool held = false;
  ~WatchSlot() { if (held) active_work_watches.fetch_sub(1); }
  bool acquire() {
    if (active_work_watches.fetch_add(1) >= kMaxWorkWatches) {
      active_work_watches.fetch_sub(1); return false;
    }
    held = true; return true;
  }
};
bool unsigned_decimal(const std::string& text, uint64_t maximum, uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;
  uint64_t n = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<uint64_t>(ch - '0');
    if (digit > maximum || n > (maximum - digit) / 10) return false;
    n = n * 10 + digit;
  }
  *value = n; return true;
}
bool read_session_work(const std::filesystem::path& session, const std::filesystem::path& path, Json::Value* result) {
  // Rechecked after every wake. The configured root is trusted; session/ledger
  // and sidecar symlinks are not. A watch never grants filesystem authority.
  for (const auto& item : {session, session / "work", path,
                           std::filesystem::path(path.string() + "-wal"),
                           std::filesystem::path(path.string() + "-shm"),
                           std::filesystem::path(path.string() + "-journal")}) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(item, ec);
    const bool parent = item == session || item == session / "work";
    if ((std::filesystem::exists(status) && (parent ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status))) ||
        std::filesystem::is_symlink(status) ||
        (ec && ec != std::errc::no_such_file_or_directory)) return false;
  }
  Json::Value request(Json::objectValue);
  request["op"] = "read";
  std::string error;
  return work_ledger_apply(path, request, result, &error);
}
}
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
  const auto after_id = query_get(req.query, "after_ledger_id");
  const auto after_revision = query_get(req.query, "after_revision");
  const auto wait = query_get(req.query, "wait_ms");
  const bool watching = after_id || after_revision || wait;
  uint64_t revision = 0, wait_ms = 0;
  if (watching && (!after_id || !session_id_is_safe(*after_id) || !after_revision ||
      !unsigned_decimal(*after_revision, 9007199254740991ULL, &revision) || !wait ||
      !unsigned_decimal(*wait, 5000, &wait_ms))) {
    fail(400, "watch requires after_ledger_id, after_revision and wait_ms (0..5000)"); return;
  }
  WatchSlot slot;
  if (watching && !slot.acquire()) {
    resp->headers["Retry-After"] = "1";
    fail(429, "session work watch capacity reached; ordinary reads remain available"); return;
  }
  const auto session = session_root_path(cfg.sessions_root_dir, *sid);
  const auto path = session / "work" / "work.sqlite";
  const auto watch = watching ? work_ledger_subscribe(path) : nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    // Subscribe/capture before reading so a commit between read and wait cannot
    // be lost. The condition variable carries no work-state or dispatch rights.
    const uint64_t generation = watch ? watch->generation() : 0;
    Json::Value result;
    if (!read_session_work(session, path, &result)) {
      fail(409, "session work ledger unavailable, invalid or unsafe"); return;
    }
    result["session_id"] = *sid;
    if (!watching) { resp->body = json_stringify(result); return; }
    const auto& state = result["state"];
    if (state.get("ledger_id", "").asString() != *after_id || state["revision"].asInt64() != static_cast<int64_t>(revision)) {
      result["work_watch"]["schema"] = "agentd.work-watch.v1";
      result["work_watch"]["changed"] = true;
      resp->body = json_stringify(result); return;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) { resp->status = 204; resp->body.clear(); return; }
    // External CLI/other-process commits do not share this process-local signal.
    // Bounded fallback reads also discover replacements and deletions.
    watch->wait_for(generation, std::min(remaining, std::chrono::milliseconds(250)));
  }
}
}  // namespace agentd
