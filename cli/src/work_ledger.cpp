#include "work_ledger.h"

#include <functional>
#include <memory>
#include <limits>
#include <sstream>
#include <random>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace {
Json::Value empty_state() {
  Json::Value state(Json::objectValue);
  state["schema"] = "agentd.work.v1";
  state["revision"] = Json::Int64(0);
  state["requirements"] = Json::Value(Json::objectValue);
  state["blockers"] = Json::Value(Json::objectValue);
  return state;
}
bool text(const Json::Value& value, size_t max = 4096) {
  return value.isString() && !value.asString().empty() && value.asString().size() <= max;
}
bool id(const Json::Value& value) {
  if (!text(value, 128)) return false;
  for (char c : value.asString()) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  return true;
}
bool known_fields(const Json::Value& req, const std::unordered_set<std::string>& fields) {
  for (const auto& key : req.getMemberNames()) if (!fields.count(key)) return false;
  return true;
}
std::string encode(const Json::Value& value) {
  Json::StreamWriterBuilder w; w["indentation"] = ""; return Json::writeString(w, value);
}
bool project(Json::Value state, Json::Value* out, std::string* error) {
  if (state.isMember("ledger_id") && !id(state["ledger_id"])) { *error = "invalid stored ledger identity"; return false; }
  auto& requirements = state["requirements"];
  for (const auto& key : requirements.getMemberNames()) {
    const auto& req = requirements[key];
    if (!req.isObject() || !text(req["title"], 256) || !req["status"].isString() ||
        (req["status"] != "pending" && req["status"] != "running" && req["status"] != "done") ||
        !req["depends_on"].isArray()) { *error = "invalid stored requirement"; return false; }
    for (const auto& dep : req["depends_on"]) if (!id(dep)) { *error = "invalid stored dependency"; return false; }
    if (req["status"] == "done" && !text(req["evidence"])) { *error = "completed requirement lacks evidence"; return false; }
  }
  for (const auto& key : state["blockers"].getMemberNames()) {
    const auto& blocker = state["blockers"][key];
    if (!blocker.isObject() || (blocker["status"] != "open" && blocker["status"] != "resolved") || !blocker["requirement_ids"].isArray()) { *error = "invalid stored blocker"; return false; }
    for (const auto& r : blocker["requirement_ids"]) if (!id(r) || !requirements.isMember(r.asString())) { *error = "invalid stored blocker requirement"; return false; }
  }
  std::unordered_map<std::string, int> visit;
  std::function<bool(const std::string&)> dfs = [&](const std::string& key) {
    if (!requirements.isMember(key) || visit[key] == 1) return false;
    if (visit[key] == 2) return true;
    visit[key] = 1;
    for (const auto& dep : requirements[key]["depends_on"]) if (!dfs(dep.asString())) return false;
    visit[key] = 2;
    return true;
  };
  for (const auto& key : requirements.getMemberNames()) {
    if (!dfs(key)) { *error = "requirement dependencies must reference existing items and form a DAG"; return false; }
  }
  Json::Value runnable(Json::arrayValue), waiting(Json::arrayValue), open(Json::arrayValue);
  std::unordered_set<std::string> blocked;
  for (const auto& key : state["blockers"].getMemberNames()) {
    const auto& blocker = state["blockers"][key];
    if (blocker["status"] != "open") continue;
    open.append(key);
    for (const auto& req : blocker["requirement_ids"]) blocked.insert(req.asString());
  }
  bool all_done = !requirements.empty();
  for (const auto& key : requirements.getMemberNames()) {
    auto& req = requirements[key];
    bool ready = !blocked.count(key);
    for (const auto& dep : req["depends_on"]) if (requirements[dep.asString()]["status"] != "done") ready = false;
    if (req["status"] == "done") {
      if (!ready) { *error = "a completed requirement cannot depend on unfinished work or an open blocker"; return false; }
      continue;
    }
    all_done = false;
    (ready ? runnable : waiting).append(key);
  }
  (*out)["ok"] = true;
  (*out)["state"] = std::move(state);
  (*out)["runnable"] = runnable;
  (*out)["waiting"] = waiting;
  (*out)["open_blockers"] = open;
  (*out)["all_requirements_done"] = all_done;
  // Blockers may be advisory; they do not themselves stop a runnable requirement.
  (*out)["can_continue"] = !runnable.empty();
  return true;
}
}

