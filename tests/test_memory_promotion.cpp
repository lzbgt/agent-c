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
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
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
    check(memory_document_read(memory, path, &d, &error) ==
              MemoryStoreStatus::Ok,
          error);
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
    check(memory_promotion_capture(memory, paths, kMemoryDocumentMaxBytes,
                                   max_keys, &plan, &error),
          error);
    return plan;
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
    const auto status =
        exec.execute(exec.ctx, name.c_str(), compact(args).c_str(), &result);
    auto receipt =
        parse(result.data ? std::string(result.data, result.len) : "");
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
    check(memory_view_capture(memory, {(memory / "STRUCTURED.md").string()},
                              mode, &s, &error),
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
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error),
          f.error);
    const auto before = f.read().revision;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error) &&
              f.read().revision == before,
          "duplicate changed head/recency");
    for (int i = 0; i < 23; ++i)
      f.put("promotion.fixture", "correction " + std::to_string(i));
    const auto corrected = f.read().revision;
    check(agentd::memory_consolidate_once(cfg, options, &report, &f.error),
          f.error);
    check(f.doc()["items"]["promotion.fixture"]["value"] == "correction 22" &&
              f.read().revision == corrected,
          "stale marker undid correction after history rollover");
    f.raw("MEMORY.md", "unrelated append and moved lines\n\n@mem fact "
                       "promotion.fixture = original observation\n");
    check(f.promote()["data"]["no_changes"].asBool() &&
              f.read().revision == corrected,
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
    f.raw("MEMORY.md", f.marker("target", "stale", base) +
                           "@mem fact independent = still runnable\n");
    auto rejected = f.promote();
    check(!rejected["data"]["promotion"]["complete"].asBool() &&
              f.doc()["items"]["target"]["value"] == "new" &&
              f.doc()["items"].isMember("independent"),
          "conflict blocked independent work or overwrote head");
    const auto same = f.read().revision;
    check(!f.promote()["data"]["promotion"]["complete"].asBool() &&
              f.read().revision == same,
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
    f.raw("MEMORY.md",
          "@mem fact collision = one\n@mem fact collision = two\n@mem fact "
          "independent = ok\n@mem {\"key\":\"bad\",\"value\":4}\n");
    auto receipt = f.promote();
    check(!receipt["data"]["promotion"]["complete"].asBool() &&
              !f.doc()["items"].isMember("collision") &&
              f.doc()["items"].isMember("independent"),
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
    const auto original =
        f.doc()["items"]["fact.source"]["derivation"]["markers"][0]["evidence"];
    f.raw("MEMORY.md",
          "unrelated text\n@mem fact fact.source = staleprovenanceneedle\n");
    check(f.has(f.view(), "fact.source"),
          "unrelated append invalidated stable intent");
    auto fence = f.view();
    f.raw("MEMORY.md", "source has been corrected\n");
    check(!memory_view_validate(fence, &f.error),
          "dependency edit passed snapshot fence");
    check(!f.has(f.view(), "fact.source"), "removed marker remained current");
    auto historical = f.view(MemoryViewMode::Historical);
    check(f.has(historical, "fact.source") &&
              historical.passages[0].historical &&
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
                            "staleprovenanceneedle", MemoryViewMode::Current,
                            indexed, false, 8, 600, &hits, &engine, &f.error) &&
                hits.empty(),
            "current lexical path exposed invalidated head");
    }
    agentd::MemorySaliencePolicy policy;
    policy.include_daily = false;
    agentd::MemorySalienceReport report;
    check(
        agentd::memory_salience_collect(f.memory, policy, &report, &f.error) &&
            report.structured_items.empty(),
        "salience exposed invalidated head");
    for (auto mode :
         {agentd::MemoryContextMode::Files, agentd::MemoryContextMode::Index,
          agentd::MemoryContextMode::Search,
          agentd::MemoryContextMode::Salience}) {
      agentd::MemoryContextPolicy policy;
      policy.mode = mode;
      policy.include_daily = false;
      std::string context;
      agentd::build_memory_context_text(f.memory.string(), f.root.string(),
                                        "mine", policy, "staleprovenanceneedle",
                                        &context);
      check(context.find("staleprovenanceneedle") == std::string::npos,
            "context injection exposed invalidated head");
    }
    Json::Value query;
    query["key"] = "fact.source";
    check(f.tool("memory_structured_query", query)["data"]["results"].empty(),
          "structured query exposed invalidated head");
    Json::Value api;
    api["key_prefix"] = "fact.";
    auto response =
        agentd::workflow_memory_query_to_json(f.root.string(), api, &f.error);
    check(response["ok"].asBool() && response["entries"].empty(),
          "API/native query exposed invalidated head");
    api["structured_path"] = "sessions//missing.md";
    check(!agentd::workflow_memory_query_to_json(f.root.string(), api,
                                                 &f.error)["ok"]
               .asBool(),
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
    check(!f.promote(plan)["ok"].asBool() && !f.read().exists,
          "stale source plan published");
    f.promote();
    Json::Value args;
    args["path"] = "STRUCTURED.md";
    args["expected_revision"] = f.read().revision;
    args["text"] = "erase receipts";
    check(!f.tool("memory_put", args)["ok"].asBool(),
          "full text erased structured receipts");
    const auto before = f.read().revision;
    f.config.policy = HostToolsetPolicyMode::ReadOnly;
    check(!f.promote(f.capture())["ok"].asBool() && f.read().revision == before,
          "read-only promotion wrote state");
    f.config.policy = HostToolsetPolicyMode::Full;
    auto doc = f.doc();
    doc["promotion_ledger"]["receipts"] = 42;
    f.raw("STRUCTURED.md", "<!-- AGENT_MEMORY_V1_BEGIN -->\n" + compact(doc) +
                               "\n<!-- AGENT_MEMORY_V1_END -->\n");
    check(!f.promote(f.capture())["ok"].asBool(),
          "corrupt ledger silently reset");
  }
  { // Code/history/generated examples are not executable promotion intents.
    Fixture f;
    f.raw(
        "MEMORY.md",
        "```\n@mem fact code = no\n```\n    @mem fact indent = no\n## "
        "History\n@mem fact old = no\n## Current\n@memory fact actual = yes\n");
    f.promote();
    check(f.doc()["items"].size() == 1 && f.doc()["items"].isMember("actual"),
          "examples/history promoted");
    f.raw("sessions/input.markdown", "@mem fact extension = retained\n");
    auto receipt = f.promote(f.capture({"sessions/input.markdown"}));
    const auto evidence =
        f.doc()["items"]["extension"]["derivation"]["markers"][0]["evidence"];
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
    check(f.promote(f.capture({"STRUCTURED.md"}))["data"]["no_changes"]
                  .asBool() &&
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
    f.raw("MEMORY.md", "@mem fact seed = original\n");
    f.promote();
    auto doc = f.doc();
    const auto seed = doc["promotion_ledger"]["receipts"].getMemberNames()[0];
    const auto receipt = doc["promotion_ledger"]["receipts"][seed];
    for (size_t i = 0; doc["promotion_ledger"]["receipts"].size() <
                       kMemoryPromotionReceiptLimit;
         ++i) {
      auto id = std::string("marker:") + std::string(64, '0');
      const auto digits = std::to_string(i);
      id.replace(id.size() - digits.size(), digits.size(), digits);
      doc["promotion_ledger"]["receipts"][id] = receipt;
    }
    f.raw("STRUCTURED.md", "<!-- AGENT_MEMORY_V1_BEGIN -->\n" + compact(doc) +
                               "\n<!-- AGENT_MEMORY_V1_END -->\n");
    f.raw("MEMORY.md", "@mem fact pending = deferred\n");
    const auto before = f.read().revision;
    const auto result = f.promote();
    check(result["data"]["promotion"]["deferred"].asUInt64() == 1 &&
              !result["data"]["promotion"]["complete"].asBool() &&
              f.read().revision == before,
          "full ledger discarded receipts or silently lost pending work");
    f.config.should_cancel = [](void *) { return true; };
    check(!f.promote(f.capture())["ok"].asBool() && f.read().revision == before,
          "cancelled promotion wrote authority");
    MemoryPromotionPlan plan;
    check(!memory_promotion_capture(f.memory,
                                    {(f.memory / "MEMORY.md").string()}, 4, 8,
                                    &plan, &f.error),
          "source byte cap silently truncated input");
  }
#if !defined(_WIN32)
  for (auto phase :
       {MemoryPublishPhase::BeforeReplace, MemoryPublishPhase::AfterReplace}) {
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
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGKILL,
          "publication crash boundary not reached");
    const auto doc = f.doc();
    check(doc["items"].isMember("crash") ==
              (phase == MemoryPublishPhase::AfterReplace),
          "interrupted publication exposed partial fact");
    check(doc.isMember("promotion_ledger") ==
              (phase == MemoryPublishPhase::AfterReplace),
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
