#include "runtime_skill_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_endpoint_util.h"

#include "agent_sha256.h"

#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace agentd {
namespace {

struct RuntimeSkillEntry {
  std::filesystem::path root;
  std::filesystem::path manifest_path;
  Json::Value manifest;
};

static const std::set<std::string> kAllowedKinds = {
  "instruction_pack",
  "workflow_bundle",
  "team_bundle",
  "policy_bundle",
};

static const std::set<std::string> kAllowedKeys = {
  "skill_id",
  "version",
  "description",
  "kind",
  "requires",
  "inputs_schema",
  "instruction_fragments",
  "workflow_template",
  "team_template",
  "policy_preset",
  "ui",
};

static const std::set<std::string> kAllowedRequiresKeys = {
  "tools",
  "plugins",
  "features",
};

static const std::unordered_set<std::string> kIgnoredDiscoveryDirs = {
  "__pycache__",
  "templates",
};

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static void append_canonical_json(const Json::Value& v, std::string* out) {
  if (!out) return;
  if (v.isObject()) {
    *out += "{";
    Json::Value::Members names = v.getMemberNames();
    std::sort(names.begin(), names.end());
    bool first = true;
    for (const auto& name : names) {
      if (!first) *out += ",";
      first = false;
      Json::Value key(name);
      *out += json_stringify_compact(key);
      *out += ":";
      append_canonical_json(v[name], out);
    }
    *out += "}";
    return;
  }
  if (v.isArray()) {
    *out += "[";
    for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
      if (i > 0) *out += ",";
      append_canonical_json(v[i], out);
    }
    *out += "]";
    return;
  }
  *out += json_stringify_compact(v);
}

static std::string manifest_hash_hex(const Json::Value& manifest) {
  std::string canonical;
  append_canonical_json(manifest, &canonical);
  char hex[65];
  agent_sha256_hex_of_bytes(canonical.data(), canonical.size(), hex);
  return std::string(hex);
}

static bool is_runtime_skill_id_safe(const std::string& s) {
  if (s.empty()) return false;
  bool expect_alnum = true;
  for (const char c : s) {
    const bool is_lower = c >= 'a' && c <= 'z';
    const bool is_digit = c >= '0' && c <= '9';
    if (expect_alnum) {
      if (!is_lower && !is_digit) return false;
      expect_alnum = false;
      continue;
    }
    if (is_lower || is_digit) continue;
    if (c == '-' || c == '_') {
      expect_alnum = true;
      continue;
    }
    return false;
  }
  return !expect_alnum;
}

static bool is_semver_like(const std::string& s) {
  if (s.empty()) return false;
  const char first = s.front();
  const bool first_ok =
    (first >= 'a' && first <= 'z') ||
    (first >= 'A' && first <= 'Z') ||
    (first >= '0' && first <= '9');
  if (!first_ok) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '.' || c == '+' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool expect_non_empty_string(
  const Json::Value& obj,
  const char* key,
  std::string* out,
  std::string* out_err
) {
  if (!obj.isMember(key) || !obj[key].isString()) {
    if (out_err) *out_err = std::string("missing or invalid field: ") + key;
    return false;
  }
  const std::string value = trim_copy(obj[key].asString());
  if (value.empty()) {
    if (out_err) *out_err = std::string("missing or invalid field: ") + key;
    return false;
  }
  if (out) *out = value;
  return true;
}

static bool validate_string_list(
  const Json::Value& value,
  const std::string& field,
  std::string* out_err
) {
  if (value.isNull()) return true;
  if (!value.isArray()) {
    if (out_err) *out_err = field + " must be a list";
    return false;
  }
  for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
    if (!value[i].isString() || trim_copy(value[i].asString()).empty()) {
      if (out_err) *out_err = field + " entries must be non-empty strings";
      return false;
    }
  }
  return true;
}

static bool validate_requires(const Json::Value& value, std::string* out_err) {
  if (value.isNull()) return true;
  if (!value.isObject()) {
    if (out_err) *out_err = "requires must be an object";
    return false;
  }
  for (const auto& key : value.getMemberNames()) {
    if (kAllowedRequiresKeys.find(key) == kAllowedRequiresKeys.end()) {
      if (out_err) *out_err = "unexpected requires keys: " + key;
      return false;
    }
  }
  return validate_string_list(value.get("tools", Json::Value(Json::nullValue)), "requires.tools", out_err) &&
         validate_string_list(value.get("plugins", Json::Value(Json::nullValue)), "requires.plugins", out_err) &&
         validate_string_list(value.get("features", Json::Value(Json::nullValue)), "requires.features", out_err);
}