bool work_ledger_apply(const std::filesystem::path& path, const Json::Value& req,
                       Json::Value* out, std::string* error) {
  std::string local_error;
  if (!error) error = &local_error;
  error->clear();
  if (!out || !req.isObject()) { *error = "expected work request object"; return false; }
  *out = Json::Value(Json::objectValue);
#if !defined(AGENT_HAVE_SQLITE3)
  (void)path;
  *error = "work ledger requires SQLite support";
  return false;
#else
  if (encode(req).size() > 64 * 1024) { *error = "work request exceeds 64 KiB"; return false; }
  if (req.isMember("op") && !req["op"].isString()) { *error = "op must be a string"; return false; }
  const auto op = req.get("op", "read").asString();
  const bool read = op == "read";
  const std::unordered_set<std::string> common = {"op", "expected_revision"};
  auto fields = common;
  if (op == "requirement") fields.insert({"id", "title", "status", "depends_on", "evidence"});
  else if (op == "raise_blocker") fields.insert({"id", "reason", "required_action", "attempts", "requirement_ids"});
  else if (op == "resolve_blocker") fields.insert({"id", "evidence"});
  else if (!read) { *error = "unsupported work operation"; return false; }
  if (!known_fields(req, fields)) { *error = "unknown work request field"; return false; }
  if (!read && !id(req["id"])) { *error = "mutation requires an id-safe id"; return false; }
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec) { *error = "cannot inspect work ledger path"; return false; }
  if (req.isMember("expected_revision") && (!req["expected_revision"].isInt64() || req["expected_revision"].asInt64() < 0)) { *error = "expected_revision must be a nonnegative integer"; return false; }
  if (read && !exists) {
    if (req.isMember("expected_revision") && req["expected_revision"].asInt64() != 0) { *error = "work revision conflict"; return false; }
    return project(empty_state(), out, error);
  }
  if (!read && !path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { *error = "cannot create work ledger directory"; return false; }
  }
  sqlite3* raw = nullptr;
  const int flags = read ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  const int rc = sqlite3_open_v2(path.string().c_str(), &raw, flags, nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
  if (rc != SQLITE_OK) { *error = "cannot open work ledger"; return false; }
  sqlite3_busy_timeout(raw, 3000);
  auto sql = [&](const char* s) { return sqlite3_exec(raw, s, nullptr, nullptr, nullptr) == SQLITE_OK; };
  if (!read && !sql("CREATE TABLE IF NOT EXISTS work_state (id INTEGER PRIMARY KEY CHECK(id=1), value TEXT NOT NULL)")) { *error = "cannot initialize work ledger"; return false; }
  if (!sql(read ? "BEGIN" : "BEGIN IMMEDIATE")) { *error = "work ledger is busy"; return false; }
  // Closing the connection rolls back every error path after BEGIN.
  Json::Value state = empty_state();
  sqlite3_stmt* query = nullptr;
  if (sqlite3_prepare_v2(raw, "SELECT value FROM work_state WHERE id=1", -1, &query, nullptr) != SQLITE_OK) { *error = "invalid work ledger schema"; return false; }
  {
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(query, sqlite3_finalize);
    const int step = sqlite3_step(query);
    if (step == SQLITE_ROW) {
      const auto size = sqlite3_column_bytes(query, 0);
      if (size > 1024 * 1024) { *error = "stored work ledger exceeds 1 MiB"; return false; }
      const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
      Json::CharReaderBuilder builder;
      std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
      if (!bytes || !reader->parse(bytes, bytes + size, &state, error) || !state.isObject() || state["schema"] != "agentd.work.v1" || !state["revision"].isInt64() || !state["requirements"].isObject() || !state["blockers"].isObject()) { *error = "invalid stored work ledger"; return false; }
    } else if (step != SQLITE_DONE) { *error = "cannot read work ledger"; return false; }
  }
  if (req.isMember("expected_revision") && (!req["expected_revision"].isInt64() || req["expected_revision"] != state["revision"])) { *error = "work revision conflict; read current state before retrying"; return false; }
  Json::Value prior_projection;
  if (!project(state, &prior_projection, error)) return false;
  if (!read) {
    const auto key = req["id"].asString();
    if (op == "requirement") {
      auto item = state["requirements"].get(key, Json::Value(Json::objectValue));
      for (const auto& field : {"title", "status", "depends_on", "evidence"}) if (req.isMember(field)) item[field] = req[field];
      if (!item.isMember("status")) item["status"] = "pending";
      if (!item.isMember("depends_on")) item["depends_on"] = Json::Value(Json::arrayValue);
      if (!text(item["title"], 256) || !item["status"].isString() || (item["status"] != "pending" && item["status"] != "running" && item["status"] != "done")) { *error = "requirement needs title and pending|running|done status"; return false; }
      if (!item["depends_on"].isArray() || item["depends_on"].size() > 256) { *error = "depends_on must be a bounded array"; return false; }
      for (const auto& dep : item["depends_on"]) if (!id(dep)) { *error = "invalid dependency id"; return false; }
      if (item.isMember("evidence") && !text(item["evidence"])) { *error = "evidence must be a nonempty string up to 4096 bytes"; return false; }
      if (item["status"] == "done" && !text(item["evidence"])) { *error = "done requires verification evidence"; return false; }
      state["requirements"][key] = std::move(item);
    } else if (op == "raise_blocker") {
      if (!text(req["reason"]) || !text(req["required_action"]) || !req["attempts"].isArray() || req["attempts"].empty() || req["attempts"].size() > 16 || !req["requirement_ids"].isArray() || req["requirement_ids"].size() > 256) { *error = "blocker requires reason, required_action, 1..16 recovery attempts and requirement_ids (empty for advisory)"; return false; }
      for (const auto& attempt : req["attempts"]) if (!text(attempt)) { *error = "each recovery attempt must describe observed evidence"; return false; }
      for (const auto& r : req["requirement_ids"]) if (!id(r) || !state["requirements"].isMember(r.asString())) { *error = "blocker references unknown requirement"; return false; }
      auto item = req; item.removeMember("op"); item.removeMember("expected_revision"); item.removeMember("id"); item["status"] = "open";
      state["blockers"][key] = std::move(item);
    } else {
      if (!state["blockers"].isMember(key) || !text(req["evidence"])) { *error = "resolve requires an existing blocker and verification evidence"; return false; }
      state["blockers"][key]["status"] = "resolved";
      state["blockers"][key]["evidence"] = req["evidence"];
    }
    if (state["requirements"].size() > 256 || state["blockers"].size() > 128) { *error = "work ledger item limit exceeded (256 requirements, 128 blockers)"; return false; }
    if (state["revision"].asInt64() == std::numeric_limits<Json::Int64>::max()) { *error = "work revision limit exceeded"; return false; }
    if (!state.isMember("ledger_id")) {
      std::random_device random;
      std::ostringstream identity;
      identity << "work_" << std::hex << std::setfill('0');
      for (int i = 0; i < 4; ++i) identity << std::setw(8) << static_cast<uint32_t>(random());
      state["ledger_id"] = identity.str();
    }
    state["revision"] = state["revision"].asInt64() + 1;
  }
  if (!project(state, out, error)) return false;
  if (!read) {
    const auto value = encode(state);
    if (value.size() > 1024 * 1024) { *error = "work ledger exceeds 1 MiB"; return false; }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "INSERT OR REPLACE INTO work_state(id,value) VALUES(1,?)", -1, &stmt, nullptr) != SQLITE_OK) { *error = "cannot prepare work update"; return false; }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
    sqlite3_bind_text(stmt, 1, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) { *error = "cannot persist work update"; return false; }
  }
  if (!sql("COMMIT")) { *error = "cannot commit work update"; return false; }
  return true;
#endif
}
