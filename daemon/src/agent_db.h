#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace agentd {

// Thin SQLite-backed troubleshooting store for agentd.
//
// This is intentionally a *mirror* of daemon activity (sessions/runs/events/tool_records),
// not the canonical persistence format (which remains `.sess` + `.events.jsonl`).
class AgentDb {
 public:
  AgentDb() = default;
  ~AgentDb();

  AgentDb(const AgentDb&) = delete;
  AgentDb& operator=(const AgentDb&) = delete;

  // Opens (and creates) the DB at the given path, runs migrations, and enables WAL mode.
  bool open(const std::string& path, std::string* out_error);
  void close();

  bool is_open() const { return db_ != nullptr; }
  std::string path() const { return path_; }

  // Session mirror.
  bool upsert_session(const std::string& session_id, int64_t now_unix_ms, std::string* out_error);
  bool replace_session_messages(
    const std::string& session_id,
    const std::vector<std::pair<std::string, std::string>>& role_and_content,
    int64_t now_unix_ms,
    std::string* out_error
  );
  bool delete_session(const std::string& session_id, std::string* out_error);

  // Run mirror.
  struct RunRow {
    std::string session_id;
    std::string job_id; // optional
    int64_t ts_unix_ms = 0;
    std::string prompt;
    std::string tools;
    std::string model;
    std::string base_url;
    bool stream_assistant = false;
    bool ok = false;
    std::string error;
    long http_status = 0;
    std::string http_body;
  };

  // Inserts a run and returns the new run_id.
  bool insert_run(const RunRow& row, int64_t* out_run_id, std::string* out_error);
  bool insert_event(int64_t run_id, int64_t ts_unix_ms, const std::string& type, const std::string& data_json, std::string* out_error);

  struct ToolRecordRow {
    int64_t run_id = 0;
    std::string tool_name;
    std::string tool_call_id;
    std::string arguments_json;
    std::string result_text;
    std::string result_for_prompt_text;
    bool result_truncated_for_prompt = false;
  };
  bool insert_tool_record(const ToolRecordRow& row, std::string* out_error);

  struct ArtifactRow {
    int64_t run_id = 0;
    int64_t ts_unix_ms = 0;
    std::string session_id;
    std::string tool_call_id;
    std::string path;
    std::string kind;
    std::string mime;
    std::string title;
    bool autoplay = false;
    int repeat = 1;
    std::string artifact_json; // JSON object string (required; stable fallback for future schema changes)
  };
  bool insert_artifact(const ArtifactRow& row, std::string* out_error);

 private:
  bool ensure_schema_locked(std::string* out_error);
  bool exec_locked(const std::string& sql, std::string* out_error);
  bool upsert_session_locked(const std::string& session_id, int64_t now_unix_ms, std::string* out_error);

  sqlite3* db_ = nullptr;
  std::string path_;
  mutable std::mutex mu_;
};

}  // namespace agentd
