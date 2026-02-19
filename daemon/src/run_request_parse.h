#pragma once

#include <json/json.h>

#include <string>

namespace agentd {

struct DaemonConfig;

struct RunRequestParseResult {
  Json::Value args;
  std::string prompt;
  std::string trace_id;
  std::string tools;
};

bool parse_run_request_base(
  const DaemonConfig& daemon_cfg,
  const std::string& request_body,
  RunRequestParseResult* out,
  Json::Value* error_out
);

}  // namespace agentd