static bool validate_inputs_schema(const Json::Value& value, std::string* out_err) {
  if (value.isNull()) return true;
  if (!value.isObject()) {
    if (out_err) *out_err = "inputs_schema must be an object";
    return false;
  }
  if (value.isMember("type") && (!value["type"].isString() || value["type"].asString() != "object")) {
    if (out_err) *out_err = "inputs_schema.type must be 'object' in v0";
    return false;
  }
  if (value.isMember("properties") && !value["properties"].isObject()) {
    if (out_err) *out_err = "inputs_schema.properties must be an object";
    return false;
  }
  if (value.isMember("required") && !validate_string_list(value["required"], "inputs_schema.required", out_err)) {
    return false;
  }
  return true;
}

static bool validate_instruction_fragments(const Json::Value& value, std::string* out_err) {
  if (value.isNull()) return true;
  if (!value.isObject()) {
    if (out_err) *out_err = "instruction_fragments must be an object";
    return false;
  }
  if (value.isMember("shared") && !validate_string_list(value["shared"], "instruction_fragments.shared", out_err)) {
    return false;
  }
  if (value.isMember("roles")) {
    if (!value["roles"].isObject()) {
      if (out_err) *out_err = "instruction_fragments.roles must be an object";
      return false;
    }
    for (const auto& role : value["roles"].getMemberNames()) {
      if (trim_copy(role).empty()) {
        if (out_err) *out_err = "instruction_fragments.roles keys must be non-empty strings";
        return false;
      }
      if (!validate_string_list(value["roles"][role], "instruction_fragments.roles." + role, out_err)) {
        return false;
      }
    }
  }
  return true;
}

static bool validate_manifest(const Json::Value& manifest, std::string* out_err) {
  if (!manifest.isObject()) {
    if (out_err) *out_err = "manifest must be a JSON object";
    return false;
  }

  for (const auto& key : manifest.getMemberNames()) {
    if (kAllowedKeys.find(key) == kAllowedKeys.end()) {
      if (out_err) *out_err = "unexpected keys: " + key;
      return false;
    }
  }

  std::string skill_id;
  if (!expect_non_empty_string(manifest, "skill_id", &skill_id, out_err)) return false;
  if (!is_runtime_skill_id_safe(skill_id)) {
    if (out_err) *out_err = "skill_id must use lowercase letters, digits, '-' or '_'";
    return false;
  }

  std::string version;
  if (!expect_non_empty_string(manifest, "version", &version, out_err)) return false;
  if (!is_semver_like(version)) {
    if (out_err) *out_err = "version must be a semver-like string";
    return false;
  }

  if (!expect_non_empty_string(manifest, "description", nullptr, out_err)) return false;

  std::string kind;
  if (!expect_non_empty_string(manifest, "kind", &kind, out_err)) return false;
  if (kAllowedKinds.find(kind) == kAllowedKinds.end()) {
    if (out_err) *out_err = "kind must be one of: instruction_pack, policy_bundle, team_bundle, workflow_bundle";
    return false;
  }

  if (!validate_requires(manifest.get("requires", Json::Value(Json::nullValue)), out_err)) return false;
  if (!validate_inputs_schema(manifest.get("inputs_schema", Json::Value(Json::nullValue)), out_err)) return false;
  if (!validate_instruction_fragments(manifest.get("instruction_fragments", Json::Value(Json::nullValue)), out_err)) {
    return false;
  }

  for (const char* field : {"workflow_template", "team_template", "policy_preset", "ui"}) {
    const Json::Value value = manifest.get(field, Json::Value(Json::nullValue));
    if (!value.isNull() && !value.isObject()) {
      if (out_err) *out_err = std::string(field) + " must be an object";
      return false;
    }
  }

  const Json::Value ui = manifest.get("ui", Json::Value(Json::nullValue));
  if (ui.isObject()) {
    for (const char* field : {"label", "category", "icon"}) {
      if (ui.isMember(field) && (!ui[field].isString() || trim_copy(ui[field].asString()).empty())) {
        if (out_err) *out_err = std::string("ui.") + field + " must be a non-empty string";
        return false;
      }
    }
  }

  return true;
}

