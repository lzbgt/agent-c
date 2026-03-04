#include "workflow_endpoint_util.h"

#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}

void collect_referenced_task_ids_from_task_template_token(
  const std::string& token,
  std::unordered_set<std::string>* out
) {
  if (!out) return;
  if (token.rfind("task.", 0) != 0) return;

  const std::string rest = token.substr(std::string("task.").size());
  const std::string suffix_text = ".assistant_text";
  const std::string dot_json = ".json:";

  std::string task_id;
  if (rest.size() > suffix_text.size() && rest.rfind(suffix_text) == (rest.size() - suffix_text.size())) {
    task_id = rest.substr(0, rest.size() - suffix_text.size());
  } else {
    const size_t jpos = rest.find(dot_json);
    if (jpos != std::string::npos) {
      task_id = rest.substr(0, jpos);
    }
  }

  if (!task_id.empty() && id_is_safe(task_id)) out->insert(task_id);
}

}  // namespace

bool parse_citation_path_line(const std::string& citation, std::string* out_path, int* out_line) {
  if (out_path) out_path->clear();
  if (out_line) *out_line = 0;
  const std::string c = trim_copy(citation);
  if (c.empty()) return false;
  const size_t pos = c.rfind(':');
  if (pos == std::string::npos) return false;
  const std::string path = trim_copy(c.substr(0, pos));
  const std::string line_str = trim_copy(c.substr(pos + 1));
  if (path.empty() || line_str.empty()) return false;
  int line = 0;
  try {
    line = std::stoi(line_str);
  } catch (...) {
    return false;
  }
  if (line <= 0) return false;
  if (out_path) *out_path = path;
  if (out_line) *out_line = line;
  return true;
}

bool id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

bool is_safe_relpath_md(const std::string& p) {
  if (p.empty()) return false;
  if (p.size() > 300) return false;
  if (p.find('\\') != std::string::npos) return false;
  if (p[0] == '/') return false;
  if (p.find("..") != std::string::npos) return false;
  if (p.find('\0') != std::string::npos) return false;
  const std::string lp = to_lower_ascii(p);
  if (lp.size() < 3 || lp.rfind(".md") != lp.size() - 3) return false;
  return true;
}

std::string new_workflow_id() {
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);
  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);
  char buf[96];
  (void)snprintf(buf, sizeof(buf), "wf_%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
}

std::string new_workflow_schedule_id() {
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);
  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);
  char buf[96];
  (void)snprintf(buf, sizeof(buf), "wfs_%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
}

std::string redact_json_best_effort(const std::string& json) {
  if (json.empty()) return json;
  Json::Value v;
  std::string perr;
  if (!json_parse_any(json, &v, &perr)) return json;

  std::function<void(Json::Value*)> walk = [&](Json::Value* cur) {
    if (!cur) return;
    if (cur->isObject()) {
      for (const auto& k : cur->getMemberNames()) {
        const std::string kl = lower_copy(k);
        if (kl == "api_key" || kl == "authorization" || kl == "auth_token") {
          (*cur)[k] = "***redacted***";
        } else {
          walk(&((*cur)[k]));
        }
      }
    } else if (cur->isArray()) {
      for (Json::ArrayIndex i = 0; i < cur->size(); i++) {
        walk(&((*cur)[i]));
      }
    }
  };
  walk(&v);
  return json_stringify_compact(v);
}

bool validate_dag_or_error(
  const std::vector<std::string>& task_ids,
  const std::unordered_map<std::string, std::vector<std::string>>& deps,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  std::unordered_set<std::string> nodes(task_ids.begin(), task_ids.end());

  std::unordered_map<std::string, int> indeg;
  indeg.reserve(nodes.size());
  for (const auto& id : nodes) indeg[id] = 0;
  for (const auto& kv : deps) {
    const std::string& t = kv.first;
    if (!nodes.count(t)) continue;
    for (const auto& d : kv.second) {
      if (!nodes.count(d)) {
        if (out_err) *out_err = "unknown dependency: " + d;
        return false;
      }
      indeg[t] += 1;
    }
  }

  std::vector<std::string> q;
  q.reserve(nodes.size());
  for (const auto& kv : indeg) {
    if (kv.second == 0) q.push_back(kv.first);
  }
  size_t idx = 0;
  size_t visited = 0;
  while (idx < q.size()) {
    const std::string cur = q[idx++];
    visited++;
    for (const auto& kv2 : deps) {
      const std::string& child = kv2.first;
      if (!nodes.count(child)) continue;
      for (const auto& dep : kv2.second) {
        if (dep == cur) {
          indeg[child] -= 1;
          if (indeg[child] == 0) q.push_back(child);
        }
      }
    }
  }
  if (visited != nodes.size()) {
    if (out_err) *out_err = "dependency cycle detected";
    return false;
  }
  return true;
}

void collect_referenced_task_ids_from_json_value(
  const Json::Value& v,
  std::unordered_set<std::string>* out
) {
  if (!out) return;
  if (v.isString()) {
    const std::string s = v.asString();
    size_t i = 0;
    while (i < s.size()) {
      const size_t p = s.find("${task.", i);
      if (p == std::string::npos) break;
      const size_t end = s.find('}', p + 2);
      if (end == std::string::npos) break;
      const std::string token = s.substr(p + 2, end - (p + 2));
      collect_referenced_task_ids_from_task_template_token(token, out);
      i = end + 1;
    }
    return;
  }
  if (v.isArray()) {
    for (Json::ArrayIndex i = 0; i < v.size(); i++) {
      collect_referenced_task_ids_from_json_value(v[i], out);
    }
    return;
  }
  if (v.isObject()) {
    if (v.isMember("$ref") && v["$ref"].isString()) {
      const std::string ref = trim_copy(v["$ref"].asString());
      collect_referenced_task_ids_from_task_template_token(ref, out);
    }
    for (const auto& k : v.getMemberNames()) {
      collect_referenced_task_ids_from_json_value(v[k], out);
    }
    return;
  }
}

}  // namespace agentd
