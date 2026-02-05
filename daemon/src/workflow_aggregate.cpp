#include "workflow_aggregate.h"

#include "json_util.h"
#include "string_util.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace agentd {
namespace {

static std::string json_stringify_compact_local(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool json_parse_any_value(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = Json::Value(Json::nullValue);
  std::string perr;
  if (!json_parse_any(s, out, &perr)) {
    if (out_err) *out_err = perr;
    return false;
  }
  return true;
}

static bool string_array_from_json(const Json::Value& v, std::vector<std::string>* out) {
  if (!out) return false;
  out->clear();
  if (!v.isArray()) return false;
  out->reserve(v.size());
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (!v[i].isString()) continue;
    const std::string s = v[i].asString();
    if (!s.empty()) out->push_back(s);
  }
  return true;
}

static Json::Value workflow_aggregate_quorum_hashes_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  Json::Value out(Json::objectValue);
  out["kind"] = "aggregate";
  out["ok"] = false;

  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    out["error"] = out_error ? *out_error : "aggregate config must be an object";
    return out;
  }

  std::vector<std::string> task_ids;
  if (!string_array_from_json(agg["task_ids"], &task_ids) || task_ids.empty()) {
    if (out_error) *out_error = "aggregate.task_ids must be a non-empty array of strings";
    out["error"] = out_error ? *out_error : "aggregate.task_ids must be a non-empty array of strings";
    return out;
  }

  int quorum = (int)task_ids.size();
  if (agg.isMember("quorum") && agg["quorum"].isInt()) {
    quorum = agg["quorum"].asInt();
  }
  if (quorum < 1) quorum = 1;
  if (quorum > (int)task_ids.size()) quorum = (int)task_ids.size();

  std::vector<std::string> ptrs;
  if (agg.isMember("pointers")) {
    (void)string_array_from_json(agg["pointers"], &ptrs);
  }
  if (ptrs.empty()) {
    ptrs.push_back("/avm/result_hash");
    ptrs.push_back("/avm/trace_hash");
  }

  std::string node_ptr = "/edge/node_id";
  if (agg.isMember("node_pointer") && agg["node_pointer"].isString() && !agg["node_pointer"].asString().empty()) {
    node_ptr = agg["node_pointer"].asString();
  }

  bool require_distinct_nodes = false;
  if (agg.isMember("require_distinct_nodes") && agg["require_distinct_nodes"].isBool()) {
    require_distinct_nodes = agg["require_distinct_nodes"].asBool();
  }

  out["mode"] = "quorum_hashes";
  out["quorum"] = quorum;
  Json::Value arr(Json::arrayValue);
  for (const auto& id : task_ids) arr.append(id);
  out["task_ids"] = arr;
  Json::Value parr(Json::arrayValue);
  for (const auto& p : ptrs) parr.append(p);
  out["pointers"] = parr;
  out["node_pointer"] = node_ptr;
  out["require_distinct_nodes"] = require_distinct_nodes;

  // Optional evidence surface: attach stable node identity (when present) to votes.
  // This is useful for multi-node correctness debugging and attestation correlation.
  //
  // When require_distinct_nodes=true, node_pointer also becomes semantically important:
  // votes are counted across distinct node_id values, not per task_id.
  std::unordered_map<std::string, std::string> node_by_task_id;
  std::vector<std::string> node_ids_in_order;
  std::unordered_map<std::string, std::vector<std::string>> task_ids_by_node;
  std::vector<std::string> unknown_node_task_ids;
  {
    Json::Value nodes_by_task(Json::objectValue);
    for (const auto& tid : task_ids) {
      auto it = result_json_by_task.find(tid);
      if (it == result_json_by_task.end()) continue;
      const Json::Value& root = it->second;
      const Json::Value* got = nullptr;
      if (json_pointer_get(root, node_ptr, &got) && got && got->isString() && !got->asString().empty()) {
        const std::string nid = got->asString();
        nodes_by_task[tid] = nid;
        node_by_task_id[tid] = nid;
        if (task_ids_by_node.find(nid) == task_ids_by_node.end()) {
          node_ids_in_order.push_back(nid);
          task_ids_by_node[nid] = std::vector<std::string>();
        }
        task_ids_by_node[nid].push_back(tid);
      } else if (require_distinct_nodes) {
        unknown_node_task_ids.push_back(tid);
      }
    }
    if (!nodes_by_task.empty()) out["nodes_by_task_id"] = nodes_by_task;
  }
  if (require_distinct_nodes && !unknown_node_task_ids.empty()) {
    Json::Value unk(Json::arrayValue);
    for (const auto& tid : unknown_node_task_ids) unk.append(tid);
    out["unknown_node_task_ids"] = unk;
  }

  Json::Value checks(Json::arrayValue);
  bool all_ok = true;
  std::string first_chosen;

  for (const auto& ptr : ptrs) {
    Json::Value c(Json::objectValue);
    c["ptr"] = ptr;
    c["ok"] = false;
    c["quorum"] = quorum;
    c["require_distinct_nodes"] = require_distinct_nodes;

    std::unordered_map<std::string, int> counts;
    counts.reserve(task_ids.size());
    Json::Value values_by_task(Json::objectValue);
    Json::Value missing(Json::arrayValue);

    for (const auto& tid : task_ids) {
      auto it = result_json_by_task.find(tid);
      if (it == result_json_by_task.end()) {
        missing.append(tid);
        continue;
      }
      const Json::Value& root = it->second;
      const Json::Value* got = nullptr;
      if (!json_pointer_get(root, ptr, &got) || !got || !got->isString()) {
        missing.append(tid);
        continue;
      }
      const std::string v = got->asString();
      values_by_task[tid] = v;
      if (!v.empty()) counts[v] += 1;
    }

    if (require_distinct_nodes) {
      // Recount votes by distinct node_id (node_pointer), not by task_id.
      // Deterministic rule: each node contributes at most one vote, taken from the first task_id
      // (in aggregate.task_ids order) that has a non-empty string at `ptr`.
      std::unordered_map<std::string, int> node_counts;
      node_counts.reserve(node_ids_in_order.size());
      Json::Value values_by_node(Json::objectValue);
      Json::Value chosen_task_by_node(Json::objectValue);
      Json::Value missing_nodes(Json::arrayValue);
      Json::Value eligible_nodes(Json::arrayValue);

      for (const auto& nid : node_ids_in_order) {
        eligible_nodes.append(nid);
        auto it = task_ids_by_node.find(nid);
        if (it == task_ids_by_node.end()) continue;

        std::string chosen_v;
        std::string chosen_tid;
        for (const auto& tid : it->second) {
          if (!values_by_task.isMember(tid) || !values_by_task[tid].isString()) continue;
          const std::string v = values_by_task[tid].asString();
          if (v.empty()) continue;
          chosen_v = v;
          chosen_tid = tid;
          break;
        }
        if (chosen_v.empty()) {
          missing_nodes.append(nid);
          continue;
        }
        values_by_node[nid] = chosen_v;
        chosen_task_by_node[nid] = chosen_tid;
        node_counts[chosen_v] += 1;
      }

      counts.swap(node_counts);
      c["values_by_node"] = values_by_node;
      c["chosen_task_by_node_id"] = chosen_task_by_node;
      c["eligible_nodes"] = eligible_nodes;
      c["missing_nodes"] = missing_nodes;
      c["count_kind"] = "nodes";
    } else {
      c["count_kind"] = "tasks";
    }

    // Determine chosen value deterministically:
    // - highest count
    // - tie-breaker: lexicographically smallest value
    std::string chosen;
    int best = 0;
    for (const auto& kv : counts) {
      const std::string& v = kv.first;
      const int n = kv.second;
      if (n > best || (n == best && !v.empty() && (chosen.empty() || v < chosen))) {
        best = n;
        chosen = v;
      }
    }

    Json::Value votes(Json::arrayValue);
    // Emit votes in a deterministic order (count desc, value asc).
    std::vector<std::pair<std::string, int>> vs;
    vs.reserve(counts.size());
    for (const auto& kv : counts) vs.push_back(kv);
    std::sort(vs.begin(), vs.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });
    for (const auto& kv : vs) {
      Json::Value vj(Json::objectValue);
      vj["value"] = kv.first;
      vj["count"] = kv.second;
      votes.append(vj);
    }

    c["chosen"] = chosen;
    c["chosen_count"] = best;
    c["votes"] = votes;
    c["values_by_task"] = values_by_task;
    c["missing"] = missing;

    const bool ok = (!chosen.empty() && best >= quorum);
    c["ok"] = ok;
    if (!ok) {
      c["error"] = "quorum not met";
      all_ok = false;
    } else if (first_chosen.empty()) {
      first_chosen = chosen;
    }

    checks.append(c);
  }

  out["checks"] = checks;
  out["ok"] = all_ok;
  if (all_ok) {
    out["assistant_text"] = first_chosen;
  } else {
    out["assistant_text"] = "";
    out["error"] = "aggregate check failed";
  }
  return out;
}

