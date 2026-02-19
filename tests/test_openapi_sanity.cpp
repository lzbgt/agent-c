#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

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

static std::string trim(const std::string& input) {
  const auto first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = input.find_last_not_of(" \t\r\n");
  return input.substr(first, last - first + 1);
}

static bool parse_ref_value(const std::string& line, std::string* out) {
  if (!out) return false;
  const auto pos = line.find("$ref:");
  if (pos == std::string::npos) return false;
  std::string rest = trim(line.substr(pos + 5));
  if (rest.empty()) return false;
  // Drop inline comments.
  const auto comment = rest.find('#');
  if (comment != std::string::npos) {
    rest = trim(rest.substr(0, comment));
  }
  if (rest.empty()) return false;
  if (rest.front() == '\'' || rest.front() == '"') {
    const char quote = rest.front();
    const auto end = rest.find(quote, 1);
    if (end == std::string::npos) return false;
    *out = rest.substr(1, end - 1);
    return true;
  }
  const auto space = rest.find_first_of(" \t\r\n");
  if (space != std::string::npos) {
    rest = rest.substr(0, space);
  }
  *out = rest;
  return !out->empty();
}

static bool load_with_refs(const std::string& path, std::string* out) {
  if (!out) return false;
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack;
  stack.push_back(path);
  out->clear();

  while (!stack.empty()) {
    std::string current = stack.back();
    stack.pop_back();
    if (current.empty()) continue;
    if (visited.count(current)) continue;
    visited.insert(current);

    std::string content;
    if (!read_all(current, &content)) {
      return false;
    }
    out->append(content);
    out->append("\n");

    const std::filesystem::path base_dir = std::filesystem::path(current).parent_path();
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
      std::string ref;
      if (!parse_ref_value(line, &ref)) continue;
      if (ref.empty()) continue;
      if (ref.find("://") != std::string::npos) continue;
      if (!ref.empty() && ref.front() == '#') continue;
      const auto hash = ref.find('#');
      std::string ref_path = (hash == std::string::npos) ? ref : ref.substr(0, hash);
      if (ref_path.empty()) continue;
      std::filesystem::path resolved = base_dir / ref_path;
      resolved = resolved.lexically_normal();
      stack.push_back(resolved.string());
    }
  }
  return true;
}

static bool contains(const std::string& haystack, const char* needle) {
  if (!needle) return false;
  return haystack.find(needle) != std::string::npos;
}

int main(int argc, char** argv) {
  const std::string agentd_spec = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  const std::string broker_spec = (argc >= 3 && argv[2]) ? std::string(argv[2]) : std::string();
  if (agentd_spec.empty() || broker_spec.empty()) {
    std::fprintf(stderr, "usage: test_openapi_sanity <path-to-agentd.yaml> <path-to-broker.yaml>\n");
    return 2;
  }

  std::string a;
  if (!load_with_refs(agentd_spec, &a)) {
    std::fprintf(stderr, "failed to read spec: %s\n", agentd_spec.c_str());
    return 1;
  }
  if (!contains(a, "openapi: 3.0.3")) {
    std::fprintf(stderr, "agentd openapi drift: expected openapi version header in %s\n", agentd_spec.c_str());
    return 1;
  }
  if (!contains(a, "/api/v1/run:") || !contains(a, "/api/v1/run_async:")) {
    std::fprintf(stderr, "agentd openapi drift: missing run endpoints in %s\n", agentd_spec.c_str());
    return 1;
  }
  if (!contains(a, "/api/v1/job:") || !contains(a, "/api/v1/job/stream:")) {
    std::fprintf(stderr, "agentd openapi drift: missing job endpoints in %s\n", agentd_spec.c_str());
    return 1;
  }
  if (!contains(a, "/api/v1/trace:")) {
    std::fprintf(stderr, "agentd openapi drift: missing /api/v1/trace in %s\n", agentd_spec.c_str());
    return 1;
  }
  if (!contains(a, "/api/v1/workflow/submit:") || !contains(a, "/api/v1/workflow:") || !contains(a, "/api/v1/workflows:")) {
    std::fprintf(stderr, "agentd openapi drift: missing workflow endpoints in %s\n", agentd_spec.c_str());
    return 1;
  }
  if (!contains(a, "trace_id")) {
    std::fprintf(stderr, "agentd openapi drift: expected trace_id in %s\n", agentd_spec.c_str());
    return 1;
  }

  std::string b;
  if (!load_with_refs(broker_spec, &b)) {
    std::fprintf(stderr, "failed to read spec: %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "openapi: 3.0.3")) {
    std::fprintf(stderr, "broker openapi drift: expected openapi version header in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "/v1/agents:") || !contains(b, "/v1/orchestrate:")) {
    std::fprintf(stderr, "broker openapi drift: missing core endpoints in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "/v1/trace:")) {
    std::fprintf(stderr, "broker openapi drift: missing /v1/trace in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "/v1/agents/{agent_id}/members:") || !contains(b, "/v1/agents/{agent_id}/membership_audit:")) {
    std::fprintf(stderr, "broker openapi drift: missing membership endpoints in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "/v1/client_auth/status:") || !contains(b, "/v1/client_auth/reload:")) {
    std::fprintf(stderr, "broker openapi drift: missing client auth endpoints in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "membership:")) {
    std::fprintf(stderr, "broker openapi drift: missing membership trace field in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "orchestrate:")) {
    std::fprintf(stderr, "broker openapi drift: expected orchestrate field documented in %s\n", broker_spec.c_str());
    return 1;
  }
  if (!contains(b, "/healthz:") || !contains(b, "/readyz:")) {
    std::fprintf(stderr, "broker openapi drift: missing health endpoints in %s\n", broker_spec.c_str());
    return 1;
  }
  return 0;
}
