#include "workflow_sop_registry.h"
#include "agentd/api.h"
#include "workflow_sop.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <thread>

namespace {
void check(bool ok, const std::string& message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
Json::Value definition(const std::string& version = "1") {
  Json::Value sop;
  sop["schema"] = "agentd.sop.v1"; sop["id"] = "inspection"; sop["version"] = version;
  sop["stages"][0]["id"] = "check";
  auto& action = sop["stages"][0]["actions"][0];
  action["task_id"] = "wait"; action["kind"] = "delay"; action["delay_ms"] = 0;
  return sop;
}
agentd::AgentDb::WorkflowSopRow row(const Json::Value& sop) {
  Json::Value args; args["sop"] = sop; std::string error;
  check(agentd::workflow_compile_sop(&args, &error), error);
  Json::StreamWriterBuilder w; w["indentation"] = "";
  agentd::AgentDb::WorkflowSopRow r;
  r.definition_id = sop["id"].asString(); r.version = sop["version"].asString();
  r.definition_hash = args["sop_compiled"]["definition_hash"].asString();
  r.sop_json = Json::writeString(w, sop); return r;
}
}
int main() {
  using namespace agentd;
  const auto root = std::filesystem::temp_directory_path() / ("agent-sop-registry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.sqlite").string();
  AgentDb db; std::string error;
  check(db.open(path, &error), error);
  db.close();
  // Real migration from the previous schema, preserving an existing table.
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open migration fixture");
  check(sqlite3_exec(raw, "DROP TABLE workflow_sop_definitions; UPDATE meta SET value='36' WHERE key='schema_version';", nullptr, nullptr, nullptr) == SQLITE_OK, "v36 fixture");
  sqlite3_close(raw);
  check(db.open(path, &error), error);
  const auto original = row(definition());
  AgentDb::WorkflowSopRow saved; bool created = false;
  check(db.create_workflow_sop(original, &saved, &created, &error) && created, error);
  const auto original_id = saved.registry_id;
  check(db.create_workflow_sop(original, &saved, &created, &error) && !created && saved.registry_id == original_id, "same version must dedupe");
  auto changed = definition(); changed["title"] = "changed";
  check(db.create_workflow_sop(row(changed), &saved, &created, &error) && !created &&
    saved.definition_hash == original.definition_hash && saved.sop_json == original.sop_json, "existing definition must never be overwritten");

  // Independent DB connections race on the same immutable identity.
  AgentDb other; check(other.open(path, &error), error);
  std::atomic<int> inserted{0};
  const auto concurrent = row(definition("2"));
  auto save = [&](AgentDb& target) {
    AgentDb::WorkflowSopRow result; std::string e; bool is_new = false;
    check(target.create_workflow_sop(concurrent, &result, &is_new, &e), e);
    check(result.definition_hash == concurrent.definition_hash, "concurrent save changed content");
    if (is_new) inserted++;
  };
  std::thread a(save, std::ref(db)), b(save, std::ref(other)); a.join(); b.join();
  check(inserted == 1, "concurrent saves must create one version");
  other.close();
  std::vector<AgentDb::WorkflowSopRow> page;
  check(db.list_workflow_sops(0, 1, &page, &error) && page.size() == 1 && page[0].version == "2" && page[0].sop_json.empty(), "metadata page leaked body or wrong order");
  check(db.list_workflow_sops(page[0].registry_id, 1, &page, &error) && page.size() == 1 && page[0].version == "1", "exclusive keyset skipped or duplicated");
  check(db.list_workflow_sops(original_id, 1, &page, &error) && page.empty(), "last page not empty");
  check(!db.list_workflow_sops(-1, 1, &page, &error) && !error.empty(), "invalid cursor accepted");
  check(!db.get_workflow_sop("unknown", "1", &saved, &error) && error.empty(), "not-found must differ from storage errors");
  db.close(); check(db.open(path, &error), error);

  Json::Value args;
  args["sop_ref"]["id"] = "inspection"; args["sop_ref"]["version"] = "1";
  const auto request = args;
  check(workflow_resolve_sop_reference(&db, &args, &error) == 0 && args["sop"] == definition() &&
    args["sop_ref"]["definition_hash"] == original.definition_hash, "restart lost exact version");
  check(workflow_compile_sop(&args, &error) && args["tasks"].size() == 1, "resolved source cannot compile");
  auto bad = request; bad["sop_ref"]["definition_hash"] = "sha256:wrong";
  const auto unchanged = bad;
  check(workflow_resolve_sop_reference(&db, &bad, &error) == 409 && bad == unchanged, "hash conflict mutated request");
  bad = request; bad["tasks"] = Json::Value(Json::arrayValue);
  check(workflow_resolve_sop_reference(&db, &bad, &error) == 400, "mixed task/reference accepted");
  bad = request; bad["sop_ref"]["version"] = "latest";
  check(workflow_resolve_sop_reference(&db, &bad, &error) == 404, "reference resolved implicit latest");
  bad = request; bad["sop_ref"]["overwrite"] = true;
  check(workflow_resolve_sop_reference(&db, &bad, &error) == 400, "unknown reference control ignored");
  check(workflow_resolve_sop_reference(nullptr, &args, &error) == 400, "compiled source resubmission should not bypass validation");
  bad = request; check(workflow_resolve_sop_reference(nullptr, &bad, &error) == 503, "missing store not reported");
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open corruption fixture");
  check(sqlite3_exec(raw, "UPDATE workflow_sop_definitions SET sop_json='{}' WHERE version='1';", nullptr, nullptr, nullptr) == SQLITE_OK, "corrupt fixture");
  sqlite3_close(raw);
  bad = request;
  check(workflow_resolve_sop_reference(&db, &bad, &error) == 500 && bad == request, "corrupt saved source must not run");
  db.close();
  {
    AgentdApi::Options options;
    options.cfg.db_path = (root / "facade.sqlite").string();
    options.cfg.state_dir = (root / "facade-state").string();
    options.cfg.auth_token = "registry-facade-test";
    AgentdApi api(options);
    check(api.init(&error), error);
    HttpRequest req; req.method = "GET"; req.path = "/api/v1/workflow/sops";
    HttpResponse resp;
    check(api.handle(req, &resp) && resp.status == 401, "facade registry route must require auth");
    req.headers["authorization"] = "Bearer registry-facade-test";
    resp = {};
    check(api.handle(req, &resp) && resp.status == 200 && resp.body.find("\"definitions\":[]") != std::string::npos, "facade registry list route missing: " + std::to_string(resp.status) + " " + resp.body);
    req.method = "POST"; req.body = "{\"sop\":" + original.sop_json + "}";
    resp = {};
    check(api.handle(req, &resp) && resp.status == 201, "facade registry save route missing");
    resp = {};
    check(api.handle(req, &resp) && resp.status == 200, "facade registry save not idempotent");
  }
  std::filesystem::remove_all(root);
  std::cout << "SOP registry migration, immutability, race, paging, restart and integrity checks passed\n";
}