static Json::Value workflow_aggregate_collect_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  Json::Value out(Json::objectValue);
  out["kind"] = "aggregate";
  out["mode"] = "collect";
  out["ok"] = false;

  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    out["error"] = out_error ? *out_error : "aggregate config must be an object";
    return out;
  }

  std::vector<std::string> task_ids;
  if (!string_array_from_json(agg["task_ids"], &task_ids) || task_ids.empty()) {
    if (out_error) *out_error = "aggregate.task_ids must be a non-empty array of strings";
    out["error"] = out_error ? *out_error : "aggregate.task_ids must be a non-empty array of strings";
    return out;
  }

  std::vector<std::string> ptrs;
  if (agg.isMember("pointers")) (void)string_array_from_json(agg["pointers"], &ptrs);
  if (ptrs.empty()) ptrs.push_back("/assistant_text");

  const bool require_all =
    !agg.isMember("require_all") || (agg["require_all"].isBool() && agg["require_all"].asBool());

  Json::Value arr(Json::arrayValue);
  for (const auto& id : task_ids) arr.append(id);
  out["task_ids"] = arr;
  Json::Value parr(Json::arrayValue);
  for (const auto& p : ptrs) parr.append(p);
  out["pointers"] = parr;
  out["require_all"] = require_all;

  Json::Value collected(Json::objectValue);
  bool ok = true;

  for (const auto& ptr : ptrs) {
    Json::Value rows(Json::arrayValue);
    for (const auto& tid : task_ids) {
      Json::Value row(Json::objectValue);
      row["task_id"] = tid;
      auto it = result_json_by_task.find(tid);
      if (it == result_json_by_task.end()) {
        row["missing"] = true;
        rows.append(row);
        if (require_all) ok = false;
        continue;
      }
      const Json::Value& root = it->second;
      const Json::Value* got = nullptr;
      if (!json_pointer_get(root, ptr, &got) || !got) {
        row["missing"] = true;
        rows.append(row);
        if (require_all) ok = false;
        continue;
      }
      row["missing"] = false;
      if (got->isString()) row["value"] = got->asString();
      else row["value"] = *got;
      rows.append(row);
    }
    collected[ptr] = rows;
  }

  out["collected"] = collected;
  out["ok"] = ok;
  if (!ok) out["error"] = "collect missing required values";
  out["assistant_text"] = json_stringify_compact_local(collected);
  return out;
}

