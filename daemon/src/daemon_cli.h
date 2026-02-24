#pragma once

#include <json/json.h>

#include <string>
#include <utility>
#include <vector>

namespace agentd {

struct DaemonConfig;
struct ToolPluginSpec;
struct ToolServerSpec;
struct CorsRouteConfig;

struct DaemonCliOverrides {
  bool help_requested = false;
  bool system_profile_set = false;
  bool workflow_enable_http_tasks_set = false;
  bool workflow_http_allow_hosts_set = false;
  bool workflow_http_allow_cidrs_set = false;
  bool workflow_http_deny_cidrs_set = false;
  bool workflow_http_deny_private_set = false;
  bool workflow_http_dns_pin_set = false;
  bool upload_max_bytes_set = false;
  bool blob_store_set = false;
  bool blob_tier_set = false;
  bool memory_recap_set = false;
  bool memory_retention_set = false;
  bool memory_salience_set = false;
  bool cors_allow_credentials_set = false;
  bool cors_max_age_set = false;
  bool cors_routes_set = false;
  std::vector<ToolPluginSpec> tool_plugin_specs;
  std::vector<ToolServerSpec> tool_server_specs;
};

int parse_daemon_cli(int argc, char** argv, DaemonConfig* cfg, DaemonCliOverrides* out);

bool host_is_loopback(std::string host);
bool parse_tool_call_limits_csv(
  const std::string& csv,
  std::vector<std::pair<std::string, size_t>>* out_limits,
  std::string* out_error
);
void parse_csv_tokens_best_effort(const std::string& csv, std::vector<std::string>* out);
bool parse_cors_route_value(const Json::Value& root, CorsRouteConfig* out, std::string* out_error);
bool parse_cors_route_json(const std::string& raw, CorsRouteConfig* out, std::string* out_error);

}  // namespace agentd
