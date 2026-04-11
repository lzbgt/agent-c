#include "memory_correlation_graph.h"

#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace agentd {
namespace {

static std::string stable_id_suffix(const std::string& s) {
  uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis.
  for (unsigned char c : s) {
    h ^= (uint64_t)c;
    h *= 1099511628211ull;
  }
  std::ostringstream os;
  os << std::hex << std::setfill('0') << std::setw(16) << h;
  return os.str();
}

static std::string json_excerpt(const Json::Value& v, size_t max_bytes = 512) {
  if (v.isNull()) return "";
  if (v.isString()) return truncate_for_event(v.asString(), max_bytes);
  if (v.isBool()) return v.asBool() ? "true" : "false";
  if (v.isInt64() || v.isUInt64() || v.isDouble() || v.isInt() || v.isUInt()) {
    return truncate_for_event(json_stringify(v), max_bytes);
  }
  return truncate_for_event(json_stringify(v), max_bytes);
}

static std::vector<std::string> extract_prefixed_values(const std::string& s, const std::string& prefix) {
  std::vector<std::string> out;
  size_t pos = 0;
  while ((pos = s.find(prefix, pos)) != std::string::npos) {
    const size_t start = pos + prefix.size();
    size_t end = start;
    while (end < s.size()) {
      const unsigned char c = (unsigned char)s[end];
      if (std::isspace(c) || c == ',' || c == ';' || c == ')' || c == ']' || c == '}') break;
      end++;
    }
    if (end > start) out.push_back(s.substr(start, end - start));
    pos = end;
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

class GraphBuilder {
 public:
  explicit GraphBuilder(std::string trace_id) : trace_id_(std::move(trace_id)) {
    Json::Value n(Json::objectValue);
    n["id"] = "trace:" + trace_id_;
    n["kind"] = "trace";
    n["trace_id"] = trace_id_;
    nodes_[n["id"].asString()] = n;
  }

  void add_node(Json::Value node) {
    if (!node.isObject() || !node.isMember("id") || !node["id"].isString()) return;
    const std::string id = node["id"].asString();
    if (id.empty()) return;
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
      nodes_[id] = node;
      return;
    }
    const std::vector<std::string> names = node.getMemberNames();
    for (const auto& name : names) {
      if (!it->second.isMember(name) || it->second[name].isNull()) {
        it->second[name] = node[name];
      }
    }
  }

  void add_edge(const std::string& from, const std::string& to, const std::string& kind, const std::string& source = "") {
    if (from.empty() || to.empty() || kind.empty()) return;
    Json::Value edge(Json::objectValue);
    edge["from"] = from;
    edge["to"] = to;
    edge["kind"] = kind;
    if (!source.empty()) edge["source"] = truncate_for_event(source, 512);
    const std::string key = kind + "\n" + from + "\n" + to + "\n" + source;
    edges_[key] = edge;
  }

  void add_source_excerpt(const std::string& owner_id, const std::string& source, const std::string& edge_kind) {
    const std::string excerpt = trim_copy(source);
    if (owner_id.empty() || excerpt.empty()) return;
    const std::string sid = "source:" + stable_id_suffix(excerpt);
    Json::Value node(Json::objectValue);
    node["id"] = sid;
    node["kind"] = "source_excerpt";
    node["excerpt"] = truncate_for_event(excerpt, 512);
    add_node(node);
    add_edge(owner_id, sid, edge_kind, excerpt);
    add_identity_edges(owner_id, excerpt);
  }

  void add_correlated_trace_edge(const std::string& owner_id) {
    if (owner_id.empty() || trace_id_.empty()) return;
    add_edge(owner_id, "trace:" + trace_id_, "correlates_trace");
  }

  void add_identity_edges(const std::string& owner_id, const std::string& source) {
    if (owner_id.empty() || source.empty()) return;
    for (const auto& tid : extract_prefixed_values(source, "trace:")) {
      const std::string id = "trace:" + tid;
      Json::Value node(Json::objectValue);
      node["id"] = id;
      node["kind"] = "trace";
      node["trace_id"] = tid;
      add_node(node);
      const bool is_base_trace = tid == trace_id_;
      const bool is_scoped_trace = !trace_id_.empty() && tid.rfind(trace_id_ + ":", 0) == 0;
      if (is_base_trace) {
        add_edge(owner_id, id, "mentions_trace", source);
      } else {
        if (is_scoped_trace) add_edge(owner_id, "trace:" + trace_id_, "mentions_trace", source);
        add_edge(owner_id, id, "mentions_related_trace", source);
      }
    }
    for (const auto& wid : extract_prefixed_values(source, "workflow:")) {
      const std::string id = "workflow:" + wid;
      Json::Value node(Json::objectValue);
      node["id"] = id;
      node["kind"] = "workflow";
      node["workflow_id"] = wid;
      add_node(node);
      add_edge(owner_id, id, "from_workflow", source);
    }
    for (const auto& task : extract_prefixed_values(source, "task:")) {
      const std::string id = "task:" + task;
      Json::Value node(Json::objectValue);
      node["id"] = id;
      node["kind"] = "workflow_task";
      node["task_id"] = task;
      add_node(node);
      add_edge(owner_id, id, "from_task", source);
    }
    for (const auto& job : extract_prefixed_values(source, "job:")) {
      const std::string id = "job:" + job;
      Json::Value node(Json::objectValue);
      node["id"] = id;
      node["kind"] = "job";
      node["job_id"] = job;
      add_node(node);
      add_edge(owner_id, id, "from_job", source);
    }
  }

  Json::Value finish() const {
    Json::Value out(Json::objectValue);
    out["schema"] = "agentd.memory.relationship_graph.v1";
    out["trace_id"] = trace_id_;
    Json::Value nodes(Json::arrayValue);
    for (const auto& kv : nodes_) nodes.append(kv.second);
    Json::Value edges(Json::arrayValue);
    for (const auto& kv : edges_) edges.append(kv.second);
    out["nodes"] = nodes;
    out["edges"] = edges;
    Json::Value counts(Json::objectValue);
    counts["nodes"] = (Json::Int64)nodes.size();
    counts["edges"] = (Json::Int64)edges.size();
    out["counts"] = counts;
    return out;
  }

 private:
  std::string trace_id_;
  std::map<std::string, Json::Value> nodes_;
  std::map<std::string, Json::Value> edges_;
};

static Json::Value checkpoint_from_entry_or_parent(const Json::Value& entry, const Json::Value& parent_checkpoint) {
  Json::Value ck(Json::objectValue);
  if (parent_checkpoint.isObject()) ck = parent_checkpoint;
  const char* fields[] = {"checkpoint_path", "structured_path", "checkpoint_ts_utc", "checkpoint_ts_utc_ms"};
  for (const char* field : fields) {
    if (entry.isMember(field)) ck[field] = entry[field];
  }
  return ck;
}

static void add_structured_entry(GraphBuilder* g, const Json::Value& entry, const Json::Value& parent_checkpoint) {
  if (!g || !entry.isObject()) return;
  const std::string key = entry.isMember("key") && entry["key"].isString() ? entry["key"].asString() : "";
  if (key.empty()) return;
  const Json::Value rec = entry.isMember("record") ? entry["record"] : Json::Value(Json::objectValue);
  const std::string id = "memory:" + key;
  Json::Value node(Json::objectValue);
  node["id"] = id;
  node["kind"] = "memory_item";
  node["key"] = key;
  if (rec.isObject()) {
    if (rec.isMember("kind") && rec["kind"].isString()) node["record_kind"] = rec["kind"];
    if (rec.isMember("value")) node["value_excerpt"] = json_excerpt(rec["value"], 512);
  }
  Json::Value ck = checkpoint_from_entry_or_parent(entry, parent_checkpoint);
  if (ck.isObject() && !ck.empty()) node["checkpoint"] = ck;
  g->add_node(node);
  g->add_correlated_trace_edge(id);

  if (rec.isObject() && rec.isMember("sources") && rec["sources"].isArray()) {
    for (Json::ArrayIndex i = 0; i < rec["sources"].size(); i++) {
      if (!rec["sources"][i].isString()) continue;
      g->add_source_excerpt(id, rec["sources"][i].asString(), "has_source_excerpt");
    }
  }
}

static void add_structured_entries(GraphBuilder* g, const Json::Value& entries, const Json::Value& parent_checkpoint) {
  if (!g || !entries.isArray()) return;
  for (Json::ArrayIndex i = 0; i < entries.size(); i++) {
    add_structured_entry(g, entries[i], parent_checkpoint);
  }
}

static void add_daily_entry(GraphBuilder* g, const Json::Value& entry) {
  if (!g || !entry.isObject()) return;
  const std::string path = entry.isMember("path") && entry["path"].isString() ? entry["path"].asString() : "daily";
  const Json::Int64 line = entry.isMember("line") && (entry["line"].isInt64() || entry["line"].isInt()) ? entry["line"].asInt64() : 0;
  const std::string id = "daily:" + path + "#L" + std::to_string((long long)line);
  Json::Value node(Json::objectValue);
  node["id"] = id;
  node["kind"] = "daily_observation";
  node["path"] = path;
  if (line > 0) node["line"] = line;
  if (entry.isMember("text")) node["text_excerpt"] = json_excerpt(entry["text"], 512);
  if (entry.isMember("ts_utc")) node["ts_utc"] = entry["ts_utc"];
  g->add_node(node);
  if (entry.isMember("trace_id") && entry["trace_id"].isString() && !entry["trace_id"].asString().empty()) {
    const std::string tid = entry["trace_id"].asString();
    Json::Value trace(Json::objectValue);
    trace["id"] = "trace:" + tid;
    trace["kind"] = "trace";
    trace["trace_id"] = tid;
    g->add_node(trace);
    g->add_edge(id, "trace:" + tid, "mentions_trace");
  }
  if (entry.isMember("source") && entry["source"].isString()) {
    g->add_source_excerpt(id, entry["source"].asString(), "has_source_excerpt");
  }
  if (entry.isMember("text") && entry["text"].isString()) {
    g->add_source_excerpt(id, entry["text"].asString(), "has_text_excerpt");
  }
}

static void add_recap_entry(GraphBuilder* g, const Json::Value& entry) {
  if (!g || !entry.isObject()) return;
  const std::string path =
    entry.isMember("recap_path") && entry["recap_path"].isString()
      ? entry["recap_path"].asString()
      : (entry.isMember("path") && entry["path"].isString() ? entry["path"].asString() : "");
  if (path.empty()) return;
  const std::string id = "recap:" + path;
  Json::Value node(Json::objectValue);
  node["id"] = id;
  node["kind"] = "recap";
  node["path"] = path;
  if (entry.isMember("summary_excerpt")) node["summary_excerpt"] = json_excerpt(entry["summary_excerpt"], 512);
  if (entry.isMember("ts_utc")) node["ts_utc"] = entry["ts_utc"];
  g->add_node(node);
  if (entry.isMember("evidence_sources") && entry["evidence_sources"].isArray()) {
    for (Json::ArrayIndex i = 0; i < entry["evidence_sources"].size(); i++) {
      if (!entry["evidence_sources"][i].isString()) continue;
      g->add_source_excerpt(id, entry["evidence_sources"][i].asString(), "has_source_excerpt");
    }
  }
  if (entry.isMember("summary_excerpt") && entry["summary_excerpt"].isString()) {
    g->add_source_excerpt(id, entry["summary_excerpt"].asString(), "has_summary_excerpt");
  }
}

}  // namespace

Json::Value memory_correlation_relationship_graph_from_response(
  const std::string& trace_id,
  const Json::Value& response
) {
  GraphBuilder graph(trace_id);
  if (!response.isObject()) return graph.finish();

  add_structured_entries(&graph, response.isMember("entries") ? response["entries"] : Json::Value(Json::arrayValue),
                         response.isMember("checkpoint") ? response["checkpoint"] : Json::Value(Json::objectValue));

  if (response.isMember("timeline") && response["timeline"].isArray()) {
    for (Json::ArrayIndex i = 0; i < response["timeline"].size(); i++) {
      const Json::Value row = response["timeline"][i];
      if (!row.isObject()) continue;
      add_structured_entries(&graph,
                             row.isMember("entries") ? row["entries"] : Json::Value(Json::arrayValue),
                             row.isMember("checkpoint") ? row["checkpoint"] : Json::Value(Json::objectValue));
    }
  }

  if (response.isMember("daily_entries") && response["daily_entries"].isArray()) {
    for (Json::ArrayIndex i = 0; i < response["daily_entries"].size(); i++) add_daily_entry(&graph, response["daily_entries"][i]);
  }
  if (response.isMember("recap_entries") && response["recap_entries"].isArray()) {
    for (Json::ArrayIndex i = 0; i < response["recap_entries"].size(); i++) add_recap_entry(&graph, response["recap_entries"][i]);
  }
  return graph.finish();
}

}  // namespace agentd
