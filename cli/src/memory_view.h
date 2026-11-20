#pragma once
#include "memory_store.h"
#include <json/json.h>
#include <map>
#include <vector>

namespace host_tools_internal {
enum class MemoryViewMode { Current, Historical, Evidence };
bool memory_view_mode(const std::string &value, MemoryViewMode *mode);
std::string memory_view_name(MemoryViewMode mode);
std::string memory_view_now_utc();
bool memory_record_current(const Json::Value &record, const std::string &now);
std::string memory_pointer_token(const std::string &key);
struct MemoryPassage {
  std::string path, source_id, revision, text, pointer, key, kind, status, evaluated_utc;
  size_t begin = 0, end = 0; // exact raw byte span [begin,end), unless pointer is set
  int line = 1, end_line = 1;
  bool historical = false;
  Json::Value record;
};
struct MemoryViewSnapshot {
  std::filesystem::path root;
  MemoryViewMode mode = MemoryViewMode::Current;
  std::string as_of_utc;
  std::map<std::string, MemoryDocument> sources;
  std::vector<MemoryPassage> passages;
};
// Caps fail explicitly: 128 selected files, 64 MiB captured bytes, 20,000 passages.
// All paths are lexical children of the configured root; links fail closed.
bool memory_view_capture(const std::filesystem::path &root, const std::vector<std::string> &absolute_files,
                         MemoryViewMode mode, MemoryViewSnapshot *snapshot, std::string *error);
bool memory_view_project(const std::string &path, const MemoryDocument &document, MemoryViewMode mode,
                         const std::string &as_of_utc, std::vector<MemoryPassage> *out, std::string *error);
bool memory_view_validate(const MemoryViewSnapshot &snapshot, std::string *error);
Json::Value memory_passage_evidence(const MemoryPassage &passage);
// Exact original retrieval, never a latest-path substitution. Returns the addressed
// raw bytes or structured JSON value, verified against source/revision/span identity.
bool memory_evidence_read(const std::filesystem::path &root, const Json::Value &reference, std::string *text,
                          Json::Value *value, std::string *error);
std::string memory_view_clip(const std::string &text, size_t bytes);
} // namespace host_tools_internal
