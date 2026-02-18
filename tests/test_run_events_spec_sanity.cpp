#include <json/json.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static bool read_all(const std::string& path, std::string* out) {
  if (!out) return false;
  out->clear();
  std::ifstream f(path, std::ios::in | std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static bool parse_json_object(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) *out_err = errs;
    return false;
  }
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  *out = v;
  return true;
}

static bool ends_with(const std::string& s, const char* suffix) {
  if (!suffix) return false;
  const std::string suf(suffix);
  if (s.size() < suf.size()) return false;
  return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static std::string trim_ws(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}

static bool is_nonneg_int(const Json::Value& v) {
  if (!(v.isInt() || v.isUInt() || v.isInt64() || v.isUInt64())) return false;
  return v.asInt64() >= 0;
}

static bool validate_event_object(const Json::Value& v, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!v.isObject()) {
    if (out_err) *out_err = "expected JSON object";
    return false;
  }
  if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
    if (out_err) *out_err = "missing/invalid type (expected non-empty string)";
    return false;
  }
  if (v.isMember("schema") && !v["schema"].isString()) {
    if (out_err) *out_err = "schema must be string";
    return false;
  }
  if (v.isMember("ts_unix_ms") && !is_nonneg_int(v["ts_unix_ms"])) {
    if (out_err) *out_err = "ts_unix_ms must be integer >= 0";
    return false;
  }
  if (v.isMember("step") && !is_nonneg_int(v["step"])) {
    if (out_err) *out_err = "step must be integer >= 0";
    return false;
  }
  if (v.isMember("epoch") && !is_nonneg_int(v["epoch"])) {
    if (out_err) *out_err = "epoch must be integer >= 0";
    return false;
  }
  if (v.isMember("source") && !v["source"].isString()) {
    if (out_err) *out_err = "source must be string";
    return false;
  }
  if (v.isMember("run_id") && !is_nonneg_int(v["run_id"])) {
    if (out_err) *out_err = "run_id must be integer >= 0";
    return false;
  }
  if (v.isMember("workflow_id") && !v["workflow_id"].isString()) {
    if (out_err) *out_err = "workflow_id must be string";
    return false;
  }
  if (v.isMember("session_id") && !v["session_id"].isString()) {
    if (out_err) *out_err = "session_id must be string";
    return false;
  }
  return true;
}

static int schema_sanity(const std::string& schema_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(schema_dir)) {
    std::fprintf(stderr, "schema dir missing: %s\n", schema_dir.c_str());
    return 1;
  }

  int files = 0;
  for (const auto& ent : fs::directory_iterator(schema_dir)) {
    if (!ent.is_regular_file()) continue;
    const std::string path = ent.path().string();
    if (!ends_with(path, ".json")) continue;
    files++;

    std::string s;
    if (!read_all(path, &s)) {
      std::fprintf(stderr, "failed to read schema: %s\n", path.c_str());
      return 1;
    }
    Json::Value v;
    std::string err;
    if (!parse_json_object(s, &v, &err)) {
      std::fprintf(stderr, "invalid schema JSON: %s: %s\n", path.c_str(), err.c_str());
      return 1;
    }
    if (!v.isMember("$schema") || !v["$schema"].isString() || v["$schema"].asString().empty()) {
      std::fprintf(stderr, "schema missing $schema: %s\n", path.c_str());
      return 1;
    }
    if (!v.isMember("title") || !v["title"].isString() || v["title"].asString().empty()) {
      std::fprintf(stderr, "schema missing title: %s\n", path.c_str());
      return 1;
    }
    if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
      std::fprintf(stderr, "schema missing type: %s\n", path.c_str());
      return 1;
    }
    if (v["type"].asString() != "object") {
      std::fprintf(stderr, "schema unexpected type (expected object): %s\n", path.c_str());
      return 1;
    }
  }

  if (files == 0) {
    std::fprintf(stderr, "no schema files found under: %s\n", schema_dir.c_str());
    return 1;
  }
  return 0;
}

static int fixtures_sanity(const std::string& fixtures_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(fixtures_dir)) {
    std::fprintf(stderr, "fixtures dir missing: %s\n", fixtures_dir.c_str());
    return 1;
  }

  int files = 0;
  int events = 0;
  for (const auto& ent : fs::directory_iterator(fixtures_dir)) {
    if (!ent.is_regular_file()) continue;
    const std::string path = ent.path().string();
    if (!ends_with(path, ".jsonl")) continue;
    files++;

    std::ifstream f(path);
    if (!f) {
      std::fprintf(stderr, "failed to read fixtures: %s\n", path.c_str());
      return 1;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
      line_no++;
      const std::string trimmed = trim_ws(line);
      if (trimmed.empty()) continue;
      Json::Value v;
      std::string err;
      if (!parse_json_object(trimmed, &v, &err)) {
        std::fprintf(stderr, "invalid fixture json: %s:%d: %s\n", path.c_str(), line_no, err.c_str());
        return 1;
      }
      if (!validate_event_object(v, &err)) {
        std::fprintf(stderr, "invalid fixture event: %s:%d: %s\n", path.c_str(), line_no, err.c_str());
        return 1;
      }
      events++;
    }
  }

  if (files == 0) {
    std::fprintf(stderr, "no fixture files found under: %s\n", fixtures_dir.c_str());
    return 1;
  }
  if (events == 0) {
    std::fprintf(stderr, "no fixture events found under: %s\n", fixtures_dir.c_str());
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  const std::string root = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  if (root.empty()) {
    std::fprintf(stderr, "usage: test_run_events_spec_sanity <path-to-docs/spec/run-events>\n");
    return 2;
  }
  const std::string schema_dir = root + "/schema";
  const std::string fixtures_dir = root + "/fixtures";
  if (schema_sanity(schema_dir) != 0) return 1;
  return fixtures_sanity(fixtures_dir);
}
