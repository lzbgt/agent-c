#include "health_endpoint.h"

#include "agent_db.h"
#include "json_util.h"

#include <chrono>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count();
}

int64_t uptime_ms(std::chrono::steady_clock::time_point start_time) {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now() - start_time)
           .count();
}

Json::Value build_base_health(std::chrono::steady_clock::time_point start_time) {
  Json::Value root(Json::objectValue);
  root["ok"] = true;
  root["service"] = "agentd";
  root["version"] = "0.1";
  root["now_unix_ms"] = Json::Int64(now_unix_ms());
  root["uptime_ms"] = Json::Int64(uptime_ms(start_time));
  return root;
}

}  // namespace

std::string build_health_body(std::chrono::steady_clock::time_point start_time) {
  return json_stringify(build_base_health(start_time));
}

std::string build_ready_body(std::chrono::steady_clock::time_point start_time, const AgentDb* db) {
  Json::Value root = build_base_health(start_time);
  const bool db_open = db && db->is_open();
  root["ready"] = db_open;
  Json::Value checks(Json::objectValue);
  checks["db_open"] = db_open;
  root["checks"] = checks;
  return json_stringify(root);
}

}  // namespace agentd
