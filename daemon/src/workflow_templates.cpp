#include "workflow_templates.h"

#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <unordered_map>

namespace agentd {
namespace {

static std::string json_stringify_compact_local(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool workflow_input_name_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const unsigned char uc = (unsigned char)c;
    if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

static std::string expand_prompt_templates(
  const std::string& prompt,
  const std::unordered_map<std::string, std::string>& assistant_text_by_task,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  const std::unordered_map<std::string, Json::Value>* inputs_by_name
) {
  if (prompt.find("${task.") == std::string::npos && prompt.find("${input.") == std::string::npos) return prompt;
  std::string out;
  out.reserve(prompt.size() + 96);

  size_t i = 0;
  while (i < prompt.size()) {
    size_t p = prompt.find("${task.", i);
    size_t p2 = prompt.find("${input.", i);
    if (p == std::string::npos || (p2 != std::string::npos && p2 < p)) p = p2;
    if (p == std::string::npos) {
      out.append(prompt, i, std::string::npos);
      break;
    }
    out.append(prompt, i, p - i);
    const size_t end = prompt.find('}', p + 2);
    if (end == std::string::npos) {
      out.append(prompt, p, std::string::npos);
      break;
    }

    const std::string token = prompt.substr(p + 2, end - (p + 2)); // strip ${ ... }
    const bool is_task = (token.rfind("task.", 0) == 0);
    const bool is_input = (token.rfind("input.", 0) == 0);
    if (!is_task && !is_input) {
      out.append(prompt, p, (end - p) + 1);
      i = end + 1;
      continue;
    }

    const std::string rest = token.substr(is_task ? std::string("task.").size() : std::string("input.").size());
    // task rest: <id>.assistant_text OR <id>.json:/ptr
    // input rest: <name> OR <name>.json:/ptr
    const std::string suffix_text = ".assistant_text";
    const std::string dot_json = ".json:";

    // assistant_text (task only)
    if (is_task && rest.size() > suffix_text.size() && rest.rfind(suffix_text) == (rest.size() - suffix_text.size())) {
      const std::string task_id = rest.substr(0, rest.size() - suffix_text.size());
      auto it = assistant_text_by_task.find(task_id);
      if (it == assistant_text_by_task.end()) {
        out.append(prompt, p, (end - p) + 1);
        i = end + 1;
        continue;
      }
      out += it->second;
      i = end + 1;
      continue;
    }

    // json pointer extraction
    const size_t jpos = rest.find(dot_json);
    if (jpos != std::string::npos) {
      const std::string id = rest.substr(0, jpos);
      const std::string ptr = rest.substr(jpos + dot_json.size());
      const Json::Value* root = nullptr;
      if (is_task) {
        auto it = result_json_by_task.find(id);
        if (it != result_json_by_task.end()) root = &it->second;
      } else if (inputs_by_name) {
        auto it = inputs_by_name->find(id);
        if (it != inputs_by_name->end()) root = &it->second;
      }
      if (!root) {
        out.append(prompt, p, (end - p) + 1);
        i = end + 1;
        continue;
      }
      const Json::Value* got = nullptr;
      if (!json_pointer_get(*root, ptr, &got) || !got) {
        out.append(prompt, p, (end - p) + 1);
        i = end + 1;
        continue;
      }
      if (got->isString()) out += got->asString();
      else out += json_stringify_compact_local(*got);
      i = end + 1;
      continue;
    }

    // input.<name> embeds the entire JSON value (stringified if needed).
    if (is_input && inputs_by_name) {
      auto it = inputs_by_name->find(rest);
      if (it != inputs_by_name->end()) {
        if (it->second.isString()) out += it->second.asString();
        else out += json_stringify_compact_local(it->second);
        i = end + 1;
        continue;
      }
    }

    // Unknown token shape; keep literal.
    out.append(prompt, p, (end - p) + 1);
    i = end + 1;
  }

  return out;
}

static void expand_templates_in_json_value_impl(
  Json::Value* v,
  const std::unordered_map<std::string, std::string>& assistant_text_by_task,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  const std::unordered_map<std::string, Json::Value>* inputs_by_name,
  bool allow_unresolved_refs,
  std::vector<std::string>* out_errors
) {
  if (!v) return;
  if (out_errors) out_errors->reserve(out_errors->size() + 4);

  // JSON-native embedding:
  // If a value is exactly {"$ref":"task.<id>.(assistant_text|json:<ptr>)"} or {"$ref":"input.<name>(.json:<ptr>)"},
  // replace the entire value with the referenced value (as JSON).
  if (v->isObject()) {
    const auto keys = v->getMemberNames();
    if (keys.size() == 1 && keys[0] == "$ref" && (*v)["$ref"].isString()) {
      const std::string ref = trim_copy((*v)["$ref"].asString());
      const std::string task_prefix = "task.";
      const std::string input_prefix = "input.";
      const bool is_task = (ref.rfind(task_prefix, 0) == 0);
      const bool is_input = (ref.rfind(input_prefix, 0) == 0);
      if (!is_task && !is_input) {
        if (out_errors) out_errors->push_back("invalid $ref (expected task.<id>... or input.<name>...): " + ref);
        *v = Json::Value(Json::nullValue);
        return;
      }
      if (is_input && !inputs_by_name) {
        // Defer input.* resolution to a later pass where inputs are available.
        return;
      }
      const std::string rest = ref.substr(is_task ? task_prefix.size() : input_prefix.size());
      const std::string suffix_text = ".assistant_text";
      const std::string dot_json = ".json:";

      if (is_task && rest.size() > suffix_text.size() && rest.rfind(suffix_text) == (rest.size() - suffix_text.size())) {
        const std::string task_id = rest.substr(0, rest.size() - suffix_text.size());
        auto it = assistant_text_by_task.find(task_id);
        if (it == assistant_text_by_task.end()) {
          if (allow_unresolved_refs) return;
          if (out_errors) out_errors->push_back("unresolved $ref assistant_text: " + ref);
          *v = Json::Value(Json::nullValue);
          return;
        }
        *v = it->second;
        return;
      }

      const size_t jpos = rest.find(dot_json);
      if (jpos != std::string::npos) {
        const std::string id = rest.substr(0, jpos);
        const std::string ptr = rest.substr(jpos + dot_json.size());
        const Json::Value* root = nullptr;
        if (is_task) {
          auto it = result_json_by_task.find(id);
          if (it != result_json_by_task.end()) root = &it->second;
        } else if (inputs_by_name) {
          auto it = inputs_by_name->find(id);
          if (it != inputs_by_name->end()) root = &it->second;
        }
        if (!root) {
          if (allow_unresolved_refs) return;
          if (out_errors) {
            out_errors->push_back(
              std::string("unresolved $ref json (") + (is_task ? "task missing" : "input missing") + "): " + ref
            );
          }
          *v = Json::Value(Json::nullValue);
          return;
        }
        if (ptr.empty()) {
          *v = *root;
          return;
        }
        const Json::Value* got = nullptr;
        if (!json_pointer_get(*root, ptr, &got) || !got) {
          if (allow_unresolved_refs) return;
          if (out_errors) out_errors->push_back("unresolved $ref json pointer: " + ref);
          *v = Json::Value(Json::nullValue);
          return;
        }
        *v = *got;
        return;
      }

      if (is_input) {
        if (!inputs_by_name) {
          if (allow_unresolved_refs) return;
          if (out_errors) out_errors->push_back("unresolved $ref input (inputs not available): " + ref);
          *v = Json::Value(Json::nullValue);
          return;
        }
        auto it = inputs_by_name->find(rest);
        if (it == inputs_by_name->end()) {
          if (allow_unresolved_refs) return;
          if (out_errors) out_errors->push_back("unresolved $ref input: " + ref);
          *v = Json::Value(Json::nullValue);
          return;
        }
        *v = it->second;
        return;
      }

      if (allow_unresolved_refs) return;
      if (out_errors) out_errors->push_back("invalid $ref shape (expected task.<id>.assistant_text or task|input.<id>.json:): " + ref);
      *v = Json::Value(Json::nullValue);
      return;
    }
  }

  if (v->isString()) {
    const std::string s = v->asString();
    if (s.find("${task.") != std::string::npos || s.find("${input.") != std::string::npos) {
      *v = expand_prompt_templates(s, assistant_text_by_task, result_json_by_task, inputs_by_name);
    }
    return;
  }
  if (v->isArray()) {
    for (Json::ArrayIndex i = 0; i < v->size(); i++) {
      expand_templates_in_json_value_impl(&((*v)[i]), assistant_text_by_task, result_json_by_task, inputs_by_name, allow_unresolved_refs, out_errors);
    }
    return;
  }
  if (v->isObject()) {
    for (const auto& k : v->getMemberNames()) {
      const bool child_allow_unresolved_refs = allow_unresolved_refs || (k == "inputs");
      expand_templates_in_json_value_impl(&((*v)[k]), assistant_text_by_task, result_json_by_task, inputs_by_name, child_allow_unresolved_refs, out_errors);
    }
    return;
  }
}

static void expand_templates_in_json_value(
  Json::Value* v,
  const std::unordered_map<std::string, std::string>& assistant_text_by_task,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  const std::unordered_map<std::string, Json::Value>* inputs_by_name,
  std::vector<std::string>* out_errors
) {
  expand_templates_in_json_value_impl(
    v, assistant_text_by_task, result_json_by_task, inputs_by_name, /*allow_unresolved_refs=*/false, out_errors
  );
}

}  // namespace

bool workflow_expand_templates_for_task_request(
  Json::Value* request_obj,
  const std::unordered_map<std::string, std::string>& assistant_text_by_task,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::vector<std::string>* out_errors
) {
  if (!request_obj || !request_obj->isObject()) return false;
  if (out_errors) out_errors->clear();

  // Pass 1: expand task.* templates/$ref first (so inputs can reference task outputs).
  expand_templates_in_json_value(request_obj, assistant_text_by_task, result_json_by_task, nullptr, out_errors);

  // Pass 2: expand input.* templates/$ref using a derived inputs map. This is done in bounded rounds so inputs can
  // reference other inputs (input -> input chains) and still be usable in the same request.
  if (request_obj->isMember("inputs") && !(*request_obj)["inputs"].isNull()) {
    std::unordered_map<std::string, Json::Value> inputs_by_name;
    for (int iter = 0; iter < 3; iter++) {
      if (!request_obj->isMember("inputs") || (*request_obj)["inputs"].isNull()) break;
      if (!(*request_obj)["inputs"].isObject()) {
        if (out_errors) out_errors->push_back("inputs must be an object");
        break;
      }

      inputs_by_name.clear();
      for (const auto& k : (*request_obj)["inputs"].getMemberNames()) {
        if (!workflow_input_name_is_safe(k)) {
          if (out_errors) out_errors->push_back("invalid input name (expected [A-Za-z0-9_-], len<=128): " + k);
          continue;
        }
        inputs_by_name[k] = (*request_obj)["inputs"][k];
      }

      const std::string before = json_stringify_compact_local((*request_obj)["inputs"]);
      expand_templates_in_json_value(request_obj, assistant_text_by_task, result_json_by_task, &inputs_by_name, out_errors);
      const std::string after =
        request_obj->isMember("inputs") && (*request_obj)["inputs"].isObject()
          ? json_stringify_compact_local((*request_obj)["inputs"])
          : std::string();
      if (before == after) break;
    }
  }

  return !out_errors || out_errors->empty();
}

}  // namespace agentd

