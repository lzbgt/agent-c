#include "toolset_host.h"

#include "agent/tools.h"

#include <json/json.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

static Json::Value json_parse(const std::string& s) {
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  const bool ok = Json::parseFromStream(rb, iss, &v, &errs);
  assert(ok);
  return v;
}

static void test_file_apply_patch() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);
  assert(reg != nullptr);
  assert(exec.execute != nullptr);
  assert(agent_tool_registry_count(reg) >= 3);

  // Seed a file in the tool root (host-side tests can use std::filesystem directly).
  {
    std::ofstream f(root / "hello.txt", std::ios::binary);
    f << "hi\n";
  }

  agent_string_t out{};
  {
    Json::Value args(Json::objectValue);
    args["patch"] =
      "--- hello.txt\n"
      "+++ hello.txt\n"
      "@@ -1 +1 @@\n"
      "-hi\n"
      "+bye\n";
    const std::string req = json_stringify(args);
    assert(exec.execute(exec.ctx, "file_apply_patch", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["check"]["exit_code"].asInt() == 0);
    assert(resp["data"]["apply"]["exit_code"].asInt() == 0);
    agent_string_free(&out);
  }

  {
    std::ifstream f(root / "hello.txt", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    assert(ss.str() == "bye\n");
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_shell_exec() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_exec_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);

  Json::Value args(Json::objectValue);
  args["cmd"] = "echo OK";
  args["timeout_ms"] = 5000;
  args["max_output_bytes"] = 4096;
  const std::string req = json_stringify(args);

  agent_string_t out{};
  assert(exec.execute(exec.ctx, "shell_exec", req.c_str(), &out) == AGENT_OK);
  const Json::Value resp = json_parse(std::string(out.data, out.len));
  assert(resp["ok"].asBool());
  assert(resp["data"]["exit_code"].asInt() == 0);
  assert(resp["data"]["output"].asString().find("OK") != std::string::npos);
  agent_string_free(&out);

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_proc_exec() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_proc_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);

  Json::Value args(Json::objectValue);
  args["argv"] = Json::Value(Json::arrayValue);
  args["argv"].append("echo");
  args["argv"].append("OK");
  args["timeout_ms"] = 5000;
  args["max_output_bytes"] = 4096;
  const std::string req = json_stringify(args);

  agent_string_t out{};
  assert(exec.execute(exec.ctx, "proc_exec", req.c_str(), &out) == AGENT_OK);
  const Json::Value resp = json_parse(std::string(out.data, out.len));
  assert(resp["data"]["exit_code"].asInt() == 0);
  assert(resp["data"]["output"].asString().find("OK") != std::string::npos);
  agent_string_free(&out);

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

int main() {
  test_file_apply_patch();
  test_shell_exec();
  test_proc_exec();
  return 0;
}