static bool load_json_file(const std::filesystem::path& path, Json::Value* out, std::string* out_err) {
  std::ifstream in(path);
  if (!in.is_open()) {
    if (out_err) *out_err = "file not found: " + path.string();
    return false;
  }
  Json::CharReaderBuilder rb;
  rb["collectComments"] = false;
  JSONCPP_STRING errs;
  if (!Json::parseFromStream(rb, in, out, &errs)) {
    if (out_err) *out_err = "invalid JSON in " + path.string() + ": " + errs;
    return false;
  }
  return true;
}

static bool should_skip_discovery_path(const std::filesystem::path& path) {
  for (const auto& part : path) {
    const std::string name = part.string();
    if (kIgnoredDiscoveryDirs.find(name) != kIgnoredDiscoveryDirs.end()) return true;
  }
  return false;
}

static std::vector<std::filesystem::path> runtime_skill_roots(const DaemonConfig& cfg) {
  std::vector<std::filesystem::path> roots;
  std::error_code ec;
  std::filesystem::path probe = std::filesystem::current_path(ec);
  if (!ec) {
    while (!probe.empty()) {
      const std::filesystem::path candidate = (probe / "tools" / "runtime_skills").lexically_normal();
      std::error_code exists_ec;
      if (std::filesystem::exists(candidate, exists_ec) && !exists_ec) {
        roots.push_back(candidate);
        break;
      }
      const std::filesystem::path parent = probe.parent_path();
      if (parent.empty() || parent == probe) break;
      probe = parent;
    }
  }
  if (!cfg.state_dir.empty()) {
    roots.push_back((std::filesystem::path(cfg.state_dir) / "runtime_skills").lexically_normal());
  }
  std::vector<std::filesystem::path> deduped;
  std::set<std::string> seen;
  for (const auto& root : roots) {
    const std::string key = root.lexically_normal().string();
    if (seen.insert(key).second) deduped.push_back(root);
  }
  return deduped;
}

static bool discover_skills(
  const DaemonConfig& cfg,
  std::vector<RuntimeSkillEntry>* out,
  std::string* out_err
) {
  if (!out) return false;
  out->clear();
  std::unordered_set<std::string> seen_skill_ids;
  for (const auto& root : runtime_skill_roots(cfg)) {
    std::error_code exists_ec;
    if (!std::filesystem::exists(root, exists_ec) || exists_ec) continue;
    std::vector<std::filesystem::path> manifests;
    std::error_code walk_ec;
    std::filesystem::recursive_directory_iterator it(
      root,
      std::filesystem::directory_options::skip_permission_denied,
      walk_ec
    );
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(walk_ec)) {
      if (walk_ec) {
        walk_ec.clear();
        continue;
      }
      const std::filesystem::path current = it->path();
      if (should_skip_discovery_path(current)) {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file()) continue;
      if (current.filename() == "manifest.json") manifests.push_back(current.lexically_normal());
    }
    std::sort(manifests.begin(), manifests.end());
    for (const auto& manifest_path : manifests) {
      Json::Value manifest;
      if (!load_json_file(manifest_path, &manifest, out_err)) return false;
      if (manifest_path.filename() != "manifest.json") {
        if (out_err) *out_err = "runtime skill manifests must be named manifest.json: " + manifest_path.string();
        return false;
      }
      if (!validate_manifest(manifest, out_err)) {
        if (out_err) *out_err = *out_err + " (" + manifest_path.string() + ")";
        return false;
      }
      const std::string skill_id = manifest["skill_id"].asString();
      if (!seen_skill_ids.insert(skill_id).second) continue;
      out->push_back(RuntimeSkillEntry{root, manifest_path, manifest});
    }
  }
  return true;
}

static bool find_skill(
  const DaemonConfig& cfg,
  const std::string& skill_id,
  RuntimeSkillEntry* out,
  std::string* out_err
) {
  std::vector<RuntimeSkillEntry> entries;
  if (!discover_skills(cfg, &entries, out_err)) return false;
  for (const auto& entry : entries) {
    if (entry.manifest["skill_id"].asString() == skill_id) {
      if (out) *out = entry;
      return true;
    }
  }
  if (out_err) *out_err = "runtime skill not found: " + skill_id;
  return false;
}

static Json::Value capabilities_to_json() {
  Json::Value caps(Json::objectValue);
  caps["tools"] = Json::Value(Json::arrayValue);
  caps["plugins"] = Json::Value(Json::arrayValue);
  Json::Value features(Json::arrayValue);
  for (const char* feature : {"approval_queue", "runtime_skills", "workflow_composer", "workflow_submit"}) {
    features.append(feature);
  }
  caps["features"] = features;
  return caps;
}

