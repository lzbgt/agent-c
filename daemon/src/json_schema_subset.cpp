#include "json_schema_subset.h"

#include <string>

namespace agentd {
namespace {

bool json_value_type_matches_string(const Json::Value& v, const std::string& type) {
  if (type == "object") return v.isObject();
  if (type == "array") return v.isArray();
  if (type == "string") return v.isString();
  if (type == "boolean") return v.isBool();
  if (type == "integer") return v.isInt() || v.isInt64() || v.isUInt() || v.isUInt64();
  if (type == "number") return v.isNumeric();
  if (type == "null") return v.isNull();
  return true; // unknown type => best-effort allow
}

bool json_schema_subset_validate_best_effort_impl(
  const Json::Value& schema,
  const Json::Value& value,
  const std::string& path,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!schema.isObject()) return true;

  const bool has_object_keywords =
    schema.isMember("properties") || schema.isMember("required") || schema.isMember("additionalProperties");
  const bool has_array_keywords = schema.isMember("items");

  if (schema.isMember("enum") && schema["enum"].isArray() && !schema["enum"].empty()) {
    bool ok = false;
    for (Json::ArrayIndex i = 0; i < schema["enum"].size(); i++) {
      if (schema["enum"][i] == value) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      if (out_error) *out_error = "schema enum mismatch at " + path;
      return false;
    }
  }

  std::string type;
  if (schema.isMember("type") && schema["type"].isString()) {
    type = schema["type"].asString();
    if (!type.empty() && !json_value_type_matches_string(value, type)) {
      if (out_error) *out_error = "schema type mismatch at " + path + " (expected " + type + ")";
      return false;
    }
  }

  const bool is_object_schema = (type == "object") || (!type.empty() ? false : has_object_keywords);
  if (is_object_schema) {
    if (!value.isObject()) {
      if (out_error) *out_error = "schema type mismatch at " + path + " (expected object)";
      return false;
    }

    if (schema.isMember("required") && schema["required"].isArray()) {
      for (Json::ArrayIndex i = 0; i < schema["required"].size(); i++) {
        if (!schema["required"][i].isString()) continue;
        const std::string k = schema["required"][i].asString();
        if (k.empty()) continue;
        if (!value.isMember(k)) {
          if (out_error) *out_error = "missing required property at " + path + ": " + k;
          return false;
        }
      }
    }

    const bool additional_props_allowed =
      !schema.isMember("additionalProperties") ||
      (!schema["additionalProperties"].isBool() || schema["additionalProperties"].asBool());

    const Json::Value props =
      schema.isMember("properties") && schema["properties"].isObject() ? schema["properties"] : Json::Value(Json::nullValue);

    if (!additional_props_allowed) {
      for (const auto& k : value.getMemberNames()) {
        if (!props.isObject() || !props.isMember(k)) {
          if (out_error) *out_error = "unknown property at " + path + ": " + k;
          return false;
        }
      }
    }

    if (props.isObject()) {
      for (const auto& k : value.getMemberNames()) {
        if (!props.isMember(k) || !props[k].isObject()) continue;
        std::string err;
        const std::string child_path = path.empty() ? k : (path + "." + k);
        if (!json_schema_subset_validate_best_effort_impl(props[k], value[k], child_path, &err)) {
          if (out_error) *out_error = err;
          return false;
        }
      }
    }
  }

  const bool is_array_schema = (type == "array") || (!type.empty() ? false : has_array_keywords);
  if (is_array_schema) {
    if (!value.isArray()) {
      if (out_error) *out_error = "schema type mismatch at " + path + " (expected array)";
      return false;
    }
    if (schema.isMember("items") && schema["items"].isObject()) {
      const Json::Value items = schema["items"];
      for (Json::ArrayIndex i = 0; i < value.size(); i++) {
        std::string err;
        const std::string child_path = path + "[" + std::to_string((int)i) + "]";
        if (!json_schema_subset_validate_best_effort_impl(items, value[i], child_path, &err)) {
          if (out_error) *out_error = err;
          return false;
        }
      }
    }
  }

  return true;
}

}  // namespace

bool json_schema_subset_validate_best_effort(
  const Json::Value& schema,
  const Json::Value& value,
  const std::string& path,
  std::string* out_error
) {
  return json_schema_subset_validate_best_effort_impl(schema, value, path, out_error);
}

}  // namespace agentd
