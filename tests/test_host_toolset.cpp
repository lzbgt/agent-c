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
  assert(agent_tool_registry_count(reg) >= 6);

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

static void test_fs_stat_list_read() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_fs_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "dir");

  // Create a large-ish directory that should be excluded by default for token efficiency.
  std::filesystem::create_directories(root / "node_modules" / "pkg");
  {
    std::ofstream f(root / "node_modules" / "pkg" / "big.txt", std::ios::binary);
    f << "hello\n";
  }

  // Create a file with enough lines to exercise pagination.
  {
    std::ofstream f(root / "dir" / "many.txt", std::ios::binary);
    for (int i = 1; i <= 250; i++) {
      f << "L" << i << "\n";
    }
  }
  {
    std::ofstream f(root / "dir" / "skip.log", std::ios::binary);
    f << "SKIPME_TOKEN_123\n";
  }

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);

  // fs_stat
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir/many.txt";
    args["count_lines"] = true;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_stat", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["is_file"].asBool());
    assert(resp["data"]["exists"].asBool());
    assert(resp["data"]["size_bytes"].asUInt64() > 0);
    assert(resp["data"]["is_binary"].asBool() == false);
    assert(resp["data"].isMember("mtime_unix_ms"));
    assert(resp["data"].isMember("ctime_unix_ms"));
    assert(resp["data"]["line_count_available"].asBool());
    assert(resp["data"]["total_lines"].asInt() == 250);
    assert(resp["data"]["output"].asString().find("many.txt") != std::string::npos);
    agent_string_free(&out);
  }

  // fs_list (non-recursive)
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir";
    args["recursive"] = false;
    args["max_entries"] = 50;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_list", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    bool found = false;
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      if (entries[i].isObject() && entries[i]["path"].isString()) {
        if (entries[i]["path"].asString().find("many.txt") != std::string::npos) {
          found = true;
          break;
        }
      }
    }
    assert(found);
    agent_string_free(&out);
  }

  // fs_list exclude_globs should filter returned entry paths
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir";
    args["recursive"] = false;
    args["max_entries"] = 50;
    Json::Value globs(Json::arrayValue);
    globs.append("*skip.log");
    args["exclude_globs"] = globs;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_list", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    bool saw_many = false;
    bool saw_skip = false;
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      if (!entries[i].isObject() || !entries[i]["path"].isString()) continue;
      const std::string p = entries[i]["path"].asString();
      if (p.find("many.txt") != std::string::npos) saw_many = true;
      if (p.find("skip.log") != std::string::npos) saw_skip = true;
    }
    assert(saw_many);
    assert(!saw_skip);
    agent_string_free(&out);
  }

  // text_search exclude_globs should filter scanned files
  {
    Json::Value args(Json::objectValue);
    args["query"] = "SKIPME_TOKEN_123";
    args["path"] = "dir";
    args["recursive"] = true;
    args["max_results"] = 20;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "text_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["matches_count"].asUInt64() >= 1);
    agent_string_free(&out);
  }
  {
    Json::Value args(Json::objectValue);
    args["query"] = "SKIPME_TOKEN_123";
    args["path"] = "dir";
    args["recursive"] = true;
    args["max_results"] = 20;
    Json::Value globs(Json::arrayValue);
    globs.append("*skip.log");
    args["exclude_globs"] = globs;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "text_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["matches_count"].asUInt64() == 0);
    agent_string_free(&out);
  }

  // fs_list (recursive, default excludes should hide node_modules)
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["max_entries"] = 2000;
    args["max_depth"] = 4;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_list", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      if (entries[i].isObject() && entries[i]["path"].isString()) {
        // Should not surface huge dependency trees unless explicitly requested.
        assert(entries[i]["path"].asString().find("node_modules") == std::string::npos);
      }
    }
    agent_string_free(&out);
  }

  // fs_list (recursive, disable default excludes should allow node_modules)
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["use_default_excludes"] = false;
    args["max_entries"] = 2000;
    args["max_depth"] = 4;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_list", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    bool saw = false;
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      if (entries[i].isObject() && entries[i]["path"].isString()) {
        if (entries[i]["path"].asString().find("node_modules") != std::string::npos) {
          saw = true;
          break;
        }
      }
    }
    assert(saw);
    agent_string_free(&out);
  }

  // fs_read pagination
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir/many.txt";
    args["start_line"] = 1;
    args["max_lines"] = 100;
    args["max_chars"] = 20000;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_read", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["is_binary"].asBool() == false);
    assert(resp["data"].isMember("mtime_unix_ms"));
    assert(resp["data"].isMember("ctime_unix_ms"));
    assert(resp["data"]["lines_returned"].asInt() == 100);
    assert(resp["data"]["has_more"].asBool());
    assert(resp["data"]["next_start_line"].asInt() == 101);
    const std::string content = resp["data"]["content"].asString();
    assert(content.find("L1\n") != std::string::npos);
    assert(content.find("L100\n") != std::string::npos);
    agent_string_free(&out);
  }

  // fs_read last page
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir/many.txt";
    args["start_line"] = 201;
    args["max_lines"] = 200;
    args["max_chars"] = 20000;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_read", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["lines_returned"].asInt() == 50);
    assert(resp["data"]["has_more"].asBool() == false);
    assert(resp["data"]["total_lines"].asInt() == 250);
    const std::string content = resp["data"]["content"].asString();
    assert(content.find("L201\n") != std::string::npos);
    assert(content.find("L250\n") != std::string::npos);
    agent_string_free(&out);
  }

  // fs_read end_line should stop early and signal "has_more"
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir/many.txt";
    args["start_line"] = 1;
    args["end_line"] = 5;
    args["max_lines"] = 200;
    args["max_chars"] = 20000;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_read", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["lines_returned"].asInt() == 5);
    assert(resp["data"]["has_more"].asBool());
    assert(resp["data"]["next_start_line"].asInt() == 6);
    assert(resp["data"]["stopped_due_to"].asString() == "end_line");
    const std::string content = resp["data"]["content"].asString();
    assert(content.find("L1\n") != std::string::npos);
    assert(content.find("L5\n") != std::string::npos);
    assert(content.find("L6\n") == std::string::npos);
    agent_string_free(&out);
  }

  // fs_read with_line_numbers
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir/many.txt";
    args["start_line"] = 1;
    args["max_lines"] = 3;
    args["max_chars"] = 20000;
    args["with_line_numbers"] = true;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_read", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const std::string content = resp["data"]["content"].asString();
    assert(content.find("1: L1\n") != std::string::npos);
    assert(content.find("2: L2\n") != std::string::npos);
    assert(content.find("3: L3\n") != std::string::npos);
    agent_string_free(&out);
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