static Json::Value workflow_aggregate_first_ok_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  Json::Value out(Json::objectValue);
  out["kind"] = "aggregate";
  out["mode"] = "first_ok";
  out["ok"] = false;

  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    out["error"] = out_error ? *out_error : "aggregate config must be an object";
    return out;
  }

  std::vector<std::string> task_ids;
  if (!string_array_from_json(agg["task_ids"], &task_ids) || task_ids.empty()) {
    if (out_error) *out_error = "aggregate.task_ids must be a non-empty array of strings";
    out["error"] = out_error ? *out_error : "aggregate.task_ids must be a non-empty array of strings";
    return out;
  }

  std::string ok_ptr = "/ok";
  if (agg.isMember("ok_pointer") && agg["ok_pointer"].isString() && !agg["ok_pointer"].asString().empty()) {
    ok_ptr = agg["ok_pointer"].asString();
  }
  std::string val_ptr = "/assistant_text";
  if (agg.isMember("value_pointer") && agg["value_pointer"].isString() && !agg["value_pointer"].asString().empty()) {
    val_ptr = agg["value_pointer"].asString();
  }

  Json::Value arr(Json::arrayValue);
  for (const auto& id : task_ids) arr.append(id);
  out["task_ids"] = arr;
  out["ok_pointer"] = ok_ptr;
  out["value_pointer"] = val_ptr;

  for (const auto& tid : task_ids) {
    auto it = result_json_by_task.find(tid);
    if (it == result_json_by_task.end()) continue;
    const Json::Value& root = it->second;
    const Json::Value* got_ok = nullptr;
    if (!json_pointer_get(root, ok_ptr, &got_ok) || !got_ok || !got_ok->isBool() || !got_ok->asBool()) continue;

    out["chosen_task_id"] = tid;
    out["chosen"] = root;
    out["ok"] = true;

    const Json::Value* got_val = nullptr;
    if (json_pointer_get(root, val_ptr, &got_val) && got_val) {
      if (got_val->isString()) out["assistant_text"] = got_val->asString();
      else out["assistant_text"] = json_stringify_compact_local(*got_val);
    } else {
      out["assistant_text"] = json_stringify_compact_local(root);
    }
    return out;
  }

  out["error"] = "no ok task found";
  out["assistant_text"] = "";
  return out;
}