static bool json_value_in_array(const Json::Value& needle, const Json::Value& haystack) {
  if (!haystack.isArray()) return false;
  for (Json::ArrayIndex i = 0; i < haystack.size(); ++i) {
    if (haystack[i] == needle) return true;
  }
  return false;
}

static bool validate_inputs_against_schema(
  const Json::Value& inputs,
  const Json::Value& schema,
  const std::string& path,
  std::string* out_err
) {
  if (schema.isNull()) return true;
  if (!schema.isObject()) {
    if (out_err) *out_err = "inputs_schema must be an object";
    return false;
  }

  const Json::Value enum_values = schema.get("enum", Json::Value(Json::nullValue));
  if (enum_values.isArray() && !json_value_in_array(inputs, enum_values)) {
    if (out_err) *out_err = path + " must be one of the allowed enum values";
    return false;
  }

  const Json::Value type_value = schema.get("type", Json::Value(Json::nullValue));
  if (!type_value.isString()) return true;
  const std::string expected_type = type_value.asString();

  if (expected_type == "object") {
    if (!inputs.isObject()) {
      if (out_err) *out_err = path + " must be an object";
      return false;
    }
    const Json::Value properties = schema.get("properties", Json::Value(Json::objectValue));
    if (!properties.isObject()) {
      if (out_err) *out_err = path + " schema properties must be an object";
      return false;
    }
    const Json::Value required = schema.get("required", Json::Value(Json::arrayValue));
    if (!required.isArray()) {
      if (out_err) *out_err = path + " schema required must be a list";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < required.size(); ++i) {
      if (!required[i].isString()) {
        if (out_err) *out_err = path + " schema required must contain strings";
        return false;
      }
      const std::string key = required[i].asString();
      if (!inputs.isMember(key)) {
        if (out_err) *out_err = path + "." + key + " is required";
        return false;
      }
    }
    const Json::Value additional = schema.get("additionalProperties", Json::Value(true));
    if (additional.isBool() && !additional.asBool() && !properties.getMemberNames().empty()) {
      std::unordered_set<std::string> allowed(properties.getMemberNames().begin(), properties.getMemberNames().end());
      for (const auto& key : inputs.getMemberNames()) {
        if (allowed.find(key) == allowed.end()) {
          if (out_err) *out_err = path + " contains unexpected keys: " + key;
          return false;
        }
      }
    }
    for (const auto& key : properties.getMemberNames()) {
      if (!inputs.isMember(key)) continue;
      if (!validate_inputs_against_schema(inputs[key], properties[key], path + "." + key, out_err)) return false;
    }
    return true;
  }

  if (expected_type == "array") {
    if (!inputs.isArray()) {
      if (out_err) *out_err = path + " must be an array";
      return false;
    }
    const Json::Value item_schema = schema.get("items", Json::Value(Json::nullValue));
    if (item_schema.isObject()) {
      for (Json::ArrayIndex i = 0; i < inputs.size(); ++i) {
        if (!validate_inputs_against_schema(inputs[i], item_schema, path + "[" + std::to_string(i) + "]", out_err)) {
          return false;
        }
      }
    }
    return true;
  }

  if (expected_type == "string") {
    if (!inputs.isString()) {
      if (out_err) *out_err = path + " must be a string";
      return false;
    }
    return true;
  }
  if (expected_type == "boolean") {
    if (!inputs.isBool()) {
      if (out_err) *out_err = path + " must be a boolean";
      return false;
    }
    return true;
  }
  if (expected_type == "integer") {
    if (!(inputs.isInt64() || inputs.isUInt64() || inputs.isInt() || inputs.isUInt())) {
      if (out_err) *out_err = path + " must be an integer";
      return false;
    }
    return true;
  }
  if (expected_type == "number") {
    if (!inputs.isNumeric()) {
      if (out_err) *out_err = path + " must be a number";
      return false;
    }
    return true;
  }
  if (expected_type == "null") {
    if (!inputs.isNull()) {
      if (out_err) *out_err = path + " must be null";
      return false;
    }
    return true;
  }

  if (out_err) *out_err = path + " uses unsupported schema type: " + expected_type;
  return false;
}

