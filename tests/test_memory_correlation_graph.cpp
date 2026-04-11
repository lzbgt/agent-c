#include "memory_correlation_graph.h"

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

bool has_node(const Json::Value& graph, const std::string& id, const std::string& kind) {
  const Json::Value nodes = graph.isMember("nodes") ? graph["nodes"] : Json::Value(Json::arrayValue);
  for (Json::ArrayIndex i = 0; i < nodes.size(); i++) {
    if (!nodes[i].isObject()) continue;
    if (nodes[i]["id"].asString() == id && nodes[i]["kind"].asString() == kind) return true;
  }
  return false;
}

bool has_edge(const Json::Value& graph, const std::string& from, const std::string& to, const std::string& kind) {
  const Json::Value edges = graph.isMember("edges") ? graph["edges"] : Json::Value(Json::arrayValue);
  for (Json::ArrayIndex i = 0; i < edges.size(); i++) {
    if (!edges[i].isObject()) continue;
    if (edges[i]["from"].asString() == from &&
        edges[i]["to"].asString() == to &&
        edges[i]["kind"].asString() == kind) {
      return true;
    }
  }
  return false;
}

void test_structured_daily_and_recap_relationships() {
  Json::Value response = parse_json(R"JSON({
    "entries": [
      {
        "key": "wf.test.corr",
        "record": {
          "kind": "fact",
          "value": "v1",
          "sources": ["workflow:wf1 task:M trace:t1 job:j1 evidence excerpt"]
        },
        "checkpoint_path": "checkpoints/structured_1.json",
        "structured_path": "STRUCTURED.md"
      }
    ],
    "daily_entries": [
      {
        "path": "2026-04-12.md",
        "line": 7,
        "trace_id": "t1",
        "source": "workflow:wf1 trace:t1",
        "text": "@obs trace:t1 daily excerpt"
      }
    ],
    "recap_entries": [
      {
        "recap_path": "recaps/daily_2026-04-12.md",
        "summary_excerpt": "summary trace:t1",
        "evidence_sources": ["workflow:wf1 task:M trace:t1"]
      }
    ]
  })JSON");

  const Json::Value graph = agentd::memory_correlation_relationship_graph_from_response("t1", response);
  assert(graph["schema"].asString() == "agentd.memory.relationship_graph.v1");
  assert(graph["trace_id"].asString() == "t1");
  assert(has_node(graph, "trace:t1", "trace"));
  assert(has_node(graph, "memory:wf.test.corr", "memory_item"));
  assert(has_node(graph, "workflow:wf1", "workflow"));
  assert(has_node(graph, "task:M", "workflow_task"));
  assert(has_node(graph, "job:j1", "job"));
  assert(has_node(graph, "daily:2026-04-12.md#L7", "daily_observation"));
  assert(has_node(graph, "recap:recaps/daily_2026-04-12.md", "recap"));
  assert(has_edge(graph, "memory:wf.test.corr", "trace:t1", "correlates_trace"));
  assert(has_edge(graph, "memory:wf.test.corr", "trace:t1", "mentions_trace"));
  assert(has_edge(graph, "memory:wf.test.corr", "workflow:wf1", "from_workflow"));
  assert(has_edge(graph, "memory:wf.test.corr", "task:M", "from_task"));
  assert(has_edge(graph, "memory:wf.test.corr", "job:j1", "from_job"));
  assert(has_edge(graph, "daily:2026-04-12.md#L7", "trace:t1", "mentions_trace"));
  assert(has_edge(graph, "recap:recaps/daily_2026-04-12.md", "workflow:wf1", "from_workflow"));
}

void test_timeline_entries_use_parent_checkpoint() {
  Json::Value response = parse_json(R"JSON({
    "timeline": [
      {
        "checkpoint": {
          "checkpoint_path": "checkpoints/structured_2.json",
          "structured_path": "STRUCTURED.md",
          "sha256": "abc"
        },
        "entries": [
          {
            "key": "wf.test.timeline",
            "record": {
              "kind": "fact",
              "value": "v2",
              "sources": ["workflow:wf2 task:C trace:t2"]
            }
          }
        ]
      }
    ]
  })JSON");

  const Json::Value graph = agentd::memory_correlation_relationship_graph_from_response("t2", response);
  assert(has_node(graph, "memory:wf.test.timeline", "memory_item"));
  assert(has_edge(graph, "memory:wf.test.timeline", "trace:t2", "correlates_trace"));
  assert(has_edge(graph, "memory:wf.test.timeline", "trace:t2", "mentions_trace"));
  assert(has_edge(graph, "memory:wf.test.timeline", "workflow:wf2", "from_workflow"));
  assert(has_edge(graph, "memory:wf.test.timeline", "task:C", "from_task"));
}

void test_task_scoped_trace_links_back_to_base_trace() {
  Json::Value response = parse_json(R"JSON({
    "entries": [
      {
        "key": "wf.test.scoped",
        "record": {
          "kind": "fact",
          "value": "v3",
          "sources": ["workflow:wf3 task:M trace:base_trace:M"]
        }
      }
    ]
  })JSON");

  const Json::Value graph = agentd::memory_correlation_relationship_graph_from_response("base_trace", response);
  assert(has_node(graph, "trace:base_trace", "trace"));
  assert(has_node(graph, "trace:base_trace:M", "trace"));
  assert(has_edge(graph, "memory:wf.test.scoped", "trace:base_trace", "mentions_trace"));
  assert(has_edge(graph, "memory:wf.test.scoped", "trace:base_trace:M", "mentions_related_trace"));
}

}  // namespace

int main() {
  test_structured_daily_and_recap_relationships();
  test_timeline_entries_use_parent_checkpoint();
  test_task_scoped_trace_links_back_to_base_trace();
  std::cout << "memory_correlation_graph_tests OK\n";
  return 0;
}
