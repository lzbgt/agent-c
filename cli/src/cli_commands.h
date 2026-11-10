#pragma once
#include <string>
#include <vector>

struct CliCommand {
  std::string command;
  std::string session_root;
  bool resume = false;
};
// Handles model-free commands, workspace selection, and exec/resume normalization.
// Returns an exit status for handled commands; -1 continues into provider setup.
int cli_prepare_command(std::vector<std::string>* arguments, CliCommand* out);