static Json::Value missing_requirements(
  const Json::Value& manifest,
  const Json::Value& capabilities
) {
  Json::Value missing(Json::objectValue);
  const Json::Value requires = manifest.get("requires", Json::Value(Json::objectValue));
  for (const char* key : {"tools", "plugins", "features"}) {
    const Json::Value required_list = requires.get(key, Json::Value(Json::arrayValue));
    const Json::Value available_list = capabilities.get(key, Json::Value(Json::arrayValue));
    Json::Value missing_list(Json::arrayValue);
    for (Json::ArrayIndex i = 0; i < required_list.size(); ++i) {
      if (!required_list[i].isString()) continue;
      if (!json_value_in_array(required_list[i], available_list)) missing_list.append(required_list[i]);
    }
    if (!missing_list.empty()) missing[key] = missing_list;
  }
  return missing;
}

static Json::Value materialize_workflow_request(
  const RuntimeSkillEntry& entry,
  const Json::Value& inputs,
  std::string* out_err
) {
  const Json::Value workflow_template = entry.manifest.get("workflow_template", Json::Value(Json::nullValue));
  if (workflow_template.isNull()) return Json::Value(Json::nullValue);
  if (!workflow_template.isObject()) {
    if (out_err) *out_err = "workflow_template must be an object";
    return Json::Value(Json::nullValue);
  }
  Json::Value workflow = workflow_template;
  Json::Value merged_inputs = workflow.get("inputs", Json::Value(Json::objectValue));
  if (!merged_inputs.isObject()) {
    if (out_err) *out_err = "workflow_template.inputs must be an object when present";
    return Json::Value(Json::nullValue);
  }
  if (inputs.isObject()) {
    for (const auto& key : inputs.getMemberNames()) merged_inputs[key] = inputs[key];
  }
  if (!merged_inputs.getMemberNames().empty()) workflow["inputs"] = merged_inputs;

  Json::Value defaults = workflow.get("defaults", Json::Value(Json::objectValue));
  if (!defaults.isObject()) {
    if (out_err) *out_err = "workflow_template.defaults must be an object when present";
    return Json::Value(Json::nullValue);
  }
  const Json::Value policy_preset = entry.manifest.get("policy_preset", Json::Value(Json::objectValue));
  if (policy_preset.isObject() && !defaults.isMember("max_steps")) {
    const Json::Value max_steps = policy_preset.get("max_steps", Json::Value(Json::nullValue));
    if (max_steps.isInt64() || max_steps.isUInt64() || max_steps.isInt() || max_steps.isUInt()) {
      defaults["max_steps"] = max_steps;
    }
  }
  if (!defaults.getMemberNames().empty()) workflow["defaults"] = defaults;

  Json::Value runtime_skill(Json::objectValue);
  runtime_skill["skill_id"] = entry.manifest["skill_id"].asString();
  runtime_skill["skill_version"] = entry.manifest["version"].asString();
  runtime_skill["manifest_sha256"] = manifest_hash_hex(entry.manifest);
  runtime_skill["inputs"] = inputs;
  workflow["runtime_skill"] = runtime_skill;
  return workflow;
}

static bool apply_common_headers(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  return daemon_require_auth(cfg, req, resp);
}

}  // namespace

void handle_runtime_skill_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  (void)db_or_null;
  if (!apply_common_headers(cfg, cors_cfg, req, resp)) return;

  std::string kind_filter = trim_copy(query_get(req.query, "kind").value_or(""));
  std::string category_filter = trim_copy(query_get(req.query, "category").value_or(""));

  std::vector<RuntimeSkillEntry> entries;
  std::string err;
  if (!discover_skills(cfg, &entries, &err)) {
    resp->status = 500;
    resp->body = json_error_body(err.empty() ? "failed to discover runtime skills" : err);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["capabilities"] = capabilities_to_json();
  Json::Value skills(Json::arrayValue);
  for (const auto& entry : entries) {
    const Json::Value ui = entry.manifest.get("ui", Json::Value(Json::objectValue));
    const std::string kind = entry.manifest["kind"].asString();
    const std::string category =
      ui.isObject() && ui.isMember("category") && ui["category"].isString() ? ui["category"].asString() : "";
    if (!kind_filter.empty() && kind != kind_filter) continue;
    if (!category_filter.empty() && category != category_filter) continue;

    Json::Value row(Json::objectValue);
    row["skill_id"] = entry.manifest["skill_id"].asString();
    row["version"] = entry.manifest["version"].asString();
    row["kind"] = kind;
    row["description"] = entry.manifest["description"].asString();
    row["category"] = category;
    row["label"] =
      ui.isObject() && ui.isMember("label") && ui["label"].isString() ? ui["label"].asString() : "";
    row["source_manifest"] = entry.manifest_path.string();
    row["root"] = entry.root.string();
    row["requires"] = entry.manifest.get("requires", Json::Value(Json::objectValue));
    row["inputs_schema"] = entry.manifest.get("inputs_schema", Json::Value(Json::nullValue));
    row["ui"] = entry.manifest.get("ui", Json::Value(Json::objectValue));
    row["has_workflow_template"] = entry.manifest.isMember("workflow_template") && entry.manifest["workflow_template"].isObject();
    row["has_team_template"] = entry.manifest.isMember("team_template") && entry.manifest["team_template"].isObject();
    row["has_policy_preset"] = entry.manifest.isMember("policy_preset") && entry.manifest["policy_preset"].isObject();
    skills.append(row);
  }
  out["skills"] = skills;
  resp->body = json_stringify_compact(out);
}