static Json::Value workflow_aggregate_quorum_ok_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  Json::Value out(Json::objectValue);
  out["kind"] = "aggregate";
  out["mode"] = "quorum_ok";
  out["ok"] = false;

  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    out["error"] = out_error ? *out_error : "aggregate config must be an object";
    out["assistant_text"] = "";
    return out;
  }

  std::vector<std::string> task_ids;
  if (!string_array_from_json(agg["task_ids"], &task_ids) || task_ids.empty()) {
    if (out_error) *out_error = "aggregate.task_ids must be a non-empty array of strings";
    out["error"] = out_error ? *out_error : "aggregate.task_ids must be a non-empty array of strings";
    out["assistant_text"] = "";
    return out;
  }

  std::string ok_ptr = "/ok";
  if (agg.isMember("ok_pointer") && agg["ok_pointer"].isString() && !agg["ok_pointer"].asString().empty()) {
    ok_ptr = agg["ok_pointer"].asString();
  }
  std::string val_ptr = "/assistant_text";
  if (agg.isMember("value_pointer") && agg["value_pointer"].isString() && !agg["value_pointer"].asString().empty()) {
    val_ptr = agg["value_pointer"].asString();
  }

  int quorum = (int)task_ids.size();
  if (agg.isMember("quorum") && agg["quorum"].isInt()) quorum = agg["quorum"].asInt();
  if (quorum < 1) quorum = 1;
  if (quorum > (int)task_ids.size()) quorum = (int)task_ids.size();

  Json::Value arr(Json::arrayValue);
  for (const auto& id : task_ids) arr.append(id);
  out["task_ids"] = arr;
  out["ok_pointer"] = ok_ptr;
  out["value_pointer"] = val_ptr;
  out["quorum"] = quorum;

  Json::Value ok_task_ids(Json::arrayValue);
  Json::Value missing_task_ids(Json::arrayValue);
  Json::Value ok_by_task(Json::objectValue);
  int ok_count = 0;

  std::string chosen_task_id;
  Json::Value chosen_root(Json::nullValue);

  for (const auto& tid : task_ids) {
    auto it = result_json_by_task.find(tid);
    if (it == result_json_by_task.end()) {
      missing_task_ids.append(tid);
      continue;
    }
    const Json::Value& root = it->second;
    const Json::Value* got_ok = nullptr;
    bool ok = false;
    if (json_pointer_get(root, ok_ptr, &got_ok) && got_ok && got_ok->isBool()) ok = got_ok->asBool();
    ok_by_task[tid] = ok;
    if (!ok) continue;
    ok_count += 1;
    ok_task_ids.append(tid);
    if (chosen_task_id.empty()) {
      chosen_task_id = tid;
      chosen_root = root;
    }
  }

  out["ok_count"] = ok_count;
  out["ok_task_ids"] = ok_task_ids;
  out["missing_task_ids"] = missing_task_ids;
  out["ok_by_task"] = ok_by_task;

  if (ok_count < quorum) {
    out["error"] = "quorum not met";
    out["assistant_text"] = "";
    return out;
  }

  out["ok"] = true;
  out["chosen_task_id"] = chosen_task_id;
  out["chosen"] = chosen_root;

  const Json::Value* got_val = nullptr;
  if (json_pointer_get(chosen_root, val_ptr, &got_val) && got_val) {
    if (got_val->isString()) out["assistant_text"] = got_val->asString();
    else out["assistant_text"] = json_stringify_compact_local(*got_val);
  } else {
    out["assistant_text"] = json_stringify_compact_local(chosen_root);
  }
  return out;
}

