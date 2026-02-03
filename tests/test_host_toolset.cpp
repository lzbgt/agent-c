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

static bool registry_contains(agent_tool_registry_t* reg, const std::string& name) {
  const size_t n = agent_tool_registry_count(reg);
  for (size_t i = 0; i < n; i++) {
    agent_tool_def_view_t v{};
    if (agent_tool_registry_get(reg, i, &v) != AGENT_OK) continue;
    if (v.name && name == v.name) return true;
  }
  return false;
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

  // Security: in scoped mode (root_dir set), unsafe_paths must not allow path traversal.
  {
    const std::string escape_name = std::string("escape_") + std::to_string((long long)getpid()) + ".txt";
    Json::Value args(Json::objectValue);
    args["patch"] = std::string("--- /dev/null\n") +
      "+++ ../" + escape_name + "\n" +
      "@@ -0,0 +1 @@\n"
      "+nope\n";
    args["unsafe_paths"] = true;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "file_apply_patch", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(!resp["ok"].asBool());
    agent_string_free(&out);
    // Ensure the escaped file was not created.
    const auto escaped = root.parent_path() / escape_name;
    std::error_code ec;
    assert(!std::filesystem::exists(escaped, ec));
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_readonly_policy_disables_exec_and_patch() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_ro_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();
  cfg.policy = HostToolsetPolicyMode::ReadOnly;

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);
  assert(reg != nullptr);
  assert(exec.execute != nullptr);

  assert(!registry_contains(reg, "shell_exec"));
  assert(!registry_contains(reg, "proc_exec"));
  assert(!registry_contains(reg, "file_apply_patch"));
  assert(registry_contains(reg, "fs_stat"));
  assert(registry_contains(reg, "fs_list"));
  assert(registry_contains(reg, "fs_find"));
  assert(registry_contains(reg, "fs_read"));
  assert(registry_contains(reg, "text_search"));
  assert(registry_contains(reg, "artifact_register"));

  // Defense-in-depth: executor rejects disabled tools even if called directly.
  {
    Json::Value args(Json::objectValue);
    args["cmd"] = "echo hi";
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "shell_exec", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(!resp["ok"].asBool());
    assert(resp["error"].asString().find("disabled") != std::string::npos);
    agent_string_free(&out);
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_scoped_mode_disables_exec_tools_but_keeps_patch() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_scoped_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();
  cfg.policy = HostToolsetPolicyMode::Full;
  cfg.enable_process_exec = false;

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);
  assert(reg != nullptr);
  assert(exec.execute != nullptr);

  assert(!registry_contains(reg, "shell_exec"));
  assert(!registry_contains(reg, "proc_exec"));
  assert(registry_contains(reg, "file_apply_patch"));
  assert(registry_contains(reg, "fs_stat"));
  assert(registry_contains(reg, "fs_list"));
  assert(registry_contains(reg, "fs_find"));
  assert(registry_contains(reg, "fs_read"));
  assert(registry_contains(reg, "text_search"));
  assert(registry_contains(reg, "artifact_register"));

  // Defense-in-depth: executor rejects disabled exec tools even if called directly.
  {
    Json::Value args(Json::objectValue);
    args["cmd"] = "echo hi";
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "shell_exec", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(!resp["ok"].asBool());
    assert(resp["error"].asString().find("disabled") != std::string::npos);
    agent_string_free(&out);
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_scoped_mode_denies_symlink_escapes() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_tools_symlink_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  // Create a file outside root, and a symlink inside root pointing to the parent dir.
  const auto outside_dir = root.parent_path();
  const auto secret = outside_dir / ("secret_" + std::to_string((long long)getpid()) + ".txt");
  {
    std::ofstream f(secret, std::ios::binary);
    f << "TOP_SECRET\n";
  }
  const auto link = root / "out";
  std::error_code ec;
  std::filesystem::create_directory_symlink(outside_dir, link, ec);
  if (ec) {
    // Some environments restrict symlink creation; skip this test in that case.
    std::filesystem::remove_all(root);
    std::filesystem::remove(secret, ec);
    return;
  }

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();
  cfg.policy = HostToolsetPolicyMode::Full;
  cfg.enable_process_exec = false;
  cfg.allow_symlinks = false;

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);

  // fs_read should reject traversing the symlink.
  {
    Json::Value args(Json::objectValue);
    args["path"] = std::string("out/") + secret.filename().string();
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_read", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(!resp["ok"].asBool());
    agent_string_free(&out);
  }

  // fs_list should not expose symlink entry in scoped no-symlinks mode.
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["max_entries"] = 200;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_list", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      if (!entries[i].isObject() || !entries[i]["path"].isString()) continue;
      const std::string p = entries[i]["path"].asString();
      assert(p.find("out") == std::string::npos);
    }
    agent_string_free(&out);
  }

  // fs_find should not traverse the symlink and reveal the secret file.
  {
    Json::Value args(Json::objectValue);
    args["path"] = ".";
    args["recursive"] = true;
    args["max_results"] = 200;
    args["name_substring"] = secret.filename().string();
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "fs_find", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& entries = resp["data"]["entries"];
    assert(entries.isArray());
    for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
      if (!entries[i].isObject() || !entries[i]["path"].isString()) continue;
      const std::string p = entries[i]["path"].asString();
      assert(p.find(secret.filename().string()) == std::string::npos);
    }
    agent_string_free(&out);
  }

  // text_search should not traverse the symlink and find the secret contents.
  {
    Json::Value args(Json::objectValue);
    args["query"] = "TOP_SECRET";
    args["path"] = ".";
    args["recursive"] = true;
    args["max_results"] = 50;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "text_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& matches = resp["data"]["matches"];
    assert(matches.isArray());
    assert(matches.empty());
    agent_string_free(&out);
  }

  // file_apply_patch should reject writing through the symlink.
  {
    const std::string escape_name = std::string("escape_via_symlink_") + std::to_string((long long)getpid()) + ".txt";
    Json::Value args(Json::objectValue);
    args["patch"] = std::string("--- /dev/null\n") +
      "+++ out/" + escape_name + "\n" +
      "@@ -0,0 +1 @@\n"
      "+nope\n";
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "file_apply_patch", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(!resp["ok"].asBool());
    agent_string_free(&out);
    // Ensure the escaped file was not created (it would have landed outside root via symlink).
    assert(!std::filesystem::exists(outside_dir / escape_name, ec));
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
  std::filesystem::remove(secret, ec);
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
  {
    std::ofstream f(root / ".gitignore", std::ios::binary);
    // Best-effort (single-file) gitignore support in host tools. This test does not require a real `.git/` directory.
    f << "dir/skip.log\n";
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

  // fs_list respect_gitignore should filter returned entry paths
  {
    Json::Value args(Json::objectValue);
    args["path"] = "dir";
    args["recursive"] = false;
    args["max_entries"] = 50;
    args["respect_gitignore"] = true;
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

  // text_search respect_gitignore should filter scanned files
  {
    Json::Value args(Json::objectValue);
    args["query"] = "SKIPME_TOKEN_123";
    args["path"] = "dir";
    args["recursive"] = true;
    args["max_results"] = 20;
    args["respect_gitignore"] = true;
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

static void test_artifact_register() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_artifact_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();
  cfg.policy = HostToolsetPolicyMode::ReadOnly;

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);
  assert(registry_contains(reg, "artifact_register"));

  const auto p = root / "hello.wav";
  {
    std::ofstream f(p, std::ios::binary);
    f << "RIFF";
  }

  Json::Value args(Json::objectValue);
  args["path"] = "hello.wav";
  args["autoplay"] = true;
  args["repeat"] = 2;
  args["title"] = "test clip";
  const std::string req = json_stringify(args);
  agent_string_t out{};
  assert(exec.execute(exec.ctx, "artifact_register", req.c_str(), &out) == AGENT_OK);
  const Json::Value resp = json_parse(std::string(out.data, out.len));
  assert(resp["ok"].asBool());
  assert(resp["data"]["tool"].asString() == "artifact_register");
  assert(resp["data"]["artifact"]["path"].asString() == "hello.wav");
  assert(resp["data"]["artifact"]["kind"].asString() == "audio");
  assert(resp["data"]["artifact"]["autoplay"].asBool());
  assert(resp["data"]["artifact"]["repeat"].asInt() == 2);
  assert(resp["data"]["artifact"]["title"].asString() == "test clip");
  agent_string_free(&out);

  // Path normalization: even if the caller provides a weird-but-contained relative path,
  // the tool should return a stable relative path for WebUI/agentd fetch agreement.
  {
    Json::Value args2(Json::objectValue);
    args2["path"] = "out/../hello.wav";
    const std::string req2 = json_stringify(args2);
    agent_string_t out2{};
    assert(exec.execute(exec.ctx, "artifact_register", req2.c_str(), &out2) == AGENT_OK);
    const Json::Value resp2 = json_parse(std::string(out2.data, out2.len));
    assert(resp2["ok"].asBool());
    assert(resp2["data"]["artifact"]["path"].asString() == "hello.wav");
    agent_string_free(&out2);
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

static void test_memory_tools() {
  const auto root = std::filesystem::temp_directory_path() / ("agent_host_mem_tools_" + std::to_string((long long)getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto sessions_root = root / "sessions";
  std::filesystem::create_directories(sessions_root);

  HostToolsetConfig cfg;
  cfg.root_dir = root.string();
  cfg.sessions_root_dir = sessions_root.string();
  cfg.session_id = "s1";

  agent_tool_registry_t* reg = nullptr;
  agent_tool_executor_t exec{};
  assert(toolset_host_create(cfg, &reg, &exec) == AGENT_OK);
  assert(registry_contains(reg, "memory_write"));
  assert(registry_contains(reg, "memory_search"));
  assert(registry_contains(reg, "memory_get"));

  // Write to core memory.
  {
    Json::Value args(Json::objectValue);
    args["layer"] = "core";
    args["title"] = "prefs";
    args["text"] = "- The user prefers Scene-based, refresh-proof UI rendering.\n";
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_write", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["tool"].asString() == "memory_write");
    assert(resp["data"]["path"].asString() == "MEMORY.md");
    agent_string_free(&out);
  }

  // Search should find the entry.
  {
    Json::Value args(Json::objectValue);
    args["query"] = "refresh-proof";
    args["max_results"] = 5;
    args["daily_days"] = 0; // core only
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_search", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const auto& results = resp["data"]["results"];
    assert(results.isArray());
    assert(results.size() >= 1);
    agent_string_free(&out);
  }

  // Read back.
  {
    Json::Value args(Json::objectValue);
    args["path"] = "MEMORY.md";
    args["from_line"] = 1;
    args["max_lines"] = 200;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_get", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const std::string text = resp["data"]["text"].asString();
    assert(text.find("refresh-proof") != std::string::npos);
    agent_string_free(&out);
  }

  // Consolidation primitive: overwrite MEMORY.md via memory_put.
  {
    Json::Value args(Json::objectValue);
    args["path"] = "MEMORY.md";
    args["text"] = "# Core memory\n\n## Active\n- Scene rendering should be server-owned and refresh-proof.\n\n## Deprecated\n- Feature set A (deprecated; no longer required).\n";
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_put", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["tool"].asString() == "memory_put");
    agent_string_free(&out);
  }

  // Verify overwrite took effect.
  {
    Json::Value args(Json::objectValue);
    args["path"] = "MEMORY.md";
    args["from_line"] = 1;
    args["max_lines"] = 200;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_get", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const std::string text = resp["data"]["text"].asString();
    assert(text.find("## Deprecated") != std::string::npos);
    assert(text.find("Feature set A") != std::string::npos);
    agent_string_free(&out);
  }

  // Structured memory: deterministic key upserts via memory_put(entries).
  {
    Json::Value e0(Json::objectValue);
    e0["key"] = "ui.rendering";
    e0["kind"] = "fact";
    e0["value"] = "Scene is server-owned and refresh-proof";
    Json::Value e1(Json::objectValue);
    e1["key"] = "feature.a";
    e1["kind"] = "fact";
    e1["value"] = "Feature set A";
    e1["status"] = "deprecated";
    Json::Value entries(Json::arrayValue);
    entries.append(e0);
    entries.append(e1);

    Json::Value args(Json::objectValue);
    args["path"] = "STRUCTURED.md";
    args["entries"] = entries;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_put", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    assert(resp["data"]["tool"].asString() == "memory_put");
    assert(resp["data"]["structured"].asBool());
    agent_string_free(&out);
  }

  // Overwrite the same key and ensure last write wins.
  {
    Json::Value e0(Json::objectValue);
    e0["key"] = "ui.rendering";
    e0["kind"] = "fact";
    e0["value"] = "Scene rendering must survive refresh + restart";
    Json::Value entries(Json::arrayValue);
    entries.append(e0);
    Json::Value args(Json::objectValue);
    args["path"] = "STRUCTURED.md";
    args["entries"] = entries;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_put", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    agent_string_free(&out);
  }

  {
    Json::Value args(Json::objectValue);
    args["path"] = "STRUCTURED.md";
    args["from_line"] = 1;
    args["max_lines"] = 300;
    const std::string req = json_stringify(args);
    agent_string_t out{};
    assert(exec.execute(exec.ctx, "memory_get", req.c_str(), &out) == AGENT_OK);
    const Json::Value resp = json_parse(std::string(out.data, out.len));
    assert(resp["ok"].asBool());
    const std::string text = resp["data"]["text"].asString();
    assert(text.find("Structured Memory") != std::string::npos);
    assert(text.find("ui.rendering") != std::string::npos);
    assert(text.find("survive refresh + restart") != std::string::npos);
    assert(text.find("server-owned and refresh-proof") == std::string::npos);
    agent_string_free(&out);
  }

  agent_tool_registry_destroy(reg);
  toolset_host_destroy(&exec);
  std::filesystem::remove_all(root);
}

int main() {
  test_file_apply_patch();
  test_readonly_policy_disables_exec_and_patch();
  test_scoped_mode_disables_exec_tools_but_keeps_patch();
  test_scoped_mode_denies_symlink_escapes();
  test_artifact_register();
  test_memory_tools();
  test_fs_stat_list_read();
  test_shell_exec();
  test_proc_exec();
  test_text_search();
  test_fs_find();
  return 0;
}