void handle_runtime_skill_resolve_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  (void)db_or_null;
  if (!apply_common_headers(cfg, cors_cfg, req, resp)) return;

  Json::Value body;
  std::string parse_err;
  if (!json_parse_object(req.body, &body, &parse_err)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON: " + parse_err);
    return;
  }

  const std::string skill_id =
    body.isMember("skill_id") && body["skill_id"].isString() ? trim_copy(body["skill_id"].asString()) : "";
  if (skill_id.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing or invalid field: skill_id");
    return;
  }

  const Json::Value inputs = body.get("inputs", Json::Value(Json::objectValue));
  if (!inputs.isObject() && !inputs.isNull()) {
    resp->status = 400;
    resp->body = json_error_body("runtime skill inputs must be an object");
    return;
  }

  RuntimeSkillEntry entry;
  std::string err;
  if (!find_skill(cfg, skill_id, &entry, &err)) {
    resp->status = err.rfind("runtime skill not found:", 0) == 0 ? 404 : 500;
    resp->body = json_error_body(err.empty() ? "runtime skill lookup failed" : err);
    return;
  }

  const Json::Value schema = entry.manifest.get("inputs_schema", Json::Value(Json::nullValue));
  if (!validate_inputs_against_schema(inputs.isNull() ? Json::Value(Json::objectValue) : inputs, schema, "inputs", &err)) {
    resp->status = 400;
    resp->body = json_error_body(err);
    return;
  }

  const Json::Value capabilities = capabilities_to_json();
  const Json::Value missing = missing_requirements(entry.manifest, capabilities);
  if (!missing.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing runtime requirements", "missing_runtime_requirements", &missing);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["skill_id"] = entry.manifest["skill_id"].asString();
  out["skill_version"] = entry.manifest["version"].asString();
  out["description"] = entry.manifest["description"].asString();
  out["kind"] = entry.manifest["kind"].asString();
  out["manifest_sha256"] = manifest_hash_hex(entry.manifest);
  out["source_manifest"] = entry.manifest_path.string();
  out["inputs"] = inputs.isNull() ? Json::Value(Json::objectValue) : inputs;
  out["manifest"] = entry.manifest;
  Json::Value resolved(Json::objectValue);
  resolved["requires"] = entry.manifest.get("requires", Json::Value(Json::objectValue));
  resolved["instruction_fragments"] = entry.manifest.get("instruction_fragments", Json::Value(Json::objectValue));
  resolved["policy_preset"] = entry.manifest.get("policy_preset", Json::Value(Json::objectValue));
  resolved["team_template"] = entry.manifest.get("team_template", Json::Value(Json::objectValue));
  resolved["workflow_template"] = entry.manifest.get("workflow_template", Json::Value(Json::objectValue));
  resolved["ui"] = entry.manifest.get("ui", Json::Value(Json::objectValue));
  out["resolved"] = resolved;
  Json::Value materialized(Json::objectValue);
  const Json::Value workflow_request = materialize_workflow_request(entry, out["inputs"], &err);
  if (!err.empty()) {
    resp->status = 400;
    resp->body = json_error_body(err);
    return;
  }
  if (!workflow_request.isNull()) materialized["workflow_request"] = workflow_request;
  out["materialized"] = materialized;
  out["capabilities_checked"] = true;
  out["capabilities"] = capabilities;
  resp->body = json_stringify_compact(out);
}

}  // namespace agentd
