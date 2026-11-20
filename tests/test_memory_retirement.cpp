#include "agent_sha256.h"
#include "memory_index.h"
#include "memory_promotion.h"
#include "memory_retention.h"
#include "memory_store.h"
#include "memory_structured.h"
#include "memory_view.h"
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
using Status = MemoryStoreStatus;
void check(bool ok, const std::string &reason) {
  if (!ok) {
    std::cerr << reason << '\n';
    std::exit(1);
  }
}
std::string json(const Json::Value &value) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, value);
}
struct Fixture {
  fs::path root, memory;
  agentd::DaemonConfig cfg;
  std::string error;
  Fixture() {
    root = fs::temp_directory_path() /
           ("memory-retirement-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    memory = root / "memory";
    fs::create_directories(memory);
    fs::create_directories(root / "sessions");
    cfg.state_dir = root.string();
    cfg.sessions_root_dir = (root / "sessions").string();
  }
  ~Fixture() { fs::remove_all(root); }
  void raw(const std::string &path, const std::string &text) {
    fs::create_directories((memory / path).parent_path());
    std::ofstream file(memory / path, std::ios::binary);
    file << text;
    check(bool(file), "fixture write");
  }
  MemoryDocument read(const std::string &path) {
    MemoryDocument doc;
    check(memory_document_read(memory, path, &doc, &error) == Status::Ok,
          error);
    return doc;
  }
  Json::Value ref(const std::string &path) {
    auto doc = read(path);
    MemoryPassage p;
    p.path = path;
    p.source_id = doc.source_id;
    p.revision = doc.revision;
    p.end = doc.text.size();
    return memory_passage_evidence(p);
  }
  void original(const Json::Value &reference, const std::string &expected) {
    std::string bytes;
    Json::Value value;
    const bool ok =
        memory_evidence_read(memory, reference, &bytes, &value, &error);
    check(ok && bytes == expected, "original evidence unavailable: " + error);
  }
  agentd::MemoryRetentionStats
  enforce(const agentd::MemoryRetentionPolicy &policy) {
    agentd::MemoryRetentionStats stats;
    const bool ok =
        agentd::memory_retention_enforce(cfg, policy, &stats, &error);
    check(ok == stats.complete, "completion bool/report mismatch");
    return stats;
  }
  void replace(const std::string &path, const std::string &text) {
    auto doc = read(path);
    MemoryDocumentTransaction tx;
    check(tx.begin(memory, path, doc.revision, {}, &error) == Status::Ok,
          error);
    check(tx.commit(text, &error) == Status::Ok, error);
  }
  void checkpoint(const std::string &path, const std::string &timestamp) {
    Json::Value doc;
    doc["schema"] = "agent_memory_checkpoint_v1";
    doc["ts_utc"] = timestamp;
    doc["path"] = "STRUCTURED.md";
    doc["doc"]["items"] = Json::Value(Json::objectValue);
    raw("checkpoints/" + path, json(doc) + "\n");
  }
};
int main() {
  {
    Fixture f;
    f.raw("2000-01-01.md", "unpromoted original evidence\n");
    f.raw("2000-01-02.md", "");
    const auto ref = f.ref("2000-01-01.md"), empty = f.ref("2000-01-02.md");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    policy.dry_run = true;
    auto preview = f.enforce(policy);
    check(preview.complete && preview.daily_selected_count == 2 &&
              preview.daily_deleted_count == 0 &&
              preview.daily_bytes_after == preview.daily_bytes_before &&
              preview.daily_bytes_projected == 0,
          "preview claimed publication");
    check(!fs::exists(f.memory / ".locks") &&
              !fs::exists(f.memory / ".revisions"),
          "preview created lock/archive state");
    policy.dry_run = false;
    auto result = f.enforce(policy);
    check(result.complete && result.daily_deleted_count == 2 &&
              result.daily_bytes_after == 0 && !f.read("2000-01-01.md").exists,
          "retirement failed");
    f.original(ref, "unpromoted original evidence\n");
    f.original(empty, "");
    auto report = agentd::memory_retention_report(policy, result, f.error);
    check(report["schema"] == "agent.memory.retention.v2" &&
              report["outcomes"].size() == 2 &&
              report["outcomes"][0]["commit_state"] == "retired",
          "versioned retirement report missing");
    auto again = f.enforce(policy);
    check(again.complete && again.daily_deleted_count == 0 &&
              again.outcomes.empty(),
          "repeat retirement replayed removed heads");
  }
  {
    Fixture f;
    f.checkpoint("structured_old.json", "2020-01-01T00:00:00Z");
    f.checkpoint("structured_middle.json", "2021-01-01T00:00:00Z");
    f.checkpoint("structured_newest.json", "2022-01-01T00:00:00Z");
    const auto ref = f.ref("checkpoints/structured_old.json");
    const auto bytes = f.read("checkpoints/structured_old.json").text;
    agentd::MemoryRetentionPolicy policy;
    policy.checkpoint_max_count = 1;
    auto result = f.enforce(policy);
    check(result.complete && result.checkpoint_deleted_count == 2 &&
              f.read("checkpoints/structured_newest.json").exists &&
              !f.read("checkpoints/structured_old.json").exists,
          "checkpoint cap did not retain newest");
    f.original(ref, bytes);
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "old observed bytes\n");
    f.raw("2000-01-02.md", "independent\n");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    policy.before_retire = [&](const auto &path) {
      if (path == "2000-01-01.md")
        f.replace(path, "concurrent corrected bytes\n");
    };
    auto result = f.enforce(policy);
    check(!result.complete && result.daily_deleted_count == 1 &&
              result.outcomes[0]["commit_state"] == "revision_conflict" &&
              f.read("2000-01-01.md").text == "concurrent corrected bytes\n",
          "stale retirement removed correction or blocked independent file");
    check(result.daily_bytes_after ==
              int64_t(std::string("old observed bytes\n").size()),
          "failed retirement counted as freed active bytes");
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "cannot retire without original\n");
    f.raw(".revisions", "blocked");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    auto result = f.enforce(policy);
    check(!result.complete && result.daily_deleted_count == 0 &&
              result.daily_bytes_after == result.daily_bytes_before &&
              f.read("2000-01-01.md").exists,
          "archive failure counted as successful deletion");
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "uncertain\n");
    f.raw("2000-01-02.md", "independent\n");
    const auto original = f.ref("2000-01-01.md");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    bool interrupted = false;
    policy.publish_observer = [&](auto phase) {
      if (!interrupted && phase == MemoryPublishPhase::AfterReplace) {
        interrupted = true;
        throw 1;
      }
    };
    auto result = f.enforce(policy);
    check(!result.complete && result.daily_deleted_count == 1 &&
              result.outcomes[0]["commit_state"] == "retired_unconfirmed" &&
              result.outcomes[0]["head_removed"].asBool() &&
              result.outcomes[0]["current_revision"] == "absent",
          "uncertain retirement falsely reported confirmed success");
    f.original(original, "uncertain\n");
    policy.publish_observer = {};
    check(f.enforce(policy).complete,
          "restart did not reconcile missing heads");
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "preserve on cancellation\n");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    policy.cancelled = [] { return true; };
    auto result = f.enforce(policy);
    check(!result.complete && result.cancelled &&
              result.daily_deleted_count == 0 &&
              f.read("2000-01-01.md").exists &&
              !fs::exists(f.memory / ".locks"),
          "cancelled capture changed source");
    bool cancelled = false;
    policy.cancelled = [&] { return cancelled; };
    policy.publish_observer = [&](auto phase) {
      if (phase == MemoryPublishPhase::Archived)
        cancelled = true;
    };
    result = f.enforce(policy);
    check(!result.complete && result.cancelled &&
              result.daily_deleted_count == 0 && f.read("2000-01-01.md").exists,
          "cancellation after archival removed source");
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "readonly\n");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    f.cfg.host_policy = HostToolsetPolicyMode::ReadOnly;
    check(!f.enforce(policy).complete && f.read("2000-01-01.md").exists,
          "readonly policy retired source");
    policy.dry_run = true;
    check(f.enforce(policy).complete && !fs::exists(f.memory / ".locks"),
          "readonly preview mutated source");
  }
  {
    Fixture f;
    for (int i = 0; i < 129; ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "2000-%02d-%02d.md", 1 + i / 28,
                    1 + i % 28);
      f.raw(name, "bounded\n");
    }
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    auto result = f.enforce(policy);
    check(!result.complete && result.daily_selected_count == 129 &&
              result.daily_deleted_count == 128 && result.deferred_count == 1 &&
              result.budget_exhausted && result.outcomes.size() == 128,
          "mutation cap silently truncated or exceeded");
    auto rest = f.enforce(policy);
    check(rest.complete && rest.daily_deleted_count == 1,
          "deferred retirement lost");
  }
  {
    Fixture f;
    const std::string block(kMemoryDocumentMaxBytes, 'x');
    for (int i = 1; i <= 9; ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "2000-01-%02d.md", i);
      f.raw(name, block);
    }
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    auto result = f.enforce(policy);
    check(!result.complete && result.budget_exhausted &&
              result.daily_deleted_count == 0 &&
              f.read("2000-01-01.md").exists &&
              !fs::exists(f.memory / ".locks"),
          "capture byte cap published partial plan");
  }
  {
    Fixture f;
    f.raw("2000-02-31.md", "not a calendar date\n");
    f.raw("2000-01-01.md", "eligible\n");
    f.checkpoint("structured_bad_date.json", "2020-02-31T00:00:00Z");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    policy.checkpoint_max_count = 1;
    auto result = f.enforce(policy);
    check(!result.complete && result.daily_deleted_count == 1 &&
              f.read("2000-02-31.md").exists &&
              f.read("checkpoints/structured_bad_date.json").exists,
          "invalid dates treated as reliable retention time");
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "unguarded\n");
    {
      MemoryDocumentTransaction tx;
      check(tx.begin(f.memory, "2000-01-01.md", std::nullopt, {}, &f.error) ==
                Status::Ok,
            f.error);
      check(tx.retire(&f.error) == Status::Invalid,
            "retire allowed implicit base revision");
    }
    {
      MemoryDocumentTransaction tx;
      check(tx.begin(f.memory, "2000-01-01.md",
                     f.read("2000-01-01.md").revision, {},
                     &f.error) == Status::Ok,
            f.error);
      f.raw("2000-01-01.md", "external correction\n");
      check(tx.retire(&f.error) == Status::Conflict &&
                f.read("2000-01-01.md").text == "external correction\n",
            "external edit before retirement was removed");
    }
    {
      MemoryDocumentTransaction tx;
      check(tx.begin(f.memory, "STRUCTURED.md", "absent", {}, &f.error) ==
                Status::Ok,
            f.error);
      check(tx.commit("primary publication\n", &f.error) == Status::Ok,
            f.error);
      const auto revision = tx.document().revision;
      const auto original = f.ref("2000-01-01.md");
      check(tx.retire_auxiliary("2000-01-01.md",
                                original["revision"].asString(),
                                &f.error) == Status::Ok,
            "auxiliary retirement tried to reacquire writer lock");
      check(tx.document().revision == revision && tx.published() &&
                tx.retire_auxiliary("STRUCTURED.md", revision, &f.error) ==
                    Status::Invalid,
            "auxiliary retirement changed primary receipt");
      f.original(original, "external correction\n");
    }
  }
  {
    Fixture f;
    HostToolsetConfig config;
    config.root_dir = f.cfg.state_dir;
    config.sessions_root_dir = f.cfg.sessions_root_dir;
    config.enable_process_exec = false;
    agent_tool_registry_t *registry = nullptr;
    agent_tool_executor_t executor{};
    check(toolset_host_create(config, &registry, &executor) == AGENT_OK,
          "toolset for pruning");
    auto put = [&](const std::string &value) {
      Json::Value args;
      args["path"] = "STRUCTURED.md";
      args["expected_revision"] = f.read("STRUCTURED.md").revision;
      args["entries"][0]["key"] = "pruning.fixture";
      args["entries"][0]["value"] = value;
      args["keep_checkpoints"] = 1;
      agent_string_t response{};
      const auto status = executor.execute(executor.ctx, "memory_put",
                                           json(args).c_str(), &response);
      std::istringstream stream(
          response.data ? std::string(response.data, response.len) : "");
      Json::CharReaderBuilder reader;
      Json::Value parsed;
      const bool valid =
          Json::parseFromStream(reader, stream, &parsed, &f.error);
      agent_string_free(&response);
      check(valid && status == AGENT_OK && parsed["ok"].asBool(),
            "memory_put checkpoint pruning failed");
      return parsed["data"];
    };
    const auto first = put("first");
    const auto path = first["checkpoint_path"].asString();
    const auto proof = f.ref(path);
    const auto original = f.read(path).text;
    fs::last_write_time(f.memory / path, fs::file_time_type::clock::now() -
                                             std::chrono::hours(1));
    const auto second = put("second");
    check(second["checkpoint_prune_ok"].asBool() &&
              second["checkpoint_pruned_count"].asInt() == 1 &&
              second["revision"] == f.read("STRUCTURED.md").revision &&
              !f.read(path).exists,
          "auxiliary pruning lost the primary receipt or kept the old "
          "checkpoint");
    f.original(proof, original);
    agent_tool_registry_destroy(registry);
    toolset_host_destroy(&executor);
  }
  {
    Fixture f;
    f.raw("2000-01-01.md", "@mem fact dependent = retirementneedle\n");
    MemoryPromotionPlan plan;
    check(memory_promotion_capture(f.memory,
                                   {(f.memory / "2000-01-01.md").string()},
                                   1024 * 1024, 8, &plan, &f.error),
          f.error);
    HostToolsetConfig config;
    config.root_dir = f.cfg.state_dir;
    config.sessions_root_dir = f.cfg.sessions_root_dir;
    Json::Value receipt;
    check(memory_promote(config, plan, &receipt, &f.error) == AGENT_OK &&
              receipt["ok"].asBool(),
          "fixture promotion");
    const auto original = f.ref("2000-01-01.md");
    const std::vector<std::string> files = {
        (f.memory / "STRUCTURED.md").string()};
    std::vector<MemorySearchHit> hits;
    std::string engine;
    check(memory_retrieve(f.memory, files, "retirementneedle",
                          MemoryViewMode::Current, true, false, 8, 600, &hits,
                          &engine, &f.error) &&
              hits.size() == 1,
          "cache fixture did not include promoted fact");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    check(f.enforce(policy).complete, "source retirement");
    for (bool indexed : {false, true})
      check(memory_retrieve(f.memory, files, "retirementneedle",
                            MemoryViewMode::Current, indexed, false, 8, 600,
                            &hits, &engine, &f.error) &&
                hits.empty(),
            "retirement left derived current candidate eligible");
    f.original(original, "@mem fact dependent = retirementneedle\n");
    check(memory_retrieve(f.memory, files, "retirementneedle",
                          MemoryViewMode::Historical, false, false, 8, 600,
                          &hits, &engine, &f.error) &&
              hits.size() == 1 && hits[0].passage.historical,
          "historical derived fact not identified");
  }
