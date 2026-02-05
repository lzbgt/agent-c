#include <cctype>
#include <cstdio>
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

static bool contains(const std::string& haystack, const char* needle) {
  if (!needle) return false;
  return haystack.find(needle) != std::string::npos;
}

static bool extract_k_schema_version_from_agent_db_cpp(const std::string& s, int* out_ver) {
  if (out_ver) *out_ver = 0;
  if (!out_ver) return false;
  const std::string key = "const int kSchemaVersion = ";
  const size_t pos = s.find(key);
  if (pos == std::string::npos) return false;
  size_t i = pos + key.size();
  while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
  int v = 0;
  bool any = false;
  while (i < s.size() && std::isdigit((unsigned char)s[i])) {
    any = true;
    v = v * 10 + (s[i] - '0');
    i++;
  }
  if (!any) return false;
  *out_ver = v;
  return true;
}

int main(int argc, char** argv) {
  const std::string db_doc = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  if (db_doc.empty()) {
    std::fprintf(stderr, "usage: test_docs_sanity <path-to-docs/DB.md>\n");
    return 2;
  }

  std::string s;
  if (!read_all(db_doc, &s)) {
    std::fprintf(stderr, "failed to read doc: %s\n", db_doc.c_str());
    return 1;
  }

  // Guardrail: DB schema docs should match the current daemon schema version.
  // If the schema changes, update docs/DB.md in the same PR.
  //
  // This test derives the expected version from daemon/src/agent_db.cpp (kSchemaVersion) so it stays in sync
  // without hardcoding a magic number here.
  std::string root = db_doc;
  const std::string suffix = "/docs/DB.md";
  if (root.size() >= suffix.size() && root.rfind(suffix) == (root.size() - suffix.size())) {
    root.resize(root.size() - suffix.size());
  }
  const std::string agent_db_cpp = root + "/daemon/src/agent_db.cpp";
  std::string adb;
  int ver = 0;
  if (!read_all(agent_db_cpp, &adb) || !extract_k_schema_version_from_agent_db_cpp(adb, &ver) || ver <= 0) {
    std::fprintf(stderr, "docs sanity: failed to infer kSchemaVersion from %s\n", agent_db_cpp.c_str());
    return 1;
  }
  const std::string header = "## Schema (v" + std::to_string(ver) + ")";
  if (s.find(header) == std::string::npos) {
    std::fprintf(stderr, "docs drift: expected '%s' in %s\n", header.c_str(), db_doc.c_str());
    return 1;
  }
  if (!contains(s, "### `jobs`")) {
    std::fprintf(stderr, "docs drift: expected jobs table documented in %s\n", db_doc.c_str());
    return 1;
  }
  if (!contains(s, "### `workflows`")) {
    std::fprintf(stderr, "docs drift: expected workflows table documented in %s\n", db_doc.c_str());
    return 1;
  }
  if (!contains(s, "### `workflow_tasks`")) {
    std::fprintf(stderr, "docs drift: expected workflow_tasks table documented in %s\n", db_doc.c_str());
    return 1;
  }
  if (!contains(s, "### `scene_states`")) {
    std::fprintf(stderr, "docs drift: expected scene_states table documented in %s\n", db_doc.c_str());
    return 1;
  }
  return 0;
}
