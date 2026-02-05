#include <json/json.h>

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

static bool parse_json_any(const std::string& s, Json::Value* out, std::string* out_err) {
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
  *out = v;
  return true;
}

static bool ends_with(const std::string& s, const char* suffix) {
  if (!suffix) return false;
  const std::string suf(suffix);
  if (s.size() < suf.size()) return false;
  return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
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

static int fixture_sanity(const std::string& fixtures_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(fixtures_dir)) {
    std::fprintf(stderr, "fixtures dir missing: %s\n", fixtures_dir.c_str());
    return 1;
  }

  int files = 0;
  for (const auto& ent : fs::directory_iterator(fixtures_dir)) {
    if (!ent.is_regular_file()) continue;
    const std::string path = ent.path().string();
    if (!ends_with(path, ".jsonl")) continue;
    files++;

    std::string s;
    if (!read_all(path, &s)) {
      std::fprintf(stderr, "failed to read fixture: %s\n", path.c_str());
      return 1;
    }

    std::istringstream iss(s);
    std::string line;
    int line_no = 0;
    while (std::getline(iss, line)) {
      line_no++;
      // Allow blank lines.
      bool all_ws = true;
      for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          all_ws = false;
          break;
        }
      }
      if (all_ws) continue;

      Json::Value v;
      std::string err;
      if (!parse_json_any(line, &v, &err) || !v.isObject()) {
        std::fprintf(stderr, "invalid fixture JSON at %s:%d: %s\n", path.c_str(), line_no, err.c_str());
        return 1;
      }
      if (!v.isMember("msg_id") || !v["msg_id"].isString() || v["msg_id"].asString().empty()) {
        std::fprintf(stderr, "fixture missing msg_id at %s:%d\n", path.c_str(), line_no);
        return 1;
      }
      if (!v.isMember("ts_utc_ms") || !(v["ts_utc_ms"].isInt64() || v["ts_utc_ms"].isUInt64() || v["ts_utc_ms"].isInt())) {
        std::fprintf(stderr, "fixture missing/invalid ts_utc_ms at %s:%d\n", path.c_str(), line_no);
        return 1;
      }
      if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
        std::fprintf(stderr, "fixture missing type at %s:%d\n", path.c_str(), line_no);
        return 1;
      }
      if (!v.isMember("from") || !v["from"].isString() || v["from"].asString().empty()) {
        std::fprintf(stderr, "fixture missing from at %s:%d\n", path.c_str(), line_no);
        return 1;
      }
      if (!v.isMember("to") || !(v["to"].isString() || v["to"].isNull())) {
        std::fprintf(stderr, "fixture missing/invalid to at %s:%d\n", path.c_str(), line_no);
        return 1;
      }
      if (!v.isMember("body") || !v["body"].isObject()) {
        std::fprintf(stderr, "fixture missing/invalid body (expected object) at %s:%d\n", path.c_str(), line_no);
        return 1;
      }
    }
  }

  if (files == 0) {
    std::fprintf(stderr, "no fixture files found under: %s\n", fixtures_dir.c_str());
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  const std::string um_eais_dir = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  if (um_eais_dir.empty()) {
    std::fprintf(stderr, "usage: test_um_eais_spec_sanity <path-to-docs/spec/um-eais>\n");
    return 2;
  }
  const std::string schema_dir = um_eais_dir + "/schema";
  const std::string fixtures_dir = um_eais_dir + "/fixtures";

  if (schema_sanity(schema_dir) != 0) return 1;
  if (fixture_sanity(fixtures_dir) != 0) return 1;
  return 0;
}

