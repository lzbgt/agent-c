#include "work_ledger.h"
#include "toolset_host.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <future>
#include <thread>

namespace {
void check(bool ok, const std::string& error) { if (!ok) { std::cerr << error << '\n'; std::exit(1); } }
Json::Value parse(const std::string& text) { Json::Value v; std::istringstream s(text); s >> v; return v; }
}
int main() {
  const auto root = std::filesystem::temp_directory_path() / ("agent-work-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto db = root / "work.sqlite";
  Json::Value out; std::string error;
  auto apply = [&](const std::string& text) { check(work_ledger_apply(db, parse(text), &out, &error), error); };
  auto reject = [&](const std::string& text) { check(!work_ledger_apply(db, parse(text), &out, &error) && !error.empty(), "invalid operation accepted"); };
  apply("{}"); check(!std::filesystem::exists(root), "read must not create storage");
  reject(R"({"expected_revision":2})");
  apply(R"({"op":"requirement","id":"production","title":"Deploy to production"})");
  const auto ledger_id = out["state"]["ledger_id"].asString();
  check(ledger_id.size() == 37 && ledger_id.substr(0, 5) == "work_", "mutation must persist a stable ledger identity");
  apply("{}"); check(out["state"]["ledger_id"] == ledger_id, "read/reopen must preserve ledger identity");
  apply(R"({"op":"requirement","id":"docs","title":"Update documentation"})");
  apply(R"({"op":"requirement","id":"handoff","title":"Verify deployment","depends_on":["production"]})");
  apply(R"({"op":"raise_blocker","id":"credential","reason":"Account lacks deployment access","required_action":"Grant the deployment role","attempts":["Existing credential returns access denied","Alternate authorized account has no role"],"requirement_ids":["production"]})");
  check(out["can_continue"].asBool() && out["runnable"].size() == 1 && out["runnable"][0] == "docs", "blocker must leave independent work runnable");
  reject(R"({"op":"requirement","id":"production","status":"done","evidence":"claimed"})");
  reject(R"({"op":"requirement","id":"docs","depends_on":["handoff"] ,"expected_revision":1})");
  reject(R"({"op":"requirement","id":"production","depends_on":["handoff"]})");
  reject(R"({"op":"requirement","id":"docs","status":"done"})");
  reject(R"({"op":"resolve_blocker","id":"credential"})");
  reject(R"({"op":"raise_blocker","id":"bad","reason":"a","required_action":"b","attempts":[],"requirement_ids":[]})");
  apply("{}"); check(out["state"]["revision"].asInt64() == 4, "failed mutations must roll back");
  apply(R"({"op":"requirement","id":"docs","status":"done","evidence":"Documentation build passes","expected_revision":4})");
  apply(R"({"op":"resolve_blocker","id":"credential","evidence":"Deployment API now accepts a read-only access check"})");
  check(out["runnable"].size() == 1 && out["runnable"][0] == "production", "resolution must make affected work runnable");
  apply(R"({"op":"requirement","id":"production","status":"done","evidence":"Deployment health check passes"})");
  check(out["runnable"].size() == 1 && out["runnable"][0] == "handoff", "dependencies must become runnable");
  apply(R"({"op":"requirement","id":"handoff","status":"done","evidence":"Operator handoff verified"})");
  check(out["all_requirements_done"].asBool() && !out["can_continue"].asBool(), "completion must require every requirement");
  check(out["state"]["ledger_id"] == ledger_id, "accepted mutations must retain identity");
  Json::Value other;
  check(work_ledger_apply(root / "other.sqlite", parse(R"({"op":"requirement","id":"x","title":"other"})"), &other, &error), error);
  check(other["state"]["ledger_id"] != ledger_id, "independent ledgers must not share identity");
  reject(R"({"op":"requirement","id":"production","status":"pending"})");
  // The same implementation is exposed to the model through the normal host tool boundary.
  HostToolsetConfig config; config.root_dir = root.string(); config.policy = HostToolsetPolicyMode::ReadOnly;
  agent_tool_registry_t* registry = nullptr; agent_tool_executor_t executor{};
  check(toolset_host_create(config, &registry, &executor) == AGENT_OK, "cannot create host tools");
  agent_string_t result{};
  check(executor.execute(executor.ctx, "work_state", R"({"op":"requirement","id":"x","title":"x"})", &result) == AGENT_OK, "tool execution failed");
  check(parse(std::string(result.data, result.len))["ok"] == false, "readonly policy must deny work mutations");
  agent_string_free(&result); toolset_host_destroy(&executor); agent_tool_registry_destroy(registry);

  // Change hints are per-path, commit-only, and cannot miss a commit before wait.
  auto watch = work_ledger_subscribe(db);
  auto same = work_ledger_subscribe(db.parent_path() / "." / db.filename());
  auto version = watch->generation();
  apply("{}");
  reject(R"({"op":"requirement","id":"new","title":"new","expected_revision":0})");
  check(watch->generation() == version, "reads/failed transactions must not notify");
  Json::Value unrelated;
  check(work_ledger_apply(root / "other.sqlite", parse(R"({"op":"requirement","id":"y","title":"other mutation"})"), &unrelated, &error), error);
  check(watch->generation() == version, "unrelated authority woke watcher");
  apply(R"({"op":"requirement","id":"new","title":"Additional work"})");
  check(same->wait_for(version, std::chrono::milliseconds(1)), "commit between snapshot and wait was lost");
  version = watch->generation();
  auto waiting = std::async(std::launch::async, [&] { return watch->wait_for(version, std::chrono::seconds(2)); });
  apply(R"({"op":"requirement","id":"new","status":"done","evidence":"verified"})");
  check(waiting.get(), "committed mutation did not wake subscriber");
  version = watch->generation();
  check(!watch->wait_for(version, std::chrono::milliseconds(10)), "unchanged ledger spuriously signaled");
  auto absent = work_ledger_subscribe(root / "uncreated" / "work.sqlite");
  check(!std::filesystem::exists(root / "uncreated"), "subscription created ledger storage");
  std::filesystem::remove_all(root);
}