static void test_text_search() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_search_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "src");
  std::filesystem::create_directories(root / "node_modules" / "pkg");

  {
    std::ofstream f(root / "src" / "a.txt", std::ios::binary);
    f << "hello world\n";
    f << "needle here\n";
  }
  {
    std::ofstream f(root / "src" / "a.cpp", std::ios::binary);
    f << "int main() { /* needle */ return 0; }\n";
  }
  {
    std::ofstream f(root / "node_modules" / "pkg" / "b.txt", std::ios::binary);
    f << "needle in excluded dir\n";
  }

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);

  // Default excludes should skip node_modules and still find src match.
  {
    Json::Value args(Json::objectValue);
    args["query"] = "needle";
    args["path"] = ".";
    args["recursive"] = true;
    args["max_results"] = 50;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "text_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["tool"].asString() == "text_search");
    const auto& matches = resp["data"]["matches"];
    assert(matches.isArray());
    bool saw_src = false;
    bool saw_node = false;
    for (Json::ArrayIndex i = 0; i < matches.size(); i++) {
      const auto& m = matches[i];
      if (!m.isObject() || !m["path"].isString()) continue;
      const std::string p = m["path"].asString();
      if (p.find("src/a.txt") != std::string::npos) saw_src = true;
      if (p.find("node_modules") != std::string::npos) saw_node = true;
    }
    assert(saw_src);
    assert(!saw_node);
    agent_string_free(&out);
  }

  // Disabling default excludes should allow node_modules match.
  {
    Json::Value args(Json::objectValue);
    args["query"] = "needle";
    args["path"] = ".";
    args["recursive"] = true;
    args["use_default_excludes"] = false;
    args["max_results"] = 50;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "text_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& matches = resp["data"]["matches"];
    assert(matches.isArray());
    bool saw_node = false;
    for (Json::ArrayIndex i = 0; i < matches.size(); i++) {
      const auto& m = matches[i];
      if (!m.isObject() || !m["path"].isString()) continue;
      if (m["path"].asString().find("node_modules") != std::string::npos) {
        saw_node = true;
        break;
      }
    }
    assert(saw_node);
    agent_string_free(&out);
  }

  // Extension filter should restrict which files are scanned.
  {
    Json::Value args(Json::objectValue);
    args["query"] = "needle";
    args["path"] = ".";
    args["recursive"] = true;
    args["use_default_excludes"] = true;
    args["extensions"] = Json::Value(Json::arrayValue);
    args["extensions"].append(".cpp");
    args["max_results"] = 50;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "text_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& matches = resp["data"]["matches"];
    assert(matches.isArray());
    bool saw_cpp = false;
    bool saw_txt = false;
    for (Json::ArrayIndex i = 0; i < matches.size(); i++) {
      const auto& m = matches[i];
      if (!m.isObject() || !m["path"].isString()) continue;
      const std::string p = m["path"].asString();
      if (p.find("src/a.cpp") != std::string::npos) saw_cpp = true;
      if (p.find("src/a.txt") != std::string::npos) saw_txt = true;
    }
    assert(saw_cpp);
    assert(!saw_txt);
    agent_string_free(&out);
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_fs_find() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_find_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "src");
  std::filesystem::create_directories(root / "node_modules" / "pkg");

  {
    std::ofstream f(root / "src" / "main.cpp", std::ios::binary);
    f << "int main() { return 0; }\n";
  }
  {
    std::ofstream f(root / "node_modules" / "pkg" / "lib.js", std::ios::binary);
    f << "console.log('x');\n";
  }

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);

  // Default excludes should skip node_modules.
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["max_results"] = 200;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_find", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["tool"].asString() == "fs_find");
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    bool saw_src = false;
    bool saw_node = false;
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      const auto& e = entries[i];
      if (!e.isObject() || !e["path"].isString()) continue;
      const std::string p = e["path"].asString();
      if (p.find("src/main.cpp") != std::string::npos) saw_src = true;
      if (p.find("node_modules") != std::string::npos) saw_node = true;
    }
    assert(saw_src);
    assert(!saw_node);
    agent_string_free(&out);
  }

  // Disabling default excludes should allow node_modules paths.
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["use_default_excludes"] = false;
    args["max_results"] = 200;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_find", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    bool saw_node = false;
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      const auto& e = entries[i];
      if (!e.isObject() || !e["path"].isString()) continue;
      if (e["path"].asString().find("node_modules") != std::string::npos) {
        saw_node = true;
        break;
      }
    }
    assert(saw_node);
    agent_string_free(&out);
  }

  // Extension filter should restrict files.
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["extensions"] = Json::Value(Json::arrayValue);
    args["extensions"].append(".cpp");
    args["max_results"] = 200;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_find", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    bool saw_cpp = false;
    bool saw_js = false;
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      const auto& e = entries[i];
      if (!e.isObject() || !e["path"].isString()) continue;
      const std::string p = e["path"].asString();
      if (p.find("src/main.cpp") != std::string::npos) saw_cpp = true;
      if (p.find("lib.js") != std::string::npos) saw_js = true;
    }
    assert(saw_cpp);
    assert(!saw_js);
    agent_string_free(&out);
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

int main() {
  test_file_apply_patch();
  test_fs_stat_list_read();
  test_shell_exec();
  test_proc_exec();
  test_text_search();
  test_fs_find();
  return 0;
}
