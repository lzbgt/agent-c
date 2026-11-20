#if defined(AGENT_HAVE_JSONCPP)
#include "memory_view.h"
#include "memory_structured.h"
#include "memory_promotion.h"
#include "agent_sha256.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <sstream>

namespace host_tools_internal {
namespace {
const std::string kBegin = "<!-- AGENT_MEMORY_V1_BEGIN -->", kEnd = "<!-- AGENT_MEMORY_V1_END -->";
std::string lower(std::string text) {
  for (auto &c : text)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return text;
}
std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string value_text(const Json::Value &value) {
  if (value.isString())
    return value.asString();
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}
bool reject(std::string *error, const std::string &text) {
  if (error)
    *error = text;
  return false;
}
struct Line {
  size_t begin, end;
  int number;
  std::string text;
};
std::vector<Line> lines(const std::string &text) {
  std::vector<Line> out;
  size_t begin = 0;
  while (begin < text.size()) {
    auto end = text.find('\n', begin);
    end = end == std::string::npos ? text.size() : end + 1;
    out.push_back({begin, end, int(out.size()) + 1, trim(text.substr(begin, end - begin))});
    begin = end;
  }
  return out;
}
int heading_level(const std::string &text) {
  size_t n = 0;
  while (n < text.size() && text[n] == '#')
    ++n;
  return n && n <= 6 && n < text.size() && text[n] == ' ' ? int(n) : 0;
}
bool historical_heading(std::string text) {
  text = lower(trim(text));
  return text == "deprecated" || text == "history" || text == "historical" || text == "superseded" ||
         text == "inactive" || text == "obsolete" || text == "deleted" || text == "archived" ||
         text == "已废弃" || text == "历史" || text == "已删除";
}
bool inactive_marker(const std::string &text) {
  auto pos = text.find("@mem ");
  if (pos == std::string::npos)
    return false;
  std::istringstream stream(text.substr(pos + 5));
  std::string status;
  stream >> status;
  status = lower(status);
  return status == "deprecated" || status == "inactive" || status == "obsolete" || status == "deleted";
}
MemoryPassage base(const std::string &path, const MemoryDocument &doc) {
  MemoryPassage out;
  out.path = path;
  out.source_id = doc.source_id;
  out.revision = doc.revision;
  return out;
}
void observation(MemoryPassage *passage) {
  MemoryMarker marker;
  std::string reason;
  std::istringstream marker_lines(passage->text);
  std::string first_line;
  if (std::getline(marker_lines, first_line) && memory_marker_fields(first_line, &marker, &reason)) {
    passage->key = marker.key; passage->kind = marker.kind;
    passage->record["value"] = marker.value; passage->record["status"] = marker.status;
    if (marker.status != "active") { passage->historical = true; passage->status = marker.status; }
  } else if (!reason.empty()) {
    passage->historical = true; passage->eligibility_reason = "invalid_marker";
  }
  const auto content = trim(passage->text);
  if (content.rfind("- @obs ", 0) != 0 && content.rfind("* @obs ", 0) != 0)
    return;
  auto pos = passage->text.find("@obs ");
  passage->kind = "observation";
  std::istringstream stream(passage->text.substr(pos + 5));
  std::string line;
  if (std::getline(stream, line))
    passage->record["value"] = trim(line);
  while (std::getline(stream, line)) {
    const auto clean = trim(line);
    const auto colon = clean.find(':');
    if (clean.rfind("- ", 0) != 0 || colon == std::string::npos)
      continue;
    const auto key = trim(clean.substr(2, colon - 2));
    if (key == "ts_utc")
      passage->record["observed_utc"] = trim(clean.substr(colon + 1));
    if (key == "importance") {
      try {
        passage->record["importance"] = std::clamp(std::stoi(trim(clean.substr(colon + 1))), 0, 5);
      } catch (...) {
      }
    }
  }
}
void raw_passages(const std::string &path, const MemoryDocument &doc, MemoryViewMode mode, size_t begin,
                  size_t end, const std::vector<Line> &all, std::vector<MemoryPassage> *out) {
  int excluded_level = 0;
  bool marker_excluded = false, fenced = false;
  char fence = 0;
  MemoryPassage current = base(path, doc);
  auto flush = [&] {
    if (!trim(current.text).empty()) {
      if (current.kind != "code")
        observation(&current);
      if (mode != MemoryViewMode::Current || !current.historical) out->push_back(std::move(current));
    }
    current = base(path, doc);
  };
  for (const auto &line : all) {
    if (line.begin < begin || line.end > end)
      continue;
    const auto raw = doc.text.substr(line.begin, line.end - line.begin);
    const bool delimiter = line.text.rfind("```", 0) == 0 || line.text.rfind("~~~", 0) == 0;
    const bool code = fenced || delimiter || raw.rfind("    ", 0) == 0;
    if (delimiter && (!fenced || line.text[0] == fence)) {
      fenced = !fenced;
      fence = line.text[0];
    }
    const auto level = code ? 0 : heading_level(line.text);
    if (level && excluded_level && level <= excluded_level)
      excluded_level = 0;
    if (level && historical_heading(line.text.substr(size_t(level))))
      excluded_level = level;
    MemoryMarker marker;
    std::string marker_error;
    const bool marker_line = !code && (memory_marker_fields(line.text, &marker, &marker_error) || !marker_error.empty());
    const bool continuation = !line.text.empty() && doc.text[line.begin] == ' ';
    if (!continuation && !code)
      marker_excluded = inactive_marker(line.text) || (marker_line && (marker.status != "active" || !marker_error.empty()));
    const bool historical = excluded_level || marker_excluded;
    if (mode == MemoryViewMode::Current && historical) {
      flush();
      continue;
    }
    const bool boundary = line.text.empty() || level ||
                          (!code && (line.text.find("@obs ") != std::string::npos ||
                                     marker_line));
    if (boundary ||
        (!current.text.empty() && (current.historical != historical || (current.kind == "code") != code ||
                                   current.text.size() + line.end - line.begin > 1600)))
      flush();
    if (current.text.empty()) {
      current.begin = line.begin;
      current.line = line.number;
      current.historical = historical;
      current.status = historical ? "historical" : "active";
      current.kind = code ? "code" : "";
    }
    current.end = line.end;
    current.end_line = line.number;
    current.text += doc.text.substr(line.begin, line.end - line.begin);
  }
  flush();
}
bool pointer_get(const Json::Value &doc, const std::string &pointer, Json::Value *value) {
  if (pointer.empty() || pointer[0] != '/')
    return false;
  Json::Value current = doc;
  size_t begin = 1;
  do {
    auto end = pointer.find('/', begin);
    const auto raw = pointer.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    std::string token;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != '~')
        token += raw[i];
      else if (i + 1 < raw.size() && (raw[i + 1] == '0' || raw[i + 1] == '1'))
        token += raw[++i] == '0' ? '~' : '/';
      else
        return false;
    }
    if (current.isObject() && current.isMember(token)) {
      Json::Value next = current[token];
      current = std::move(next);
    } else if (current.isArray() && !token.empty() && token.size() <= 8 &&
               (token.size() == 1 || token[0] != '0') &&
               token.find_first_not_of("0123456789") == std::string::npos) {
      const auto index = std::stoul(token);
      if (index >= current.size())
        return false;
      Json::Value next = current[Json::ArrayIndex(index)];
      current = std::move(next);
    } else
      return false;
    if (end == std::string::npos)
      break;
    begin = end + 1;
  } while (true);
  *value = std::move(current);
  return true;
}
} // namespace
bool memory_view_mode(const std::string &value, MemoryViewMode *mode) {
  if (value == "current")
    *mode = MemoryViewMode::Current;
  else if (value == "historical")
    *mode = MemoryViewMode::Historical;
  else if (value == "evidence")
    *mode = MemoryViewMode::Evidence;
  else
    return false;
  return true;
}
std::string memory_view_name(MemoryViewMode mode) {
  return mode == MemoryViewMode::Current      ? "current"
         : mode == MemoryViewMode::Historical ? "historical"
                                              : "evidence";
}
std::string memory_view_now_utc() {
  const auto time = std::time(nullptr);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  char text[32]{};
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}
bool memory_record_current(const Json::Value &record, const std::string &now) {
  if (!record.isObject() || !record.isMember("value") || record["value"].isNull() ||
      (record.isMember("status") && !record["status"].isString()) ||
      (record.isMember("kind") && !record["kind"].isString()) ||
      (record.isMember("valid_from") && !record["valid_from"].isString()))
    return false;
  for (const auto *field : {"deleted", "invalidated"})
    if (record.isMember(field) && (!record[field].isBool() || record[field].asBool()))
      return false;
  if (record.isMember("deleted_utc") &&
      (!record["deleted_utc"].isString() || !record["deleted_utc"].asString().empty()))
    return false;
  if (lower(trim(record.get("status", "active").asString())) != "active" ||
      lower(record.get("kind", "fact").asString()) == "deprecated" || record.get("deleted", false) == true ||
      record.get("invalidated", false) == true)
    return false;
  const auto valid = record.get("valid_from", "").asString();
  if (valid.empty())
    return true;
  if (valid.size() != 20 || valid[4] != '-' || valid[7] != '-' || valid[10] != 'T' || valid[13] != ':' ||
      valid[16] != ':' || valid[19] != 'Z')
    return false;
  for (size_t i = 0; i < valid.size(); ++i)
    if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 &&
        !std::isdigit(static_cast<unsigned char>(valid[i])))
      return false;
  const auto year = std::stoi(valid.substr(0, 4)), month = std::stoi(valid.substr(5, 2)),
             day = std::stoi(valid.substr(8, 2));
  const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 1970 || month < 1 || month > 12 || day < 1 ||
      day > days[month] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ||
      std::stoi(valid.substr(11, 2)) > 23 || std::stoi(valid.substr(14, 2)) > 59 ||
      std::stoi(valid.substr(17, 2)) > 59)
    return false;
  return valid <= now;
}
std::string memory_pointer_token(const std::string &key) {
  std::string out;
  for (const auto c : key)
    out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
  return out;
}
std::string memory_view_clip(const std::string &text, size_t bytes) {
  if (text.size() <= bytes)
    return text;
  while (bytes && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80)
    --bytes;
  return text.substr(0, bytes);
}
bool memory_view_project(const std::string &path, const MemoryDocument &doc, MemoryViewMode mode,
                         const std::string &now, std::vector<MemoryPassage> *out, std::string *error) {
  if (!out)
    return false;
  out->clear();
  if (!doc.exists)
    return true;
  if (std::count(doc.text.begin(), doc.text.end(), '\n') > 20000)
    return reject(error, "memory source exceeds 20000-line retrieval budget");
  const auto all = lines(doc.text);
  const auto begin = doc.text.find(kBegin), end = doc.text.find(kEnd);
  if (begin == std::string::npos && end == std::string::npos) {
    raw_passages(path, doc, mode, 0, doc.text.size(), all, out);
    return true;
  }
  Json::Value structured;
  if (!memory_structured_parse_doc(doc.text, &structured, error))
    return false;
  for (const auto &key : structured["items"].getMemberNames()) {
    if (out->size() >= 20000)
      return reject(error, "memory source exceeds passage budget");
    const auto &record = structured["items"][key];
    if (!record.isObject())
      continue;
    auto add = [&](const Json::Value &value, const std::string &pointer, bool historical) {
      if (!value.isObject() ||
          (mode == MemoryViewMode::Current && (historical || !memory_record_current(value, now))))
        return;
      auto passage = base(path, doc);
      passage.key = key;
      passage.kind = value["kind"].isString() ? value["kind"].asString() : "fact";
      passage.status = value["status"].isString() ? value["status"].asString() : "unknown";
      passage.historical = historical || !memory_record_current(value, now);
      passage.pointer = pointer + "/value";
      passage.begin = begin;
      passage.end = end + kEnd.size();
      passage.line = 1 + int(std::count(doc.text.begin(), doc.text.begin() + begin, '\n'));
      passage.end_line = passage.line;
      passage.text = key + " = " + value_text(value["value"]);
      passage.record_revision = memory_record_revision(value);
      passage.record = value;
      passage.record.removeMember("versions");
      out->push_back(std::move(passage));
    };
    const auto pointer = "/items/" + memory_pointer_token(key);
    add(record, pointer, false);
    if (mode != MemoryViewMode::Current && record["versions"].isArray()) {
      for (Json::ArrayIndex i = 0; i < record["versions"].size(); ++i) {
        if (out->size() >= 20000)
          return reject(error, "memory source exceeds passage budget");
        add(record["versions"][i], pointer + "/versions/" + std::to_string(i), true);
      }
    }
  }
  // The human-readable generated sections repeat state. Only the explicitly
  // retained user-notes block adds independent Markdown evidence.
  const std::string notes_begin = "<!-- AGENT_MEMORY_V1_NOTES_BEGIN -->",
                    notes_end = "<!-- AGENT_MEMORY_V1_NOTES_END -->";
  const auto notes = doc.text.find(notes_begin), stop = doc.text.find(notes_end);
  if (notes != std::string::npos && stop != std::string::npos && stop > notes)
    raw_passages(path, doc, mode, notes + notes_begin.size(), stop, all, out);
  return true;
}
bool memory_view_capture(const std::filesystem::path &input, const std::vector<std::string> &files,
                         MemoryViewMode mode, MemoryViewSnapshot *snapshot, std::string *error) {
  if (!snapshot)
    return false;
  *snapshot = {};
  snapshot->mode = mode;
  snapshot->as_of_utc = memory_view_now_utc();
  std::error_code ec;
  snapshot->root = std::filesystem::absolute(input, ec).lexically_normal();
  if (ec || input.empty() || files.size() > 128)
    return reject(error, "memory snapshot path/file budget exceeded");
  size_t bytes = 0;
  for (const auto &file : files) {
    const auto absolute = std::filesystem::absolute(file, ec).lexically_normal();
    if (ec)
      return reject(error, "invalid memory source");
    const auto path = absolute.lexically_relative(snapshot->root).generic_string();
    if (snapshot->sources.count(path))
      continue;
    MemoryDocument doc;
    if (memory_document_read(snapshot->root, path, &doc, error) != MemoryStoreStatus::Ok)
      return false;
    bytes += doc.text.size();
    if (bytes > 64 * 1024 * 1024)
      return reject(error, "memory snapshot exceeds 64 MiB");
    std::vector<MemoryPassage> passages;
    if (!memory_view_project(path, doc, mode, snapshot->as_of_utc, &passages, error))
      return false;
    snapshot->passages.insert(snapshot->passages.end(), std::make_move_iterator(passages.begin()),
                              std::make_move_iterator(passages.end()));
    if (snapshot->passages.size() > 20000)
      return reject(error, "memory snapshot exceeds 20000 passages");
    snapshot->sources.emplace(path, std::move(doc));
  }
  // A root structured key makes its raw promotion marker non-authoritative.
  // Capture this dependency even when the caller does not return that layer.
  if (!snapshot->sources.count("STRUCTURED.md")) {
    MemoryDocument authority;
    if (memory_document_read(snapshot->root, "STRUCTURED.md", &authority, error) != MemoryStoreStatus::Ok)
      return false;
    bytes += authority.text.size();
    if (bytes > 64 * 1024 * 1024)
      return reject(error, "memory snapshot exceeds 64 MiB");
    snapshot->sources.emplace("STRUCTURED.md", std::move(authority));
  }
  Json::Value authority;
  if (!memory_structured_parse_doc(snapshot->sources.at("STRUCTURED.md").text, &authority, error))
    return false;
  auto &passages = snapshot->passages;
  for (auto &passage : passages) {
    passage.evaluated_utc = snapshot->as_of_utc;
    if (passage.pointer.empty() && !passage.key.empty() && authority["items"].isMember(passage.key)) {
      passage.historical = true;
      passage.status = "superseded_marker";
    }
  }
  if (!memory_promotion_validate_dependencies(snapshot, error)) return false;
  if (mode == MemoryViewMode::Current)
    passages.erase(std::remove_if(passages.begin(), passages.end(),
                                  [](const auto &passage) { return passage.historical; }),
                   passages.end());
  return memory_view_validate(*snapshot, error);
}
bool memory_view_validate(const MemoryViewSnapshot &snapshot, std::string *error) {
  for (const auto &source : snapshot.sources) {
    MemoryDocument current;
    if (memory_document_read(snapshot.root, source.first, &current, error) != MemoryStoreStatus::Ok)
      return false;
    if (current.revision != source.second.revision)
      return reject(error, "memory changed during retrieval; discard these results and reread");
  }
  return true;
}
Json::Value memory_passage_evidence(const MemoryPassage &passage) {
  Json::Value ref;
  ref["schema"] = "agent.memory.evidence.v1";
  ref["path"] = passage.path;
  ref["source_id"] = passage.source_id;
  ref["revision"] = passage.revision;
  ref["span"]["kind"] = passage.pointer.empty() ? "bytes" : "json_pointer";
  if (passage.pointer.empty()) {
    ref["span"]["begin"] = Json::UInt64(passage.begin);
    ref["span"]["end"] = Json::UInt64(passage.end);
  } else
    ref["span"]["pointer"] = passage.pointer;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const auto identity = Json::writeString(writer, ref);
  char hash[65]{};
  agent_sha256_hex_of_bytes(identity.data(), identity.size(), hash);
  ref["id"] = std::string("evidence:") + hash;
  return ref;
}
bool memory_evidence_read(const std::filesystem::path &root, const Json::Value &ref, std::string *text,
                          Json::Value *value, std::string *error) {
  if (text)
    text->clear();
  if (value)
    *value = Json::nullValue;
  if (!text || !value || !ref.isObject() || ref.get("schema", "") != "agent.memory.evidence.v1" ||
      !ref["path"].isString() || !ref["revision"].isString() || !ref["source_id"].isString() ||
      !ref["span"].isObject())
    return reject(error, "invalid memory evidence reference");
  const auto path = ref["path"].asString();
  const auto extension = lower(std::filesystem::path(path).extension().string());
  const bool markdown = extension == ".md" || extension == ".markdown";
  const bool artifact =
      path.size() >= 5 && path.substr(path.size() - 5) == ".json" &&
      (path.rfind("checkpoints/structured_", 0) == 0 || path.rfind("recaps/recap_", 0) == 0);
  if (!markdown && !artifact)
    return reject(error, "evidence source is not a memory document or published evidence artifact");
  for (const auto &part : std::filesystem::path(path))
    if (!part.empty() && part.string()[0] == '.')
      return reject(error, "private memory metadata is not an evidence source");
  MemoryDocument doc;
  if (memory_document_read_revision(root, ref["path"].asString(), ref["revision"].asString(), &doc, error) !=
      MemoryStoreStatus::Ok)
    return false;
  if (doc.source_id != ref["source_id"].asString())
    return reject(error, "memory evidence scope/source mismatch");
  auto passage = base(ref["path"].asString(), doc);
  const auto &span = ref["span"];
  if (span.get("kind", "") == "json_pointer" && span["pointer"].isString()) {
    Json::Value structured;
    passage.pointer = span["pointer"].asString();
    bool parsed = false;
    if (doc.text.find(kBegin) != std::string::npos)
      parsed = memory_structured_parse_doc(doc.text, &structured, error);
    else {
      Json::CharReaderBuilder reader;
      std::istringstream stream(doc.text);
      parsed = Json::parseFromStream(reader, stream, &structured, error);
    }
    if (!parsed || !pointer_get(structured, passage.pointer, value))
      return reject(error, "memory evidence pointer missing");
    *text = value_text(*value);
  } else if (span.get("kind", "") == "bytes" && span["begin"].isUInt64() && span["end"].isUInt64()) {
    passage.begin = size_t(span["begin"].asUInt64());
    passage.end = size_t(span["end"].asUInt64());
    if (passage.begin > passage.end || passage.end > doc.text.size())
      return reject(error, "memory evidence span out of range");
    *text = doc.text.substr(passage.begin, passage.end - passage.begin);
    *value = *text;
  } else
    return reject(error, "invalid memory evidence span");
  if (memory_passage_evidence(passage)["id"] != ref["id"]) {
    text->clear();
    *value = Json::nullValue;
    return reject(error, "memory evidence identity mismatch");
  }
  return true;
}
} // namespace host_tools_internal

#endif
