#include "memory_archives.h"
#include "memory_consolidator.h"
#include "memory_index.h"
#include "memory_promotion.h"
#include "memory_salience.h"
#include "memory_structured.h"
#include "run_memory_context.h"
#include "workflow_memory_query.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
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
Json::Value parse(const std::string &text) {
  Json::CharReaderBuilder r;
  Json::Value v;
  std::string e;
  std::istringstream in(text);
  check(Json::parseFromStream(r, in, &v, &e), e);
  return v;
}
struct Fixture {
  fs::path root, memory;
  HostToolsetConfig config;
  std::string error;
  Fixture() {
    root = fs::temp_directory_path() /
           ("memory-promotion-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    memory = root / "memory";
    fs::create_directories(memory);
    fs::create_directories(root / "sessions");
    config.root_dir = root.string();
    config.sessions_root_dir = (root / "sessions").string();
    config.policy = HostToolsetPolicyMode::Full;
  }
  ~Fixture() { fs::remove_all(root); }
  void raw(const std::string &path, const std::string &text) {
    fs::create_directories((memory / path).parent_path());
    std::ofstream out(memory / path, std::ios::binary);
    out << text;
    check(bool(out), "fixture write");
  }
  MemoryDocument read(const std::string &path = "STRUCTURED.md") {
    MemoryDocument d;
    check(memory_document_read(memory, path, &d, &error) == MemoryStoreStatus::Ok, error);
    return d;
  }
  Json::Value doc() {
    Json::Value d;
    check(memory_structured_parse_doc(read().text, &d, &error), error);
    return d;
  }
  MemoryPromotionPlan capture(std::vector<std::string> paths = {"MEMORY.md"},
                              size_t max_keys = 256) {
    for (auto &path : paths)
      path = (memory / path).string();
    MemoryPromotionPlan plan;
    check(memory_promotion_capture(memory, paths, kMemoryDocumentMaxBytes, max_keys, &plan, &error),
          error);
    return plan;
  }
  void save(const Json::Value &d) {
    raw("STRUCTURED.md",
        "<!-- AGENT_MEMORY_V1_BEGIN -->\n" + compact(d) + "\n<!-- AGENT_MEMORY_V1_END -->\n");
  }
  void fill_receipts() {
    auto d = doc();
    const auto receipt =
        d["promotion_ledger"]["receipts"][d["promotion_ledger"]["receipts"].getMemberNames()[0]];
    for (size_t i = 0; d["promotion_ledger"]["receipts"].size() < kMemoryPromotionReceiptLimit;
         ++i) {
      auto id = std::string("marker:") + std::string(64, '0');
      const auto digits = std::to_string(i);
      id.replace(id.size() - digits.size(), digits.size(), digits);
      if (d["promotion_ledger"]["consumed"].isMember(id))
        continue;
      d["promotion_ledger"]["receipts"][id] = receipt;
    }
    save(d);
  }
  fs::path archived(const std::string &revision) {
    for (const auto &file : fs::recursive_directory_iterator(memory / ".revisions"))
      if (file.path().filename() == revision.substr(7) + ".blob")
        return file.path();
    check(false, "missing original archive fixture");
    return {};
  }
  Json::Value promote(const MemoryPromotionPlan &plan) {
    Json::Value receipt;
    auto status = memory_promote(config, plan, &receipt, &error);
    check(status == AGENT_OK, "promotion transport failure: " + error);
    return receipt;
  }
  Json::Value promote() {
    auto result = promote(capture());
    check(result["ok"].asBool(), compact(result));
    return result;
  }
  Json::Value tool(const std::string &name, const Json::Value &args) {
    agent_tool_registry_t *registry = nullptr;
    agent_tool_executor_t exec{};
    check(toolset_host_create(config, &registry, &exec) == AGENT_OK, "toolset");
    agent_string_t result{};
    const auto status = exec.execute(exec.ctx, name.c_str(), compact(args).c_str(), &result);
    auto receipt = parse(result.data ? std::string(result.data, result.len) : "");
    agent_string_free(&result);
    agent_tool_registry_destroy(registry);
    toolset_host_destroy(&exec);
    check(status == AGENT_OK, "tool transport");
    return receipt;
  }
  void put(const std::string &key, const std::string &value) {
    Json::Value args;
    args["path"] = "STRUCTURED.md";
    args["expected_revision"] = read().revision;
    args["entries"][0]["key"] = key;
    args["entries"][0]["value"] = value;
    args["entries"][0]["source"] = "direct:correction";
    auto r = tool("memory_put", args);
    check(r["ok"].asBool(), compact(r));
  }
  std::string marker(const std::string &key, const std::string &value,
                     const std::string &revision) {
    Json::Value j;
    j["key"] = key;
    j["value"] = value;
    j["expected_record_revision"] = revision;
    return "@mem " + compact(j) + "\n";
  }
  MemoryViewSnapshot view(MemoryViewMode mode = MemoryViewMode::Current) {
    MemoryViewSnapshot s;
    check(memory_view_capture(memory, {(memory / "STRUCTURED.md").string()}, mode, &s, &error),
          error);
    return s;
  }
  bool has(const MemoryViewSnapshot &s, const std::string &key) {
    for (const auto &p : s.passages)
      if (p.key == key && !p.pointer.empty())
        return true;
    return false;
  }
};
int main() {
  { // Reproduce the old daemon path, then repeat after correction and history
    // rollover.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact promotion.fixture = original observation\n");
    agentd::DaemonConfig cfg;
    cfg.state_dir = f.root.string();
    cfg.sessions_root_dir = f.config.sessions_root_dir;
    cfg.tools = "host";
    cfg.host_policy = HostToolsetPolicyMode::Full;
    agentd::MemoryConsolidateOptions options;
    options.include_daily = false;
    options.include_session = false;
    options.include_structured = false;
    Json::Value report;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error), f.error);
    const auto before = f.read().revision;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error) &&
              f.read().revision == before,
          "duplicate changed head/recency");
    for (int i = 0; i < 23; ++i)
      f.put("promotion.fixture", "correction " + std::to_string(i));
    const auto corrected = f.read().revision;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error), f.error);
    check(f.doc()["items"]["promotion.fixture"]["value"] == "correction 22" &&
              f.read().revision == corrected,
          "stale marker undid correction after history rollover");
    f.raw("MEMORY.md", "unrelated append and moved lines\n\n@mem fact "
                       "promotion.fixture = original observation\n");
    check(f.promote()["data"]["no_changes"].asBool() && f.read().revision == corrected,
          "moving an old marker created a new intent");
    auto plan = f.capture();
    options.dry_run = true;
    const auto old = f.read().revision;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error) &&
              f.read().revision == old,
          "preview mutated authority");
  }
  { // Bound replacement to a per-key revision; preserve other keys and consume
    // rejected intents.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact target = old\n");
    f.promote();
    const auto base = memory_record_revision(f.doc()["items"]["target"]);
    f.put("unrelated", "other key does not invalidate base");
    f.raw("MEMORY.md", f.marker("target", "new", base));
    const auto applied = f.promote();
    check(applied["data"]["promotion"]["complete"].asBool() &&
              f.doc()["items"]["target"]["value"] == "new",
          "anchored independent-key update rejected");
    const auto history = f.doc()["items"]["target"]["versions"][0];
    check(history.isMember("derivation"), "history lost original derivation");
    f.raw("MEMORY.md",
          f.marker("target", "stale", base) + "@mem fact independent = still runnable\n");
    auto rejected = f.promote();
    check(!rejected["data"]["promotion"]["complete"].asBool() &&
              f.doc()["items"]["target"]["value"] == "new" &&
              f.doc()["items"].isMember("independent"),
          "conflict blocked independent work or overwrote head");
    const auto same = f.read().revision;
    check(!f.promote()["data"]["promotion"]["complete"].asBool() && f.read().revision == same,
          "rejected intent was retried");
    f.put("target", "direct fix");
    check(!f.doc()["items"]["target"].isMember("derivation"),
          "direct correction retained stale derivation");
    check(f.promote()["data"]["promotion"]["complete"].asBool() &&
              f.doc()["items"]["target"]["value"] == "direct fix",
          "resolved stale intent revived");
    Json::Value query;
    query["key"] = "target";
    auto queried = f.tool("memory_structured_query", query);
    check(queried["data"]["results"][0]["record_revision"] ==
              memory_record_revision(f.doc()["items"]["target"]),
          "query record revision differs from replacement base");
  }
  { // No order-dependent choice among conflicting observations, and no silent
    // truncation.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact collision = one\n@mem fact collision = two\n@mem fact "
                       "independent = ok\n@mem {\"key\":\"bad\",\"value\":4}\n");
    auto receipt = f.promote();
    check(!receipt["data"]["promotion"]["complete"].asBool() &&
              !f.doc()["items"].isMember("collision") && f.doc()["items"].isMember("independent"),
          "conflicting first observations used last-write-wins");
    check(receipt["data"]["promotion"]["errors"].size() == 1 &&
              receipt["data"]["promotion"]["unresolved"].asInt() == 2,
          "conflict/error accounting");
    Fixture bounded;
    bounded.raw("MEMORY.md", "@mem fact a = A\n@mem fact b = B\n");
    auto limited = bounded.promote(bounded.capture({"MEMORY.md"}, 1));
    check(limited["data"]["promotion"]["deferred"].asUInt64() == 1 &&
              !limited["data"]["promotion"]["complete"].asBool(),
          "key cap silently truncated");
    check(bounded.promote()["data"]["promotion"]["complete"].asBool() &&
              bounded.doc()["items"].size() == 2,
          "deferred observation lost");
  }
  { // A changed/removed supporting marker invalidates every current consumer,
    // not history.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact fact.source = staleprovenanceneedle\n");
    f.promote();
    auto snapshot = f.view();
    check(f.has(snapshot, "fact.source"), "initial derived head missing");
    const auto original = f.doc()["items"]["fact.source"]["derivation"]["markers"][0]["evidence"];
    f.raw("MEMORY.md", "unrelated text\n@mem fact fact.source = staleprovenanceneedle\n");
    check(f.has(f.view(), "fact.source"), "unrelated append invalidated stable intent");
    auto fence = f.view();
    f.raw("MEMORY.md", "source has been corrected\n");
    check(!memory_view_validate(fence, &f.error), "dependency edit passed snapshot fence");
    check(!f.has(f.view(), "fact.source"), "removed marker remained current");
    auto historical = f.view(MemoryViewMode::Historical);
    check(f.has(historical, "fact.source") && historical.passages[0].historical &&
              !historical.passages[0].eligibility_reason.empty(),
          "history did not identify invalidation");
    std::string text;
    Json::Value value;
    check(memory_evidence_read(f.memory, original, &text, &value, &f.error) &&
              text.find("@mem fact fact.source") != std::string::npos,
          "source edit lost retained original bytes");
    for (bool indexed : {false, true}) {
      std::vector<MemorySearchHit> hits;
      std::string engine;
      check(memory_retrieve(f.memory, {(f.memory / "STRUCTURED.md").string()},
                            "staleprovenanceneedle", MemoryViewMode::Current, indexed, false, 8,
                            600, &hits, &engine, &f.error) &&
                hits.empty(),
            "current lexical path exposed invalidated head");
    }
    agentd::MemorySaliencePolicy policy;
    policy.include_daily = false;
    agentd::MemorySalienceReport report;
    check(agentd::memory_salience_collect(f.memory, policy, &report, &f.error) &&
              report.structured_items.empty(),
          "salience exposed invalidated head");
    for (auto mode : {agentd::MemoryContextMode::Files, agentd::MemoryContextMode::Index,
                      agentd::MemoryContextMode::Search, agentd::MemoryContextMode::Salience}) {
      agentd::MemoryContextPolicy policy;
      policy.mode = mode;
      policy.include_daily = false;
      std::string context;
      agentd::build_memory_context_text(f.memory.string(), f.root.string(), "mine", policy,
                                        "staleprovenanceneedle", &context);
      check(context.find("staleprovenanceneedle") == std::string::npos,
            "context injection exposed invalidated head");
    }
    Json::Value query;
    query["key"] = "fact.source";
    check(f.tool("memory_structured_query", query)["data"]["results"].empty(),
          "structured query exposed invalidated head");
    Json::Value api;
    api["key_prefix"] = "fact.";
    auto response = agentd::workflow_memory_query_to_json(f.root.string(), api, &f.error);
    check(response["ok"].asBool() && response["entries"].empty(),
          "API/native query exposed invalidated head");
    api["structured_path"] = "sessions//missing.md";
    check(!agentd::workflow_memory_query_to_json(f.root.string(), api, &f.error)["ok"].asBool(),
          "non-normalized query path escaped revision validation");
    fs::remove(f.memory / "MEMORY.md");
    check(!f.has(f.view(), "fact.source") &&
              memory_evidence_read(f.memory, original, &text, &value, &f.error),
          "deleted source became current or lost original");
  }
  { // Source fence, malformed ledger, and read-only protection never overwrite
    // authority.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact a = original\n");
    auto plan = f.capture();
    auto forged = plan;
    forged.markers[0].value = "not present in the original";
    forged.markers[0].id = memory_marker_id(forged.markers[0]);
    check(!f.promote(forged)["ok"].asBool() && !f.read().exists,
          "ungrounded native plan published");
    f.raw("MEMORY.md", "@mem fact a = changed\n");
    check(!f.promote(plan)["ok"].asBool() && !f.read().exists, "stale source plan published");
    f.promote();
    Json::Value args;
    args["path"] = "STRUCTURED.md";
    args["expected_revision"] = f.read().revision;
    args["text"] = "erase receipts";
    check(!f.tool("memory_put", args)["ok"].asBool(), "full text erased structured receipts");
    const auto before = f.read().revision;
    f.config.policy = HostToolsetPolicyMode::ReadOnly;
    check(!f.promote(f.capture())["ok"].asBool() && f.read().revision == before,
          "read-only promotion wrote state");
    f.config.policy = HostToolsetPolicyMode::Full;
    auto doc = f.doc();
    doc["promotion_ledger"]["receipts"] = 42;
    f.raw("STRUCTURED.md",
          "<!-- AGENT_MEMORY_V1_BEGIN -->\n" + compact(doc) + "\n<!-- AGENT_MEMORY_V1_END -->\n");
    MemoryPromotionPlan invalid;
    check(!memory_promotion_capture(f.memory, {(f.memory / "MEMORY.md").string()},
                                    kMemoryDocumentMaxBytes, 256, &invalid, &f.error),
          "corrupt ledger silently reset");
  }
  { // Code/history/generated examples are not executable promotion intents.
    Fixture f;
    f.raw("MEMORY.md", "```\n@mem fact code = no\n```\n    @mem fact indent = no\n## "
                       "History\n@mem fact old = no\n## Current\n@memory fact actual = yes\n");
    f.promote();
    check(f.doc()["items"].size() == 1 && f.doc()["items"].isMember("actual"),
          "examples/history promoted");
    f.raw("sessions/input.markdown", "@mem fact extension = retained\n");
    auto receipt = f.promote(f.capture({"sessions/input.markdown"}));
    const auto evidence = f.doc()["items"]["extension"]["derivation"]["markers"][0]["evidence"];
    fs::remove(f.memory / "sessions/input.markdown");
    std::string bytes;
    Json::Value value;
    check(memory_evidence_read(f.memory, evidence, &bytes, &value, &f.error),
          "markdown original inaccessible");
  }
  { // Conversion keeps the original notes as witnesses; publication is
    // idempotent.
    Fixture f;
    f.raw("STRUCTURED.md", "@mem fact self = retained witness\n");
    auto plan = f.capture({"STRUCTURED.md"});
    check(f.promote(plan)["ok"].asBool(), "self promotion");
    check(f.has(f.view(), "self"), "conversion dropped original witness");
    const auto before = f.read().revision;
    check(f.promote(f.capture({"STRUCTURED.md"}))["data"]["no_changes"].asBool() &&
              f.read().revision == before,
          "converted marker replayed");
  }
  { // Archive failure occurs before the fact/receipt commit.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact blocked = unavailable original\n");
    f.raw(".revisions", "not a directory");
    check(!f.promote(f.capture())["ok"].asBool() && !f.read().exists,
          "archive failure published unsupported fact");
  }
  {
    Fixture f;
    std::string markers;
    for (int i = 0; i < 4097; ++i)
      markers += "@mem fact fresh." + std::to_string(i) + " = pending\n";
    f.raw("MEMORY.md", markers);
    MemoryPromotionPlan plan;
    check(!memory_promotion_capture(f.memory, {(f.memory / "MEMORY.md").string()},
                                    kMemoryDocumentMaxBytes, 1000, &plan, &f.error) &&
              !f.read().exists,
          "fresh-intent admission cap silently truncated or published proposals");
  }
  { // Exercise actual marker batches through the daemon's default 1-MiB observation cap.
    Fixture f;
    std::string markers;
    for (int i = 0; i < 4096; ++i)
      markers += "@mem fact batch." + std::to_string(i) + " = value " + std::to_string(i) + "\n";
    f.raw("MEMORY.md", markers);
    agentd::DaemonConfig cfg;
    cfg.state_dir = f.root.string();
    cfg.sessions_root_dir = f.config.sessions_root_dir;
    cfg.tools = "host";
    cfg.host_policy = HostToolsetPolicyMode::Full;
    agentd::MemoryConsolidateOptions options;
    options.include_daily = false;
    options.include_session = false;
    options.max_entries = 1000;
    options.keep_checkpoints = 2;
    Json::Value report;
    for (int i = 0; i < 5; ++i) {
      const bool complete = agentd::memory_consolidate_once(cfg, options, &report, &f.error);
      check(complete == (i == 4), "real batch continuation failed: " + compact(report));
    }
    check(f.read().text.size() > 1024 * 1024 &&
              f.doc()["promotion_ledger"]["receipts"].size() == 4096,
          "real receipt-limit fixture too small");
    markers += "@mem fact batch.newA = next A\n@mem fact batch.newB = next B\n";
    f.raw("MEMORY.md", markers);
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error),
          "real rolling scan blocked new work at old receipt limit: " + f.error);
    const auto &r = report["memory_put_response"]["data"]["promotion"];
    check(r["compacted_receipts"].asUInt64() == 4096 &&
              r["observations_checked"].asUInt64() == 4098 &&
              r["decisions_omitted"].asUInt64() == 2 && r["details_incomplete"].asBool(),
          "real batch omitted work or details without a budget status");
    check(f.doc()["items"].isMember("batch.newA") && f.doc()["items"].isMember("batch.newB") &&
              f.has(f.view(), "batch.0"),
          "rolling scan lost eligible old or new facts");
    const auto before = f.read().revision;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error) &&
              f.read().revision == before &&
              report["memory_put_response"]["data"]["promotion"]["archive_reads"].asUInt64() == 1,
          "real batch retry replayed or repeatedly decoded old receipt table");
  }
  { // Exact consumed IDs survive compaction; original receipts and leaf evidence remain readable.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact seed = original\n");
    f.promote();
    const auto id = f.doc()["promotion_ledger"]["receipts"].getMemberNames()[0];
    const auto original_receipt = f.doc()["promotion_ledger"]["receipts"][id];
    f.put("seed", "direct correction");
    f.fill_receipts();
    f.raw("MEMORY.md", "@mem fact seed = original\n@mem fact pending = independent\n");
    const auto before = f.read();
    const auto plan = f.capture();
    MemoryPromotionUpdate preview;
    check(memory_promotion_prepare(f.memory, f.doc(), plan, &preview, &f.error, false) &&
              preview.compacted == 4096 && f.read().revision == before.revision,
          "compaction preview changed authority or missed capacity");
    const auto proposed = memory_promotion_preview_report(f.doc(), plan, &preview);
    check(proposed["preview"].asBool() && proposed["would_compact_receipts"].asUInt64() == 4096 &&
              f.read().revision == before.revision,
          "preview misreported publication or compacted authority");
    const auto result = f.promote(plan);
    const auto &report = result["data"]["promotion"];
    check(result["ok"].asBool() && report["complete"].asBool() &&
              report["compacted_receipts"].asUInt64() == 4096 &&
              report["consumed_receipt_count"].asUInt() == 4096 &&
              report["receipt_count"].asUInt() == 1,
          "full ledger did not compact safely: " + compact(result));
    check(f.doc()["items"]["seed"]["value"] == "direct correction" &&
              f.doc()["items"].isMember("pending"),
          "compaction revived an old value or blocked independent work");
    const auto compacted = f.read();
    std::cout << "Compaction fixture bytes: before=" << before.text.size()
              << " current=" << compacted.text.size()
              << " consumed=" << f.doc()["promotion_ledger"]["consumed"].size() << "\n";
    const auto duplicate = f.promote();
    check(duplicate["data"]["no_changes"].asBool() && f.read().revision == compacted.revision &&
              duplicate["data"]["promotion"]["archive_reads"].asUInt() == 1,
          "archived receipt was replayed or repeatedly decoded");
    Json::Value ref;
    for (const auto &decision : duplicate["data"]["promotion"]["decisions"])
      if (decision["id"] == id)
        ref = decision["original_receipt_evidence"];
    std::string text;
    Json::Value value;
    check(memory_evidence_read(f.memory, ref, &text, &value, &f.error) &&
              compact(value) == compact(original_receipt),
          "compaction lost literal original receipt: " + f.error);
    check(memory_evidence_read(f.memory, original_receipt["evidence"], &text, &value, &f.error),
          "compaction lost original marker bytes");
    for (const auto &file : fs::recursive_directory_iterator(f.memory / ".revisions"))
      if (file.is_regular_file())
        fs::last_write_time(file.path(),
                            fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    MemoryArchivePolicy expiry;
    expiry.max_age_days = 1;
    expiry.dry_run = false;
    MemoryArchiveReport expired;
    check(memory_archive_expire(f.memory, expiry, &expired, &f.error) &&
              memory_evidence_read(f.memory, ref, &text, &value, &f.error) &&
              memory_evidence_read(f.memory, original_receipt["evidence"], &text, &value, &f.error),
          "expiry discarded compacted original or leaf witness");
    std::ofstream damaged(f.archived(before.revision));
    damaged << "corrupt original";
    damaged.close();
    const auto unchanged = f.read().revision;
    auto held = f.promote(f.capture());
    check(!held["ok"].asBool() && f.read().revision == unchanged &&
              f.doc()["items"]["seed"]["value"] == "direct correction",
          "missing/corrupt compacted original became a new runnable intent");
    const auto original_path = f.archived(before.revision);
    fs::remove(original_path);
    held = f.promote(f.capture());
    check(!held["ok"].asBool() && f.read().revision == unchanged,
          "absent compacted original became a runnable intent");
    std::ofstream restored(original_path, std::ios::binary);
    restored << before.text;
    restored.close();
    auto wrong_scope = f.doc();
    wrong_scope["promotion_ledger"]["receipt_sources"][0]["source_id"] =
        "memory:" + std::string(64, '0');
    f.save(wrong_scope);
    const auto wrong_scope_head = f.read().revision;
    held = f.promote(f.capture());
    check(!held["ok"].asBool() && f.read().revision == wrong_scope_head,
          "wrong-scope compacted original was accepted");
    wrong_scope["promotion_ledger"]["schema"] = "agent.memory.promotion.v1";
    f.save(wrong_scope);
    MemoryPromotionPlan unsupported;
    check(!memory_promotion_capture(f.memory, {(f.memory / "MEMORY.md").string()},
                                    kMemoryDocumentMaxBytes, 256, &unsupported, &f.error),
          "unsupported ledger was silently converted or reset");
  }
  { // Rejected intents remain consumed even when the original head is restored.
    Fixture f;
    f.put("target", "original head");
    auto d = f.doc();
    const auto head = d["items"]["target"];
    const auto base = memory_record_revision(head);
    f.raw("MEMORY.md",
          f.marker("target", "competing A", base) + f.marker("target", "competing B", base));
    auto rejected = f.promote(f.capture());
    check(!rejected["data"]["promotion"]["complete"].asBool(), "conflicting fixture not rejected");
    f.fill_receipts();
    f.raw("MEMORY.md", f.marker("target", "competing A", base) + "@mem fact other = independent\n");
    auto compacted = f.promote(f.capture());
    check(!compacted["data"]["promotion"]["complete"].asBool() &&
              compacted["data"]["promotion"]["compacted_receipts"].asUInt64() == 4096,
          "rejected receipt compaction failed");
    f.put("target", "intervening correction");
    d = f.doc();
    d["items"]["target"] = head;
    f.save(d);
    const auto prior = f.read().revision;
    auto again = f.promote(f.capture());
    check(!again["data"]["promotion"]["complete"].asBool() && f.read().revision == prior &&
              f.doc()["items"]["target"]["value"] == "original head",
          "compacted rejection replayed after head restoration");
  }
  { // Compact capacity is bounded. Do not drop nonce identities to make room.
    Fixture f;
    f.raw("MEMORY.md", "@mem fact seed = original\n");
    f.promote();
    f.fill_receipts();
    f.raw("MEMORY.md", "@mem fact first = accepted\n");
    check(f.promote()["data"]["promotion"]["compacted_receipts"].asUInt64() == 4096,
          "capacity fixture failed to compact");
    f.fill_receipts();
    auto d = f.doc();
    auto &consumed = d["promotion_ledger"]["consumed"];
    for (size_t i = 100000; consumed.size() < kMemoryPromotionConsumedLimit; ++i) {
      auto id = std::string("marker:") + std::string(64, '0');
      const auto digits = std::to_string(i);
      id.replace(id.size() - digits.size(), digits.size(), digits);
      consumed[id] = 0;
    }
    f.save(d);
    f.raw("MEMORY.md", "@mem fact deferred = pending\n");
    const auto before = f.read().revision;
    auto result = f.promote(f.capture());
    check(result["data"]["promotion"]["deferred"].asUInt64() == 1 &&
              !result["data"]["promotion"]["complete"].asBool() && f.read().revision == before,
          "full consumed index forgot IDs or lost pending work");
    d = f.doc();
    d["promotion_ledger"]["consumed"][d["promotion_ledger"]["receipts"].getMemberNames()[0]] = 0;
    f.save(d);
    MemoryPromotionPlan invalid;
    check(!memory_promotion_capture(f.memory, {(f.memory / "MEMORY.md").string()},
                                    kMemoryDocumentMaxBytes, 256, &invalid, &f.error),
          "overlapping current/compacted receipt identity accepted");
  }
  {
    Fixture f;
    f.raw("MEMORY.md", "@mem fact cancelled = original\n");
    auto plan = f.capture();
    f.config.should_cancel = [](void *) { return true; };
    check(!f.promote(plan)["ok"].asBool() && !f.read().exists,
          "cancelled promotion wrote authority");
    MemoryPromotionPlan invalid;
    check(!memory_promotion_capture(f.memory, {(f.memory / "MEMORY.md").string()}, 4, 8, &invalid,
                                    &f.error),
          "source byte cap silently truncated input");
  }
#if !defined(_WIN32)
  for (auto phase : {MemoryPublishPhase::BeforeReplace, MemoryPublishPhase::AfterReplace}) {
    Fixture f;
    f.raw("MEMORY.md", "@mem fact seed = original\n");
    f.promote();
    f.fill_receipts();
    const auto original = f.read();
    f.raw("MEMORY.md", "@mem fact compact.crash = once\n");
    auto plan = f.capture();
    const auto child = fork();
    check(child >= 0, "compaction fork");
    if (child == 0) {
      plan.publish_observer = [&](auto at) {
        if (at == phase)
          raise(SIGKILL);
      };
      Json::Value result;
      memory_promote(f.config, plan, &result, &f.error);
      _exit(3);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "compaction crash boundary missing");
    auto d = f.doc();
    check(d["promotion_ledger"]["consumed"].size() ==
              (phase == MemoryPublishPhase::AfterReplace ? 4096u : 0u),
          "compaction and fact were partially published");
    MemoryDocument saved;
    check(memory_document_read_revision(f.memory, "STRUCTURED.md", original.revision, &saved,
                                        &f.error) == MemoryStoreStatus::Ok,
          "compaction publication lost original receipt table");
    check(f.promote()["data"]["promotion"]["complete"].asBool(),
          "compaction crash recovery failed");
    f.put("compact.crash", "new direct correction");
    check(f.promote()["data"]["no_changes"].asBool() &&
              f.doc()["items"]["compact.crash"]["value"] == "new direct correction",
          "compaction crash replayed old intent");
  }
#endif
#if !defined(_WIN32)
  for (auto phase : {MemoryPublishPhase::BeforeReplace, MemoryPublishPhase::AfterReplace}) {
    Fixture f;
    f.raw("MEMORY.md", "@mem fact crash = complete fact\n");
    auto plan = f.capture();
    const auto child = fork();
    check(child >= 0, "fork");
    if (child == 0) {
      plan.publish_observer = [&](auto at) {
        if (at == phase)
          raise(SIGKILL);
      };
      Json::Value receipt;
      memory_promote(f.config, plan, &receipt, &f.error);
      _exit(3);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "publication crash boundary not reached");
    const auto doc = f.doc();
    check(doc["items"].isMember("crash") == (phase == MemoryPublishPhase::AfterReplace),
          "interrupted publication exposed partial fact");
    check(doc.isMember("promotion_ledger") == (phase == MemoryPublishPhase::AfterReplace),
          "fact and receipt diverged after process death");
    check(f.promote()["data"]["promotion"]["complete"].asBool(),
          "restart could not reconcile promotion");
    f.put("crash", "correction after restart");
    check(f.promote()["data"]["no_changes"].asBool() &&
              f.doc()["items"]["crash"]["value"] == "correction after restart",
          "crash retry replayed old fact");
  }
#endif
  std::cout << "Memory promotion: corrections, provenance, conflicts, budgets, "
               "scopes and crash boundaries passed\n";
}