static Json::Value workflow_aggregate_strict_all_ok_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  Json::Value out(Json::objectValue);
  out["kind"] = "aggregate";
  out["mode"] = "strict_all_ok";
  out["ok"] = false;

  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    out["error"] = out_error ? *out_error : "aggregate config must be an object";
    out["assistant_text"] = "";
    return out;
  }

  std::vector<std::string> task_ids;
  if (!string_array_from_json(agg["task_ids"], &task_ids) || task_ids.empty()) {
    if (out_error) *out_error = "aggregate.task_ids must be a non-empty array of strings";
    out["error"] = out_error ? *out_error : "aggregate.task_ids must be a non-empty array of strings";
    out["assistant_text"] = "";
    return out;
  }

  std::string ok_ptr = "/ok";
  if (agg.isMember("ok_pointer") && agg["ok_pointer"].isString() && !agg["ok_pointer"].asString().empty()) {
    ok_ptr = agg["ok_pointer"].asString();
  }
  std::string val_ptr = "/assistant_text";
  if (agg.isMember("value_pointer") && agg["value_pointer"].isString() && !agg["value_pointer"].asString().empty()) {
    val_ptr = agg["value_pointer"].asString();
  }

  Json::Value arr(Json::arrayValue);
  for (const auto& id : task_ids) arr.append(id);
  out["task_ids"] = arr;
  out["ok_pointer"] = ok_ptr;
  out["value_pointer"] = val_ptr;

  Json::Value ok_by_task(Json::objectValue);
  Json::Value missing_task_ids(Json::arrayValue);
  Json::Value not_ok_task_ids(Json::arrayValue);

  std::string chosen_task_id;
  Json::Value chosen_root(Json::nullValue);

  bool all_ok = true;
  for (const auto& tid : task_ids) {
    auto it = result_json_by_task.find(tid);
    if (it == result_json_by_task.end()) {
      missing_task_ids.append(tid);
      all_ok = false;
      continue;
    }
    const Json::Value& root = it->second;
    const Json::Value* got_ok = nullptr;
    bool ok = false;
    if (json_pointer_get(root, ok_ptr, &got_ok) && got_ok && got_ok->isBool()) ok = got_ok->asBool();
    ok_by_task[tid] = ok;
    if (!ok) {
      not_ok_task_ids.append(tid);
      all_ok = false;
      continue;
    }
    if (chosen_task_id.empty()) {
      chosen_task_id = tid;
      chosen_root = root;
    }
  }

  out["missing_task_ids"] = missing_task_ids;
  out["not_ok_task_ids"] = not_ok_task_ids;
  out["ok_by_task"] = ok_by_task;

  if (!all_ok) {
    out["error"] = "not all tasks are ok";
    out["assistant_text"] = "";
    return out;
  }

  out["ok"] = true;
  out["chosen_task_id"] = chosen_task_id;
  out["chosen"] = chosen_root;

  const Json::Value* got_val = nullptr;
  if (json_pointer_get(chosen_root, val_ptr, &got_val) && got_val) {
    if (got_val->isString()) out["assistant_text"] = got_val->asString();
    else out["assistant_text"] = json_stringify_compact_local(*got_val);
  } else {
    out["assistant_text"] = json_stringify_compact_local(chosen_root);
  }
  return out;
}