#if !defined(_WIN32)
  {
    Fixture f;
    fs::create_directories(f.root / "outside");
    {
      std::ofstream file(f.root / "outside/evidence.md");
      file << "outside source";
    }
    fs::create_symlink(f.root / "outside/evidence.md",
                       f.memory / "2000-01-01.md");
    f.raw("2000-01-02.md", "independent source\n");
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    const auto result = f.enforce(policy);
    check(!result.complete && result.daily_deleted_count == 1 &&
              fs::is_symlink(f.memory / "2000-01-01.md") &&
              fs::file_size(f.root / "outside/evidence.md") == 14,
          "retention followed a linked source");
    MemoryDocument invalid;
    check(memory_document_read(f.memory, "sessions//note.md", &invalid,
                               &f.error) == Status::Invalid,
          "non-normalized source acquired a second identity");
  }
#endif
#if !defined(_WIN32)
  {
    Fixture f;
    f.raw("2000-01-01.md", "original process race\n");
    const auto original = f.ref("2000-01-01.md");
    pid_t children[2];
    for (int i = 0; i < 2; ++i) {
      children[i] = fork();
      check(children[i] >= 0, "fork");
      if (children[i] == 0) {
        MemoryDocumentTransaction tx;
        std::string why;
        auto status = tx.begin(f.memory, "2000-01-01.md",
                               original["revision"].asString(), {}, &why);
        if (status == Status::Conflict)
          _exit(4);
        if (status != Status::Ok)
          _exit(5);
        status = i == 0 ? tx.retire(&why)
                        : tx.commit("concurrent replacement\n", &why);
        _exit(status == Status::Ok ? 0 : 6);
      }
    }
    int successes = 0, conflicts = 0;
    for (auto child : children) {
      int code = 0;
      check(waitpid(child, &code, 0) == child && WIFEXITED(code), "race child");
      successes += WEXITSTATUS(code) == 0;
      conflicts += WEXITSTATUS(code) == 4;
    }
    check(successes == 1 && conflicts == 1,
          "retirement and replacement both accepted same revision");
    f.original(original, "original process race\n");
  }
  for (auto phase :
       {MemoryPublishPhase::BeforeReplace, MemoryPublishPhase::AfterReplace}) {
    Fixture f;
    f.raw("2000-01-01.md", "crash original\n");
    const auto original = f.ref("2000-01-01.md");
    const auto child = fork();
    check(child >= 0, "crash fork");
    if (child == 0) {
      MemoryDocumentTransaction tx;
      std::string why;
      if (tx.begin(f.memory, "2000-01-01.md", original["revision"].asString(),
                   {}, &why) != Status::Ok)
        _exit(2);
      tx.retire(&why, [&](auto at) {
        if (at == phase)
          raise(SIGKILL);
      });
      _exit(3);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGKILL,
          "retirement crash boundary not reached");
    auto current = f.read("2000-01-01.md");
    check(current.exists == (phase == MemoryPublishPhase::BeforeReplace),
          "crash exposed wrong head state");
    f.original(original, "crash original\n");
    MemoryDocumentTransaction retry;
    check(retry.begin(f.memory, "2000-01-01.md", current.revision, {},
                      &f.error) == Status::Ok &&
              retry.retire(&f.error) == Status::Ok,
          "restart retirement reconciliation failed");
  }
#endif
  std::cout << "Memory retirement: originals, CAS, partial outcomes, budgets, "
               "cache eligibility and process death passed\n";
}
