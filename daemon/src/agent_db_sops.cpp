#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <memory>

namespace agentd {
#if defined(AGENT_HAVE_SQLITE3)
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
const char* columns = "registry_id,definition_id,version,definition_hash,title,sop_json,created_unix_ms";
void read_row(sqlite3_stmt* s, AgentDb::WorkflowSopRow* out) {
  out->registry_id = sqlite3_column_int64(s, 0);
  out->definition_id = reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
  out->version = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
  out->definition_hash = reinterpret_cast<const char*>(sqlite3_column_text(s, 3));
  out->title = reinterpret_cast<const char*>(sqlite3_column_text(s, 4));
  out->sop_json = reinterpret_cast<const char*>(sqlite3_column_text(s, 5));
  out->created_unix_ms = sqlite3_column_int64(s, 6);
}
}
#endif

bool AgentDb::create_workflow_sop(const WorkflowSopRow& def, WorkflowSopRow* out, bool* created, std::string* error) {
  if (error) error->clear();
  if (created) *created = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)def; (void)out;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out || !created || def.definition_id.empty() || def.definition_id.size() > 128 ||
      def.version.empty() || def.version.size() > 128 || def.definition_hash.empty() ||
      def.definition_hash.size() > 96 || def.title.size() > 256 || def.sop_json.empty() || def.sop_json.size() > 128 * 1024) {
    if (error) *error = "invalid SOP definition or unavailable database";
    return false;
  }
  sqlite3_stmt* raw = nullptr;
  const char* sql = "INSERT INTO workflow_sop_definitions(definition_id,version,definition_hash,title,sop_json,created_unix_ms) VALUES(?,?,?,?,?,?) ON CONFLICT(definition_id,version) DO NOTHING";
  if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  {
    Statement stmt(raw, sqlite3_finalize);
    if (!(agent_db_bind_text(raw, 1, def.definition_id) && agent_db_bind_text(raw, 2, def.version) &&
          agent_db_bind_text(raw, 3, def.definition_hash) && agent_db_bind_text(raw, 4, def.title) &&
          agent_db_bind_text(raw, 5, def.sop_json) && agent_db_bind_i64(raw, 6, agent_db_unix_ms_now()) && agent_db_step_done(raw))) {
      if (error) *error = agent_db_sqlite_err(db_);
      return false;
    }
    *created = sqlite3_changes(db_) == 1;
  }
  const auto select = std::string("SELECT ") + columns + " FROM workflow_sop_definitions WHERE definition_id=? AND version=?";
  if (sqlite3_prepare_v2(db_, select.c_str(), -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw, sqlite3_finalize);
  if (!agent_db_bind_text(raw, 1, def.definition_id) || !agent_db_bind_text(raw, 2, def.version) || sqlite3_step(raw) != SQLITE_ROW) {
    if (error) *error = "failed to read saved SOP version";
    return false;
  }
  read_row(raw, out);
  return true;
#endif
}

bool AgentDb::get_workflow_sop(const std::string& id, const std::string& version, WorkflowSopRow* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id; (void)version; (void)out;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out) { if (error) *error = "db not available"; return false; }
  sqlite3_stmt* raw = nullptr;
  const auto sql = std::string("SELECT ") + columns + " FROM workflow_sop_definitions WHERE definition_id=? AND version=?";
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw, sqlite3_finalize);
  if (!agent_db_bind_text(raw, 1, id) || !agent_db_bind_text(raw, 2, version)) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  const int rc = sqlite3_step(raw);
  if (rc == SQLITE_ROW) { read_row(raw, out); return true; }
  if (rc != SQLITE_DONE && error) *error = agent_db_sqlite_err(db_);
  return false;
#endif
}

bool AgentDb::list_workflow_sops(int64_t before_id, size_t limit, std::vector<WorkflowSopRow>* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)before_id; (void)limit; (void)out;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out || before_id < 0 || limit < 1 || limit > 101) { if (error) *error = "invalid SOP page or unavailable database"; return false; }
  out->clear();
  sqlite3_stmt* raw = nullptr;
  const char* sql = "SELECT registry_id,definition_id,version,definition_hash,title,'',created_unix_ms FROM workflow_sop_definitions WHERE (?=0 OR registry_id<?) ORDER BY registry_id DESC LIMIT ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw, sqlite3_finalize);
  if (!(agent_db_bind_i64(raw, 1, before_id) && agent_db_bind_i64(raw, 2, before_id) && agent_db_bind_i64(raw, 3, (int64_t)limit))) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  int rc;
  while ((rc = sqlite3_step(raw)) == SQLITE_ROW) { WorkflowSopRow row; read_row(raw, &row); out->push_back(std::move(row)); }
  if (rc != SQLITE_DONE) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  return true;
#endif
}
}  // namespace agentd
