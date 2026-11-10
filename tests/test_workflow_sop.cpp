#include "workflow_sop.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {
void check(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
Json::Value example() {
  std::istringstream text(R"({"sop":{"schema":"agentd.sop.v1","id":"inspection","version":"1","stages":[
    {"id":"intake","actions":[{"task_id":"a","kind":"delay"},{"task_id":"b","kind":"delay"}]},
    {"id":"inspect","mode":"parallel","actions":[{"task_id":"c","kind":"delay"},{"task_id":"d","kind":"delay"}]},
    {"id":"release","actions":[{"task_id":"e","kind":"delay"}]}
  ]}})");
  Json::Value value;
  text >> value;
  return value;
}
void rejects(Json::Value value) {
  const auto before = value;
  std::string error;
  check(!agentd::workflow_compile_sop(&value, &error), "invalid definition accepted");
  check(!error.empty() && value == before, "failure must preserve input and explain why");
}
}

int main() {
  auto value = example();
  std::string error;
  check(agentd::workflow_compile_sop(&value, &error), error.c_str());
  const auto& tasks = value["tasks"];
  check(tasks.size() == 5 && tasks[0]["depends_on"].empty(), "first action has no dependencies");
  check(tasks[1]["depends_on"][0] == "a", "sequence must wait on predecessor");
  check(tasks[2]["depends_on"][0] == "b" && tasks[3]["depends_on"][0] == "b", "parallel stage must wait on previous stage");
  check(tasks[4]["depends_on"].size() == 2 && tasks[4]["depends_on"][0] == "c" && tasks[4]["depends_on"][1] == "d", "next stage joins every parallel action");
  const auto manifest = value["sop_compiled"];
  check(manifest["definition_hash"].asString().size() == 71, "definition must be hashed");
  auto same = example();
  check(agentd::workflow_compile_sop(&same, &error) && same["sop_compiled"] == manifest, "compilation must be deterministic");
  auto changed = example();
  changed["sop"]["version"] = "2";
  check(agentd::workflow_compile_sop(&changed, &error) && changed["sop_compiled"]["definition_hash"] != manifest["definition_hash"], "definition changes must change digest");
  auto bad = example(); bad["tasks"] = Json::Value(Json::arrayValue); rejects(bad);
  bad = example(); bad["sop"]["stages"][1]["actions"][0]["task_id"] = "a"; rejects(bad);
  bad = example(); bad["sop"]["stages"][1]["id"] = "intake"; rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["mode"] = "paralell"; rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["approval"] = true; rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["actions"][0]["depends_on"] = Json::Value(Json::arrayValue); rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["actions"][0]["approval"] = true; rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["actions"][0]["allow_error"] = true; rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["actions"][0]["kind"] = "edge_parallel"; rejects(bad);
  bad = example(); bad["infer_depends_on"] = true; rejects(bad);
  bad = example(); bad["sop"]["schema"] = "agentd.sop.v2"; rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["actions"] = Json::Value(Json::arrayValue); rejects(bad);
  bad = example(); bad["sop"]["stages"][0]["actions"] = Json::Value(Json::arrayValue);
  for (int i = 0; i < 129; ++i) { Json::Value t; t["task_id"] = "a" + std::to_string(i); bad["sop"]["stages"][0]["actions"].append(t); }
  rejects(bad);
  Json::Value legacy(Json::objectValue); legacy["tasks"] = tasks;
  check(agentd::workflow_compile_sop(&legacy, &error) && !legacy.isMember("sop_compiled"), "legacy submissions pass through");
}
