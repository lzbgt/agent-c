#include "json_schema_subset.h"

#include <json/json.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

Json::Value parse_json(const std::string& text) {
  Json::CharReaderBuilder rb;
  Json::Value out;
  std::string errs;
  const std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
  const bool ok = reader->parse(text.data(), text.data() + text.size(), &out, &errs);
  if (!ok) {
    std::cerr << "failed to parse fixture JSON: " << errs << "\n";
    std::abort();
  }
  return out;
}

bool validate(const Json::Value& schema, const Json::Value& value, std::string* out_err = nullptr) {
  return agentd::json_schema_subset_validate_best_effort(schema, value, "$", out_err);
}

void test_object_schema_accepts_supported_contract() {
  const Json::Value schema = parse_json(R"JSON({
    "type": "object",
    "required": ["assistant_text", "usage"],
    "additionalProperties": false,
    "properties": {
      "assistant_text": {"type": "string", "enum": ["OK"]},
      "usage": {
        "type": "object",
        "required": ["total_tokens"],
        "properties": {"total_tokens": {"type": "integer"}}
      },
      "tags": {"type": "array", "items": {"type": "string"}}
    },
    "description": "ignored unsupported keyword"
  })JSON");
  const Json::Value value = parse_json(R"JSON({
    "assistant_text": "OK",
    "usage": {"total_tokens": 7},
    "tags": ["green", "deterministic"]
  })JSON");
  std::string err;
  assert(validate(schema, value, &err));
  assert(err.empty());
}

void test_required_and_type_fail_closed() {
  const Json::Value schema = parse_json(R"JSON({
    "type": "object",
    "required": ["ok"],
    "properties": {"ok": {"type": "boolean"}}
  })JSON");
  std::string err;
  assert(!validate(schema, parse_json(R"JSON({"ok": "true"})JSON"), &err));
  assert(err.find("expected boolean") != std::string::npos);
  assert(!validate(schema, parse_json(R"JSON({})JSON"), &err));
  assert(err.find("missing required property") != std::string::npos);
}

void test_additional_properties_and_array_items_fail_closed() {
  const Json::Value no_extra_schema = parse_json(R"JSON({
    "type": "object",
    "additionalProperties": false,
    "properties": {"id": {"type": "string"}}
  })JSON");
  std::string err;
  assert(!validate(no_extra_schema, parse_json(R"JSON({"id": "a", "extra": true})JSON"), &err));
  assert(err.find("unknown property") != std::string::npos);

  const Json::Value array_schema = parse_json(R"JSON({"type": "array", "items": {"type": "integer"}})JSON");
  assert(!validate(array_schema, parse_json(R"JSON([1, "2"])JSON"), &err));
  assert(err.find("expected integer") != std::string::npos);
}

void test_unknown_type_and_non_object_schema_remain_best_effort() {
  std::string err;
  assert(validate(parse_json(R"JSON({"type": "future-type"})JSON"), parse_json(R"JSON({"anything": true})JSON"), &err));
  assert(validate(parse_json(R"JSON("not-a-schema")JSON"), parse_json(R"JSON(null)JSON"), &err));
}

}  // namespace

int main() {
  test_object_schema_accepts_supported_contract();
  test_required_and_type_fail_closed();
  test_additional_properties_and_array_items_fail_closed();
  test_unknown_type_and_non_object_schema_remain_best_effort();
  std::cout << "json_schema_subset_tests OK\n";
  return 0;
}
