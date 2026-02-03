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
  const std::string agentd_spec = (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string();
  const std::string broker_spec = (argc >= 3 && argv[2]) ? std::string(argv[2]) : std::string();
  if (agentd_spec.empty() || broker_spec.empty()) {
    std::fprintf(stderr, "usage: test_openapi_sanity <path-to-agentd.yaml> <path-to-broker.yaml>\n");
    return 2;
  }

  std::string a;
  if (!read_all(agentd_spec, &a)) {
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
  if (!contains(a, "trace_id")) {
    std::fprintf(stderr, "agentd openapi drift: expected trace_id in %s\n", agentd_spec.c_str());
    return 1;
  }

  std::string b;
  if (!read_all(broker_spec, &b)) {
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
