#include "memory_view.h"
#include "memory_index.h"
#include "memory_salience.h"
#include "run_memory_context.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
using namespace host_tools_internal;
namespace fs = std::filesystem;
void check(bool ok, const std::string &why) {
  if (!ok) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
std::string doc_text(const Json::Value &items) {
  Json::Value doc;
  doc["schema"] = "agent_memory_v2";
  doc["items"] = items;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return "# Generated duplicate must not enter retrieval\n## Deprecated\nrenderedstale\n<!-- "
         "AGENT_MEMORY_V1_BEGIN -->\n" +
         Json::writeString(writer, doc) + "\n<!-- AGENT_MEMORY_V1_END -->\n";
}
int main() {
  const auto root =
      fs::temp_directory_path() /
      ("memory-view-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root / "sessions");
  fs::create_directories(root / "recaps");
  std::string error;
  Json::Value items;
  auto record = [](const std::string &value, const std::string &status = "active") {
    Json::Value r;
    r["kind"] = "fact";
    r["value"] = value;
    r["status"] = status;
    r["updated_utc"] = "2026-01-01T00:00:00Z";
    return r;
  };
  items["bridge/A~B"] = record("currentneedle 中文 foo_bar");
  items["bridge/A~B"]["versions"][0] = record("oldneedle");
  items["retired"] = record("retiredneedle", "deprecated");
  items["unknown"] = record("unknownneedle", "unsupported");
  items["future"] = record("futureneedle");
  items["future"]["valid_from"] = "9999-01-01T00:00:00Z";
  items["malformed"] = record("malformedneedle");
  items["malformed"]["status"] = Json::Value(Json::arrayValue);
  const auto publish = [&](const std::string &text, const std::string &expected) {
    MemoryDocumentTransaction tx;
    check(tx.begin(root, "STRUCTURED.md", expected, {}, &error) == MemoryStoreStatus::Ok, error);
    check(tx.commit(text, &error) == MemoryStoreStatus::Ok, error);
    return tx.document();
  };
  auto original = publish(doc_text(items), "absent");
  {
    std::ofstream file(root / "MEMORY.md");
    file << "# Current\nrawneedle\n\n## Deprecated\nrawstale\n\n## Active\nrawagain\n```\n## "
            "Deprecated\ncode evidence\n```\ncurrentaftercode\n- @mem deprecated "
            "old.key = markerstale\n- @mem fact bridge/A~B = rawoldneedle\n";
  }
  {
    std::ofstream file(root / "sessions" / "other.md");
    file << "privateotherneedle";
  }
  {
    std::ofstream file(root / "recaps" / "recap_latest.json");
    file << "{\"ts_utc\":\"9999-01-01T00:00:00Z\",\"summary_text\":\"unversionedrecapstale\"}";
  }
  std::vector<std::string> files = {(root / "STRUCTURED.md").string(), (root / "MEMORY.md").string()};
  for (const bool index : {false, true}) {
    auto query = [&](const std::string &q, MemoryViewMode view, bool present) {
      std::vector<MemorySearchHit> hits;
      std::string engine, index_error;
      check(
          memory_retrieve(root, files, q, view, index, false, 20, 600, &hits, &engine, &error, &index_error),
          error);
      check(!hits.empty() == present, "wrong eligibility: " + q + " index=" + std::to_string(index));
      if (index && q == "currentneedle")
        check(engine == "fts5", "FTS path was not exercised: " + index_error);
      return hits;
    };
    auto hits = query("currentneedle", MemoryViewMode::Current, true);
    const auto reference = memory_passage_evidence(hits[0].passage);
    std::string text;
    Json::Value value;
    check(memory_evidence_read(root, reference, &text, &value, &error) &&
              value.asString() == "currentneedle 中文 foo_bar",
          error);
    for (const auto &term :
         {"oldneedle", "retiredneedle", "unknownneedle", "futureneedle", "malformedneedle", "renderedstale",
          "rawstale", "markerstale", "rawoldneedle", "privateotherneedle"})
      query(term, MemoryViewMode::Current, false);
    query("oldneedle", MemoryViewMode::Historical, true);
    query("retiredneedle", MemoryViewMode::Evidence, true);
    query("rawstale", MemoryViewMode::Historical, true);
    query("中文", MemoryViewMode::Current, true);
    query("foo_bar", MemoryViewMode::Current, true);
    query("rawagain", MemoryViewMode::Current, true);
    query("currentaftercode", MemoryViewMode::Current, true);
  }
  {
    std::vector<MemorySearchHit> scoped;
    std::string scoped_engine;
    check(memory_retrieve(root, {(root / "sessions" / "other.md").string()}, "privateotherneedle",
                          MemoryViewMode::Current, true, false, 20, 600, &scoped, &scoped_engine, &error) &&
              scoped.size() == 1,
          "wide cache fixture failed");
    check(memory_retrieve(root, files, "privateotherneedle", MemoryViewMode::Current, true, false, 20, 600,
                          &scoped, &scoped_engine, &error) &&
              scoped.empty(),
          "previous cache scope leaked into current query");
  }
  MemoryViewSnapshot captured;
  check(memory_view_capture(root, files, MemoryViewMode::Current, &captured, &error), error);
  const auto evidence = memory_passage_evidence(captured.passages[0]);
  const auto timestamp = fs::last_write_time(root / "STRUCTURED.md");
  items["bridge/A~B"]["value"] = "correctedneedle";
  auto corrected = publish(doc_text(items), original.revision);
  fs::last_write_time(root / "STRUCTURED.md", timestamp);
  check(!memory_view_validate(captured, &error), "concurrent correction was not detected");
  std::vector<MemorySearchHit> hits;
  std::string engine;
  check(!memory_retrieve_snapshot(captured, "currentneedle", true, false, 20, 600, &hits, &engine, &error) &&
            hits.empty(),
        "stale captured search emitted results");
  check(memory_retrieve(root, files, "currentneedle", MemoryViewMode::Current, true, false, 20, 600, &hits,
                        &engine, &error) &&
            hits.empty(),
        "mtime-preserving correction left stale indexed content");
  check(memory_retrieve(root, files, "correctedneedle", MemoryViewMode::Current, true, false, 20, 600, &hits,
                        &engine, &error) &&
            hits.size() == 1,
        "correction missing from refreshed index");
  std::string historical;
  Json::Value value;
  check(memory_evidence_read(root, evidence, &historical, &value, &error) &&
            value.asString() == "currentneedle 中文 foo_bar",
        "reference drifted to current path");
  check(!memory_evidence_read(root / "sessions", evidence, &historical, &value, &error),
        "cross-scope reference accepted");
  fs::create_directories(root / "other-scope");
  {
    std::ofstream same_bytes(root / "other-scope" / "STRUCTURED.md");
    same_bytes << original.text;
  }
  check(!memory_evidence_read(root / "other-scope", evidence, &historical, &value, &error),
        "same bytes accepted another scope's source ID");
  auto altered = evidence;
  altered["span"]["pointer"] = "/items/retired/value";
  check(!memory_evidence_read(root, altered, &historical, &value, &error),
        "altered span retained original evidence ID");
  for (const auto mode : {agentd::MemoryContextMode::Files, agentd::MemoryContextMode::Index,
                          agentd::MemoryContextMode::Search, agentd::MemoryContextMode::Salience}) {
    agentd::MemoryContextPolicy policy;
    policy.mode = mode;
    policy.include_daily = false;
    policy.total_cap = 40000;
    std::string context;
    check(agentd::build_memory_context_text(root.string(), root.parent_path().string(), "mine", policy,
                                            "correctedneedle", &context),
          "context missing");
    for (const auto &term : {"oldneedle", "retiredneedle", "rawstale", "markerstale", "unversionedrecapstale",
                             "privateotherneedle"})
      check(context.find(term) == std::string::npos, std::string("context leaked ") + term);
    check(context.size() <= policy.total_cap, "context cap exceeded");
  }
  agentd::MemorySaliencePolicy policy;
  policy.include_daily = false;
  agentd::MemorySalienceReport report;
  check(agentd::memory_salience_collect(root, policy, &report, &error), error);
  check(report.structured_items.size() == 1 && report.structured_items[0].text == "correctedneedle",
        "salience did not use only active current head");
  fs::remove(root / "STRUCTURED.md");
  check(memory_retrieve(root, files, "correctedneedle", MemoryViewMode::Current, true, false, 20, 600, &hits,
                        &engine, &error) &&
            hits.empty(),
        "deleted source survived in index");
  fs::create_symlink(root / "sessions" / "other.md", root / "linked.md");
  check(
      !memory_view_capture(root, {(root / "linked.md").string()}, MemoryViewMode::Current, &captured, &error),
      "linked source bypassed scope");
  check(!memory_view_capture(root, {(root / ".." / "outside.md").string()}, MemoryViewMode::Current,
                             &captured, &error),
        "outside path accepted");
  {
    std::ofstream file(root / "too-many.md");
    for (int i = 0; i < 20001; ++i)
      file << "line\n";
  }
  check(!memory_view_capture(root, {(root / "too-many.md").string()}, MemoryViewMode::Current, &captured,
                             &error),
        "line budget silently truncated a source");
  check(!memory_view_capture(root, std::vector<std::string>(129, (root / "MEMORY.md").string()),
                             MemoryViewMode::Current, &captured, &error),
        "selected source budget not enforced");
  {
    std::ofstream damaged(root / "STRUCTURED.md");
    damaged << "<!-- AGENT_MEMORY_V1_BEGIN -->\nbadfact";
  }
  agentd::MemoryContextPolicy incomplete_policy;
  std::string incomplete;
  check(agentd::build_memory_context_text(root.string(), root.parent_path().string(), "mine",
                                          incomplete_policy, "needle", &incomplete),
        "retrieval failure was silently omitted");
  check(incomplete.rfind("DURABLE_MEMORY_UNAVAILABLE", 0) == 0 &&
            incomplete.find("badfact") == std::string::npos,
        "retrieval failure injected unverified data");
  fs::remove_all(root);
  std::cout << "Currentness, scope, original references, FTS/fallback, context and correction/deletion "
               "checks passed\n";
}
