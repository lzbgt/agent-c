#include <string>
#include <vector>
#include <iostream>

#ifdef _WIN32
#include <cstdio>
#include <cstdlib>

static int run_cmd_capture(const std::string& cmd, std::string* out) {
  if (out) out->clear();
  FILE* pipe = _popen(cmd.c_str(), "r");
  if (!pipe) return -1;
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe)) {
    if (out) out->append(buf);
  }
  int rc = _pclose(pipe);
  return rc;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc < 3) {
    std::cerr << "usage: test_tool_plugin_host_limits <plugin_host_bin> <plugin_path>\n";
    return 2;
  }
  const std::string host = argv[1] ? argv[1] : "";
  const std::string plugin = argv[2] ? argv[2] : "";
  if (host.empty() || plugin.empty()) {
    std::cerr << "missing required args\n";
    return 2;
  }

  std::string cmd = "\"" + host + "\" --plugin \"" + plugin +
    "\" --limit-cpu-ms 1 --limit-wall-ms 1 --limit-as-mb 1 2>&1";

  std::string output;
  const int rc = run_cmd_capture(cmd, &output);
  if (rc == 0) {
    std::cerr << "expected limit failure, got success\n";
    return 1;
  }
  const bool has_failed = output.find("Failed to apply limits") != std::string::npos;
  const bool has_unsupported = output.find("resource limits are not supported") != std::string::npos;
  if (!has_failed && !has_unsupported) {
    std::cerr << "missing expected limit error output: " << output << "\n";
    return 1;
  }
  return 0;
#else
  (void)argc;
  (void)argv;
  return 0;
#endif
}
