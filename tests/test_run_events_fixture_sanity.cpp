#include <json/json.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

static bool is_nonnegative_int(const Json::Value& v) {
  if (!v.isInt64() && !v.isInt()) return false;
  return v.asInt64() >= 0;
}

int main(int argc, char** argv) {
  const std::string fixtures_path = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  if (fixtures_path.empty()) {
    std::fprintf(stderr, "usage: test_run_events_fixture_sanity <path-to-fixture.jsonl>\n");
    return 2;
  }

  std::ifstream f(fixtures_path);
  if (!f) {
    std::fprintf(stderr, "failed to read fixture: %s\n", fixtures_path.c_str());
    return 1;
  }

  std::string line;
  int line_no = 0;
  while (std::getline(f, line)) {
    line_no++;
    if (line.empty()) continue;
    Json::Value v;
    std::string err;
    if (!parse_json_any(line, &v, &err)) {
      std::fprintf(stderr, "invalid JSON at line %d: %s\n", line_no, err.c_str());
      return 1;
    }
    if (!v.isObject()) {
      std::fprintf(stderr, "expected object at line %d\n", line_no);
      return 1;
    }
    if (!v.isMember("type") || !v["type"].isString() || v["type"].asString().empty()) {
      std::fprintf(stderr, "missing type at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("schema") && (!v["schema"].isString() || v["schema"].asString().empty())) {
      std::fprintf(stderr, "invalid schema at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("ts_unix_ms") && !is_nonnegative_int(v["ts_unix_ms"])) {
      std::fprintf(stderr, "invalid ts_unix_ms at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("step") && !is_nonnegative_int(v["step"])) {
      std::fprintf(stderr, "invalid step at line %d\n", line_no);
      return 1;
    }
    if (v.isMember("epoch") && !is_nonnegative_int(v["epoch"])) {
      std::fprintf(stderr, "invalid epoch at line %d\n", line_no);
      return 1;
    }
  }
  return 0;
}
