#include "health_endpoint.h"

#include "agent_db.h"
#include "json_util.h"

#include <chrono>
#include <sstream>

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

int64_t uptime_seconds(std::chrono::steady_clock::time_point start_time) {
  return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
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

std::string build_metrics_body(std::chrono::steady_clock::time_point start_time, const AgentDb* db) {
  const bool db_open = db && db->is_open();
  std::ostringstream out;
  out << "# HELP agentd_up 1 if agentd is running.\n";
  out << "# TYPE agentd_up gauge\n";
  out << "agentd_up 1\n";
  out << "# HELP agentd_ready 1 if agentd is ready to serve traffic.\n";
  out << "# TYPE agentd_ready gauge\n";
  out << "agentd_ready " << (db_open ? 1 : 0) << "\n";
  out << "# HELP agentd_db_open 1 if the SQLite DB is open.\n";
  out << "# TYPE agentd_db_open gauge\n";
  out << "agentd_db_open " << (db_open ? 1 : 0) << "\n";
  out << "# HELP agentd_uptime_seconds Process uptime in seconds.\n";
  out << "# TYPE agentd_uptime_seconds gauge\n";
  out << "agentd_uptime_seconds " << uptime_seconds(start_time) << "\n";
  out << "# HELP agentd_now_unix_ms Current unix time in milliseconds.\n";
  out << "# TYPE agentd_now_unix_ms gauge\n";
  out << "agentd_now_unix_ms " << now_unix_ms() << "\n";
  return out.str();
}

}  // namespace agentd
