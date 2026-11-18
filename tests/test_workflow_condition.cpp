#include "workflow_condition.h"
#include "workflow_sop.h"

#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
Json::Value condition(Json::Value equals = Json::Value(true), const char* path = "/payload/accepted") {
  Json::Value value; value["task_id"] = "source"; value["pointer"] = path; value["equals"] = equals;
  return value;
}
}

int main() {
  using namespace agentd;
  std::string error;
  auto spec = condition();
  check(workflow_condition_validate(spec, &error), "valid condition rejected");
  for (const auto& value : {Json::Value(1), Json::Value(1.5), Json::Value(Json::arrayValue), Json::Value(Json::objectValue)}) {
    auto bad = condition(value);
    check(!workflow_condition_validate(bad, &error) && !error.empty(), "unbounded/coerced value accepted");
  }
  for (const auto* path : {"", "payload/accepted", "/bad~", "/bad~2", "/bad~/"}) {
    auto bad = condition(true, path);
    check(!workflow_condition_validate(bad, &error), "invalid pointer accepted");
  }
  auto bad = spec; bad["equals"] = std::string(1025, 'x');
  check(!workflow_condition_validate(bad, &error), "oversized comparison");
  bad = spec; bad["equals"] = std::string("a\0b", 3);
  check(!workflow_condition_validate(bad, &error), "NUL comparison");
  bad = spec; bad["actor"] = "owner";
  check(!workflow_condition_validate(bad, &error), "unknown field accepted");
  bad = spec; bad.removeMember("equals");
  check(!workflow_condition_validate(bad, &error), "missing equals accepted as null");
  bad = spec; bad["pointer"] = std::string(513, '/');
  check(!workflow_condition_validate(bad, &error), "pointer depth/length unbounded");

  Json::Value result; result["ok"] = true; result["payload"]["accepted"] = true;
  std::unordered_map<std::string, Json::Value> sources{{"source", result}};
  Json::Value receipt;
  check(workflow_condition_evaluate(spec, sources, &receipt).isNull() && receipt["matched"] == true, "true must proceed");
  auto skipped = workflow_condition_evaluate(condition(false), sources, &receipt);
  check(skipped["ok"] == true && skipped["skipped"] == true && receipt["actual"] == true &&
        receipt["matched"] == false && skipped["elapsed_ms"] == 0 && skipped["total_tokens"] == 0, "false must have zero-cost skip");
  auto failed = workflow_condition_evaluate(condition("true"), sources, &receipt);
  check(failed["ok"] == false && failed["retryable"] == false && !receipt.isMember("matched"), "type mismatch must fail");
  failed = workflow_condition_evaluate(condition(true, "/missing"), sources, &receipt);
  check(failed["ok"] == false && failed["skipped"] == false, "missing pointer must fail rather than skip");
  check(workflow_condition_evaluate(spec, {}, &receipt)["ok"] == false, "missing source must fail");
  sources["source"]["payload"]["a/b"]["~key"] = "literal ${task.missing.json:/value}";
  auto literal = condition("literal ${task.missing.json:/value}", "/payload/a~1b/~0key");
  check(workflow_condition_evaluate(literal, sources, &receipt).isNull(), "pointer escaping or literal equality failed");
  sources["source"]["items"] = Json::Value(Json::arrayValue);
  sources["source"]["items"].append(true);
  check(workflow_condition_evaluate(condition(true, "/items/0"), sources, &receipt).isNull(), "array index failed");
  for (const auto* path : {"/items/00", "/items/+0", "/items/-0", "/items/4294967296", "/items/999999999999999999999999999"}) {
    check(workflow_condition_evaluate(condition(true, path), sources, &receipt)["ok"] == false, "noncanonical or overflowing index accepted");
  }
  sources["source"]["nothing"] = Json::Value();
  check(workflow_condition_evaluate(condition(Json::Value(), "/nothing"), sources, &receipt).isNull(), "explicit null equality failed");

  Json::Value task; task["task_id"] = "guarded"; task["when"] = spec;
  Json::Value request, deps(Json::arrayValue);
  std::vector<std::string> ids;
  check(workflow_condition_attach(task, "guarded", &request, &deps, &ids, &error) &&
        deps.size() == 1 && deps[0] == "source" && request["when"] == spec, "condition must create dependency");
  check(workflow_condition_attach(task, "guarded", &request, &deps, &ids, &error) && deps.size() == 1, "dependency duplicate");
  task["allow_error"] = true;
  check(!workflow_condition_attach(task, "guarded", &request, &deps, &ids, &error), "allow_error bypass accepted");
  task.removeMember("allow_error"); task["request"]["when"] = spec;
  check(!workflow_condition_attach(task, "guarded", &request, &deps, &ids, &error), "nested guard accepted");
  task.removeMember("request");
  check(!workflow_condition_attach(task, "source", &request, &deps, &ids, &error), "self dependency accepted");

  skipped = workflow_condition_evaluate(condition(false), sources, &receipt);
  skipped["condition"] = receipt;
  request["when"] = condition(false);
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  Json::Value projected;
  workflow_condition_project(Json::writeString(writer, request), skipped, &projected);
  check(projected["skipped"] == true && projected["condition"]["matched"] == false &&
        !projected["condition"].isMember("actual") && !projected["condition"].isMember("equals"), "metadata must not copy values");
  projected = Json::Value(); request["when"]["equals"] = true;
  workflow_condition_project(Json::writeString(writer, request), skipped, &projected);
  check(projected.isNull(), "mismatched receipt projected");

  Json::Value sop;
  sop["sop"]["schema"] = "agentd.sop.v1"; sop["sop"]["id"] = "conditional"; sop["sop"]["version"] = "1";
  Json::Value stage; stage["id"] = "inspect"; stage["repeat_count"] = 2;
  Json::Value source; source["task_id"] = "source"; source["kind"] = "delay";
  stage["actions"].append(source);
  Json::Value action; action["task_id"] = "optional"; action["kind"] = "delay"; action["when"] = spec;
  stage["actions"].append(action); sop["sop"]["stages"].append(stage);
  auto original = sop;
  check(workflow_compile_sop(&sop, &error), error.c_str());
  check(sop["tasks"][1]["when"]["task_id"] == "source:repeat:1" &&
        sop["tasks"][3]["when"]["task_id"] == "source:repeat:2", "SOP guard must map exact current iteration");
  check(sop["tasks"][1]["depends_on"].size() == 1, "guard duplicate compiler dependency");
  auto again = original; check(workflow_compile_sop(&again, &error) && sop == again, "conditional compile nondeterministic");
  bad = original; bad["sop"]["stages"][0]["mode"] = "parallel";
  const auto before = bad;
  check(!workflow_compile_sop(&bad, &error) && bad == before, "parallel same-stage guard accepted or mutated failure");
  bad = original; bad["sop"]["stages"][0]["actions"][0]["when"] = spec;
  check(!workflow_compile_sop(&bad, &error), "first action self guard accepted");
  std::cout << "PASS bounded conditional actions and repeated SOP identity\n";
}