static Json::Value workflow_aggregate_best_of_n_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  Json::Value out(Json::objectValue);
  out["kind"] = "aggregate";
  out["mode"] = "best_of_n";
  out["ok"] = false;

  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    out["error"] = out_error ? *out_error : "aggregate config must be an object";
    return out;
  }

  std::vector<std::string> task_ids;
  if (!string_array_from_json(agg["task_ids"], &task_ids) || task_ids.empty()) {
    if (out_error) *out_error = "aggregate.task_ids must be a non-empty array of strings";
    out["error"] = out_error ? *out_error : "aggregate.task_ids must be a non-empty array of strings";
    return out;
  }

  std::string ok_ptr = "/ok";
  if (agg.isMember("ok_pointer") && agg["ok_pointer"].isString() && !agg["ok_pointer"].asString().empty()) {
    ok_ptr = agg["ok_pointer"].asString();
  }
  bool require_ok = true;
  if (agg.isMember("require_ok") && agg["require_ok"].isBool()) require_ok = agg["require_ok"].asBool();

  std::string cand_ptr = "/assistant_text";
  if (agg.isMember("candidate_pointer") && agg["candidate_pointer"].isString() && !agg["candidate_pointer"].asString().empty()) {
    cand_ptr = agg["candidate_pointer"].asString();
  }
  bool parse_json = true;
  if (agg.isMember("parse_json") && agg["parse_json"].isBool()) parse_json = agg["parse_json"].asBool();

  std::string score_ptr = "/score";
  if (agg.isMember("score_pointer") && agg["score_pointer"].isString() && !agg["score_pointer"].asString().empty()) {
    score_ptr = agg["score_pointer"].asString();
  }
  std::string val_ptr = "/answer";
  if (agg.isMember("value_pointer") && agg["value_pointer"].isString() && !agg["value_pointer"].asString().empty()) {
    val_ptr = agg["value_pointer"].asString();
  }
  bool maximize = true;
  if (agg.isMember("maximize") && agg["maximize"].isBool()) maximize = agg["maximize"].asBool();

  bool has_default_score = false;
  double default_score = 0.0;
  if (agg.isMember("default_score")) {
    has_default_score = json_value_to_double_best_effort(agg["default_score"], &default_score);
  }

  Json::Value arr(Json::arrayValue);
  for (const auto& id : task_ids) arr.append(id);
  out["task_ids"] = arr;
  out["ok_pointer"] = ok_ptr;
  out["require_ok"] = require_ok;
  out["candidate_pointer"] = cand_ptr;
  out["parse_json"] = parse_json;
  out["score_pointer"] = score_ptr;
  out["value_pointer"] = val_ptr;
  out["maximize"] = maximize;
  if (has_default_score) out["default_score"] = default_score;

  Json::Value candidates(Json::arrayValue);

  bool found = false;
  double best_score = 0.0;
  std::string best_task_id;
  Json::Value best_root;
  Json::Value best_candidate;
  Json::Value best_value;

  for (const auto& tid : task_ids) {
    Json::Value row(Json::objectValue);
    row["task_id"] = tid;

    auto it = result_json_by_task.find(tid);
    if (it == result_json_by_task.end()) {
      row["missing"] = true;
      candidates.append(row);
      continue;
    }
    row["missing"] = false;
    const Json::Value& root = it->second;

    bool root_ok = false;
    const Json::Value* got_ok = nullptr;
    if (json_pointer_get(root, ok_ptr, &got_ok) && got_ok && got_ok->isBool()) root_ok = got_ok->asBool();
    row["ok"] = root_ok;
    if (require_ok && !root_ok) {
      row["eligible"] = false;
      candidates.append(row);
      continue;
    }

    const Json::Value* got_cand = nullptr;
    if (!json_pointer_get(root, cand_ptr, &got_cand) || !got_cand) {
      row["eligible"] = false;
      row["missing_candidate"] = true;
      candidates.append(row);
      continue;
    }

    Json::Value cand = *got_cand;
    if (parse_json && cand.isString()) {
      const std::string s = cand.asString();
      Json::Value parsed;
      std::string perr;
      if (!json_parse_any_value(s, &parsed, &perr)) {
        row["eligible"] = false;
        row["parse_error"] = perr;
        candidates.append(row);
        continue;
      }
      cand = parsed;
    }

    const Json::Value* got_score = nullptr;
    double score = 0.0;
    bool has_score = false;
    if (json_pointer_get(cand, score_ptr, &got_score) && got_score) {
      has_score = json_value_to_double_best_effort(*got_score, &score);
    }
    if (!has_score && has_default_score) {
      has_score = true;
      score = default_score;
    }

    if (!has_score) {
      row["eligible"] = false;
      row["missing_score"] = true;
      candidates.append(row);
      continue;
    }

    const Json::Value* got_val = nullptr;
    Json::Value val = cand;
    if (json_pointer_get(cand, val_ptr, &got_val) && got_val) val = *got_val;

    row["eligible"] = true;
    row["score"] = score;
    if (val.isString()) row["value"] = val.asString();
    else row["value"] = val;
    candidates.append(row);

    if (!found) {
      found = true;
      best_score = score;
      best_task_id = tid;
      best_root = root;
      best_candidate = cand;
      best_value = val;
      continue;
    }
    if (maximize) {
      if (score > best_score) {
        best_score = score;
        best_task_id = tid;
        best_root = root;
        best_candidate = cand;
        best_value = val;
      }
    } else {
      if (score < best_score) {
        best_score = score;
        best_task_id = tid;
        best_root = root;
        best_candidate = cand;
        best_value = val;
      }
    }
  }

  out["candidates"] = candidates;
  if (!found) {
    out["error"] = "no eligible candidates found";
    out["assistant_text"] = "";
    return out;
  }

  out["ok"] = true;
  out["chosen_task_id"] = best_task_id;
  out["chosen_score"] = best_score;
  out["chosen"] = best_root;
  out["chosen_candidate"] = best_candidate;
  out["chosen_value"] = best_value;
  if (best_value.isString()) out["assistant_text"] = best_value.asString();
  else out["assistant_text"] = json_stringify_compact_local(best_value);
  return out;
}

}  // namespace

Json::Value workflow_aggregate_to_json(
  const Json::Value& agg,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!agg.isObject()) {
    if (out_error) *out_error = "aggregate config must be an object";
    Json::Value o(Json::objectValue);
    o["kind"] = "aggregate";
    o["ok"] = false;
    o["error"] = out_error ? *out_error : "aggregate config must be an object";
    o["assistant_text"] = "";
    return o;
  }
  const std::string mode =
    agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : "quorum_hashes";
  if (mode == "collect") return workflow_aggregate_collect_to_json(agg, result_json_by_task, out_error);
  if (mode == "first_ok") return workflow_aggregate_first_ok_to_json(agg, result_json_by_task, out_error);
  if (mode == "quorum_ok") return workflow_aggregate_quorum_ok_to_json(agg, result_json_by_task, out_error);
  if (mode == "strict_all_ok") return workflow_aggregate_strict_all_ok_to_json(agg, result_json_by_task, out_error);
  if (mode == "best_of_n") return workflow_aggregate_best_of_n_to_json(agg, result_json_by_task, out_error);
  // Default: quorum_hashes (also used when mode omitted).
  return workflow_aggregate_quorum_hashes_to_json(agg, result_json_by_task, out_error);
}

}  // namespace agentd
