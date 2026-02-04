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
  // If the schema changes, update docs/DB.md and this test in the same PR.
  if (!contains(s, "## Schema (v17)")) {
    std::fprintf(stderr, "docs drift: expected '## Schema (v17)' in %s\n", db_doc.c_str());
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
