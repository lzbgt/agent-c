#include "memory_correlation_index.h"
#include "memory_derivation.h"
#include "memory_index.h"
#include "memory_store.h"
#include "memory_structured.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif
using namespace host_tools_internal;
namespace fs = std::filesystem;
void check(bool ok, const std::string &why) {
  if (!ok) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
std::string compact(const Json::Value &value) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, value);
}
std::string structured(const Json::Value &items, const std::string &schema = "agent_memory_v2") {
  Json::Value doc;
  doc["schema"] = schema;
  doc["items"] = items;
  return "<!-- AGENT_MEMORY_V1_BEGIN -->\n" + compact(doc) + "\n<!-- AGENT_MEMORY_V1_END -->\n";
}
int main() {
  const auto root = fs::temp_directory_path() /
                    ("memory-cache-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string error;
  auto publish = [&](const std::string &path, const std::string &text,
                     const std::string &revision) {
    MemoryDocumentTransaction tx;
    check(tx.begin(root, path, revision, {}, &error) == MemoryStoreStatus::Ok, error);
    check(tx.commit(text, &error) == MemoryStoreStatus::Ok, error);
    return tx.document();
  };
  auto query = [&](const std::string &term, MemoryViewMode mode, bool present) {
    std::vector<MemorySearchHit> hits;
    std::string engine, index_error;
    check(memory_retrieve(root, {(root / "MEMORY.md").string()}, term, mode, true, false, 8, 600,
                          &hits, &engine, &error, &index_error),
          error);
    check(engine == "fts5", "FTS path missing: " + index_error);
    check(hits.empty() != present, "wrong indexed eligibility: " + term);
  };
  auto original =
      publish("MEMORY.md", "cachedeletionneedle\n\n## Deprecated\nhistoryonlyneedle\n", "absent");
  query("historyonlyneedle", MemoryViewMode::Historical, true);
  query("cachedeletionneedle", MemoryViewMode::Current, true);
#if defined(AGENT_HAVE_SQLITE3)
  {
    sqlite3 *db = nullptr;
    check(sqlite3_open((root / ".memory_retrieval_v2.sqlite3").c_str(), &db) == SQLITE_OK,
          "cannot inspect FTS cache");
    sqlite3_stmt *stmt = nullptr;
    check(sqlite3_prepare_v2(db, "SELECT v FROM eligible_passages_config WHERE k='secure-delete'",
                             -1, &stmt, nullptr) == SQLITE_OK,
          "FTS configuration missing");
    check(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1,
          "FTS secure-delete not active");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    std::ifstream input(root / ".memory_retrieval_v2.sqlite3", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    check(bytes.find("historyonlyneedle") == std::string::npos,
          "previous historical cache payload remained after current rebuild");
  }
#endif
  MemoryViewSnapshot captured;
  check(memory_view_capture(root, {(root / "MEMORY.md").string()}, MemoryViewMode::Current,
                            &captured, &error),
        error);
  Json::Value refs(Json::arrayValue);
  refs.append(memory_passage_evidence(captured.passages[0]));
  const auto derivation = memory_derivation_make(captured, refs);
  check(memory_derivation_evaluate(derivation, captured)["state"] == "current",
        "new derivation not current");
  auto bad = derivation;
  bad["as_of_utc"] = "2026-02-30T00:00:00Z";
  check(memory_derivation_evaluate(bad, captured)["state"] == "unverified",
        "invalid manifest calendar accepted");
  bad = derivation;
  bad["input_revisions"].append(bad["input_revisions"][0]);
  check(memory_derivation_evaluate(bad, captured)["state"] == "unverified",
        "duplicate inputs accepted");
  bad = derivation;
  bad["input_revisions"][0]["source_id"] = "another-scope";
  check(memory_derivation_evaluate(bad, captured)["state"] == "unverified",
        "wrong input scope accepted");
  bad = derivation;
  bad["evidence"][0]["span"]["end"] = 0;
  check(memory_derivation_evaluate(bad, captured)["state"] == "stale", "altered span accepted");
  check(memory_derivation_evaluate(Json::nullValue, captured)["state"] == "unverified",
        "missing manifest accepted");
  const std::vector<std::string> caches = {".memory_index.sqlite3", ".memory_retrieval_v1.sqlite3",
                                           ".memory_correlation.sqlite3",
                                           ".memory_retrieval_v2.sqlite3"};
  for (const auto &name : caches)
    for (const auto *suffix : {"", "-wal", "-shm", "-journal"})
      if (!fs::exists(root / (name + suffix)))
        std::ofstream(root / (name + suffix)) << "disposablecachepayload";
  std::ofstream(root / ".memory_correlation.json") << "disposablecachepayload";
  std::ofstream(root / "unrelated.sqlite3") << "unrelated";
  auto corrected = publish("MEMORY.md", "corrected\n", original.revision);
  for (const auto &name : caches)
    for (const auto *suffix : {"", "-wal", "-shm", "-journal"})
      check(!fs::exists(root / (name + suffix)),
            "publication left derived cache: " + name + suffix);
  check(!fs::exists(root / ".memory_correlation.json") && fs::exists(root / "unrelated.sqlite3"),
        "cache purge crossed ownership boundary");
  for (const auto &entry : fs::recursive_directory_iterator(root / ".revisions"))
    if (entry.is_regular_file()) {
      std::ifstream f(entry.path());
      std::string bytes((std::istreambuf_iterator<char>(f)), {});
      check(bytes.find("disposablecachepayload") == std::string::npos,
            "cache payload was archived");
    }
  MemoryViewSnapshot current;
  check(memory_derivation_capture(root, {derivation}, &current, &error), error);
  check(memory_derivation_evaluate(derivation, current)["state"] == "stale",
        "correction did not invalidate derived state");
  std::string text;
  Json::Value value;
  check(memory_evidence_read(root, refs[0], &text, &value, &error) &&
            text.find("cachedeletionneedle") != std::string::npos,
        "original evidence lost after correction");
  {
    MemoryCacheLease held;
    check(held.begin(root, {}, &error) == MemoryStoreStatus::Ok, error);
    MemoryDocumentTransaction tx;
    check(tx.begin(root, "MEMORY.md", corrected.revision, {}, &error) == MemoryStoreStatus::Ok,
          error);
    const auto start = std::chrono::steady_clock::now();
    check(tx.commit("blocked\n", &error) == MemoryStoreStatus::Busy && !tx.published(),
          "cache contention published a source");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
          "cache contention exceeded deadline");
    std::vector<MemorySearchHit> hits;
    std::string engine, index_error;
    check(memory_retrieve(root, {(root / "MEMORY.md").string()}, "corrected",
                          MemoryViewMode::Current, true, false, 8, 600, &hits, &engine, &error,
                          &index_error) &&
              engine == "substr" && !index_error.empty() && hits.size() == 1,
          "cache busy did not expose lexical fallback");
  }
  MemoryDocument unchanged;
  check(memory_document_read(root, "MEMORY.md", &unchanged, &error) == MemoryStoreStatus::Ok &&
            unchanged.revision == corrected.revision,
        "busy commit changed primary");
  fs::create_directory(root / ".memory_correlation.json");
  {
    MemoryDocumentTransaction tx;
    check(tx.begin(root, "MEMORY.md", corrected.revision, {}, &error) == MemoryStoreStatus::Ok,
          error);
    check(tx.commit("blocked again", &error) == MemoryStoreStatus::Unavailable && !tx.published(),
          "invalid cache type allowed publication");
  }
  fs::remove(root / ".memory_correlation.json");
  auto recovered = publish("MEMORY.md", "recovered\n", corrected.revision);
  query("recovered", MemoryViewMode::Current, true);
  {
    MemoryDocumentTransaction tx;
    check(tx.begin(root, "MEMORY.md", recovered.revision, {}, &error) == MemoryStoreStatus::Ok,
          error);
    check(tx.retire(&error) == MemoryStoreStatus::Ok, error);
  }
  check(!fs::exists(root / ".memory_retrieval_v2.sqlite3"), "retirement did not purge cache");
  check(memory_derivation_capture(root, {derivation}, &current, &error), error);
  check(memory_derivation_evaluate(derivation, current)["state"] == "stale",
        "retirement did not invalidate derivation");
  // Decoder: unknown schema remains available as exact historical evidence.
  Json::Value items;
  items["key"]["kind"] = "fact";
  items["key"]["value"] =
      "literal <!-- AGENT_MEMORY_V1_END --> <!-- AGENT_MEMORY_V1_NOTES_BEGIN --> 中文";
  items["key"]["source"] = "legacy-source";
  items["key"]["updated_utc"] = "2026-01-01T00:00:00Z";
  auto aliased = items["key"];
  aliased["valid_from_utc"] = "9999-01-01T00:00:00Z";
  check(!memory_record_current(aliased, memory_view_now_utc()),
        "legacy time alias bypassed current eligibility");
  const auto stored = publish("STRUCTURED.md", structured(items), "absent");
  Json::Value decoded;
  check(memory_structured_parse_doc(stored.text, &decoded, &error) && decoded["items"] == items,
        "structured parse changed stored fields");
  MemoryViewSnapshot exact;
  check(memory_view_capture(root, {(root / "STRUCTURED.md").string()}, MemoryViewMode::Current,
                            &exact, &error) &&
            exact.passages.size() == 1,
        error);
  auto evidence = memory_passage_evidence(exact.passages[0]);
  check(memory_evidence_read(root, evidence, &text, &value, &error) &&
            value == items["key"]["value"],
        "marker inside JSON string broke evidence");
  auto synthesized = exact.passages[0];
  synthesized.pointer = "/items/key/sources";
  check(!memory_evidence_read(root, memory_passage_evidence(synthesized), &text, &value, &error),
        "evidence manufactured missing sources");
  const auto legacy =
      publish("STRUCTURED.md", structured(items, "unsupported_future"), stored.revision);
  check(!memory_view_capture(root, {(root / "STRUCTURED.md").string()}, MemoryViewMode::Current,
                             &exact, &error),
        "unsupported schema entered current");
  check(memory_view_capture(root, {(root / "STRUCTURED.md").string()}, MemoryViewMode::Historical,
                            &exact, &error) &&
            exact.passages.size() == 1 && exact.passages[0].historical,
        error);
  check(memory_evidence_read(root, memory_passage_evidence(exact.passages[0]), &text, &value,
                             &error) &&
            value == items["key"]["value"],
        "unsupported original could not be read");
  check(!memory_structured_decode("<!-- AGENT_MEMORY_V1_BEGIN "
                                  "-->\n{\"items\":{},\"items\":{}}\n<!-- AGENT_MEMORY_V1_END -->",
                                  &decoded, &error),
        "ambiguous duplicate JSON keys accepted");
  check(
      agentd::memory_correlation_source_matches("trace:sample:task:X workflow:w", "trace:sample") &&
          !agentd::memory_correlation_source_matches("trace:sample-other", "trace:sample"),
      "correlation token boundary failed");
  const auto outcome =
      publish("recaps/recap_original.json",
              "{\"model_response\":\"literal <!-- AGENT_MEMORY_V1_BEGIN -->\"}", "absent");
  MemoryPassage response;
  response.path = "recaps/recap_original.json";
  response.source_id = outcome.source_id;
  response.revision = outcome.revision;
  response.pointer = "/model_response";
  check(memory_evidence_read(root, memory_passage_evidence(response), &text, &value, &error) &&
            value == "literal <!-- AGENT_MEMORY_V1_BEGIN -->",
        "literal marker in JSON artifact broke original evidence");
  fs::remove_all(root);
  std::cout << "Cache purge, contention/fallback, derivation invalidation and exact original "
               "decoding passed\n";
}
