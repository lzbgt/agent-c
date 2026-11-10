#include "cli_commands.h"
#include "work_ledger.h"
#include "session_store.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

namespace {
std::string command_home() {
  if (const char* home = std::getenv("HOME"); home && home[0]) return home;
#if !defined(_WIN32)
  if (const auto* pw = getpwuid(getuid()); pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
#endif
  return std::filesystem::current_path().string();
}
bool take_flag(std::vector<std::string>& args, const std::string& flag, std::string* value) {
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] != flag) continue;
    if (i + 1 == args.size() || args[i + 1].empty()) return false;
    *value = args[i + 1];
    args.erase(args.begin() + i, args.begin() + i + 2);
    return true;
  }
  return true;
}
}

int cli_prepare_command(std::vector<std::string>* arguments, CliCommand* out) {
  auto& args = *arguments;
  std::string cmd = args[0];
  args.erase(args.begin());
  std::string work_dir;
  if (!take_flag(args, "--cd", &work_dir) || !take_flag(args, "-C", &work_dir)) {
    std::cerr << "Missing working directory\n"; return 2;
  }
  if (!work_dir.empty()) {
    std::error_code ec;
    std::filesystem::current_path(work_dir, ec);
    if (ec) { std::cerr << "Cannot use working directory: " << ec.message() << "\n"; return 2; }
  }
  std::string session_root = (std::filesystem::path(command_home()) / ".agent" / "sessions").string();
  if (!take_flag(args, "--session-root", &session_root) || session_root.empty()) {
    std::cerr << "Missing session root directory\n"; return 2;
  }
  if (cmd == "sessions") {
    if (!args.empty()) { std::cerr << "Unexpected sessions argument\n"; return 2; }
    SessionStoreConfig store;
    store.root_dir = session_root;
    std::vector<std::string> ids;
    if (session_store_list(store, &ids) != AGENT_OK) return 1;
    std::sort(ids.begin(), ids.end());
    for (const auto& id : ids) std::cout << id << '\n';
    return 0;
  }
  if (cmd == "work") {
    std::string file = ".agent/work.sqlite";
    if (!take_flag(args, "--file", &file) || args.size() > 1) {
      std::cerr << "Usage: agent work [JSON|-] [--file path]\n"; return 2;
    }
    std::string input = args.empty() ? "{}" : args.front();
    if (input == "-") {
      input.clear(); char ch;
      while (std::cin.get(ch)) {
        if (input.size() >= 64 * 1024) { std::cerr << "Work request exceeds 64 KiB\n"; return 2; }
        input.push_back(ch);
      }
    }
    Json::Value request, response;
    Json::CharReaderBuilder builder;
    std::istringstream stream(input);
    std::string error;
    if (!Json::parseFromStream(builder, stream, &request, &error) ||
        !work_ledger_apply(file, request, &response, &error)) {
      response = Json::Value(Json::objectValue); response["ok"] = false; response["error"] = error;
    }
    Json::StreamWriterBuilder writer; writer["indentation"] = "";
    std::cout << Json::writeString(writer, response) << '\n';
    return response["ok"].asBool() ? 0 : 1;
  }
  const bool resume = cmd == "resume";
  if (resume) {
    if (args.empty() || args[0].empty() || args[0][0] == '-' ||
        std::find(args.begin(), args.end(), "--session") != args.end() ||
        std::find(args.begin(), args.end(), "--no-session") != args.end()) {
      std::cerr << "resume requires a session ID; --session and --no-session are incompatible\n"; return 2;
    }
    const std::string id = args.front(); args.erase(args.begin());
    args.insert(args.end(), {"--session", id});
  }
  if (cmd == "exec") cmd = "run";
  out->command = cmd;
  out->resume = resume;
  out->session_root = session_root;
  return -1;
}
