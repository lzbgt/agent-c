#include "memory_archives.h"
#include "memory_retention.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
struct Fixture {
  fs::path state, memory;
  std::string error;
  Fixture() {
    state = fs::temp_directory_path() /
            ("memory-archive-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    memory = state / "memory";
  }
  ~Fixture() { fs::remove_all(state); }
  MemoryDocument publish(const std::string &path, const std::string &text) {
    MemoryDocumentTransaction t;
    check(t.begin(memory, path, std::nullopt, {}, &error) == MemoryStoreStatus::Ok, error);
    check(t.commit(text, &error) == MemoryStoreStatus::Ok, error);
    return t.document();
  }
  fs::path blob(const std::string &revision) {
    for (const auto &file : fs::recursive_directory_iterator(memory / ".revisions"))
      if (file.path().filename() == revision.substr(7) + ".blob")
        return file.path();
    check(false, "missing archive fixture");
    return {};
  }
  MemoryDocument old() {
    auto d = publish("MEMORY.md", "original\n");
    publish("MEMORY.md", "corrected\n");
    fs::last_write_time(blob(d.revision),
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    return d;
  }
  MemoryArchiveReport expire(MemoryArchivePolicy p, bool expected = true) {
    MemoryArchiveReport r;
    const auto result = memory_archive_expire(memory, p, &r, &error);
    check(result == expected,
          "unexpected expiry outcome: " + error +
              Json::writeString(Json::StreamWriterBuilder{}, memory_archive_report(r)));
    return r;
  }
};
int main() {
  MemoryArchivePolicy p;
  p.max_age_days = 7;
  p.dry_run = false;
  {
    Fixture f;
    const auto first = f.publish("MEMORY.md", "oldest original\n");
    for (int i = 0; i < 130; ++i)
      f.publish("MEMORY.md", "version " + std::to_string(i) + "\n");
    MemoryDocument original;
    check(memory_document_read_revision(f.memory, "MEMORY.md", first.revision, &original,
                                        &f.error) == MemoryStoreStatus::Ok,
          "ordinary source writes silently expired original history");
    MemoryArchivePolicy disabled;
    disabled.dry_run = false;
    auto r = f.expire(disabled);
    check(r.expired == 0 && fs::exists(f.blob(first.revision)), "disabled policy removed history");
  }
  {
    Fixture f;
    const auto original = f.old();
    const auto blob = f.blob(original.revision);
    auto preview = p;
    preview.dry_run = true;
    auto r = f.expire(preview);
    check(r.selected == 1 && r.expired == 0 && r.outcomes[0]["commit_state"] == "preview" &&
              fs::exists(blob) && !fs::exists(f.memory / ".expiry"),
          "preview mutated archive or intent state");
    auto applied = f.expire(p);
    check(applied.expired == 1 && applied.expired_bytes == original.text.size() &&
              !fs::exists(blob),
          "explicit expiry failed");
    const auto intent = f.memory / applied.outcomes[0]["intent_path"].asString();
    check(fs::exists(intent), "expired original lost durable metadata intent");
    MemoryDocument unavailable;
    check(memory_document_read_revision(f.memory, "MEMORY.md", original.revision, &unavailable,
                                        &f.error) == MemoryStoreStatus::Unavailable,
          "expired original drifted to current head");
    check(f.expire(p).expired == 0, "fresh pass counted absent archive as another expiry");
  }
  {
    Fixture f;
    const auto original = f.old();
    f.publish("pins.md", "Required original: " + original.revision + "\n");
    auto r = f.expire(p);
    check(r.protected_count == 1 && r.selected == 0 && fs::exists(f.blob(original.revision)),
          "active literal evidence reference was expired");
    f.publish("pins.md", "no original reference\n");
    auto removed = f.expire(p);
    check(removed.expired == 1, "removing the last active pin did not permit expiry");
  }
  {
    Fixture f;
    const auto original = f.old();
    f.publish("recaps/recap_manifest.json", "{\"revision\":\"" + original.revision + "\"}\n");
    auto r = f.expire(p);
    check(r.protected_count == 1 && r.expired == 0, "active recap input reference was expired");
  }
  {
    Fixture f;
    auto original = f.old();
    std::ofstream(f.blob(original.revision)) << "corrupt";
    fs::last_write_time(f.blob(original.revision),
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    auto r = f.expire(p, false);
    check(r.expired == 0 && !r.errors.empty() && fs::exists(f.blob(original.revision)),
          "corrupt original counted as expiry");
  }
  {
    Fixture f;
    auto original = f.old();
    fs::create_symlink(f.memory / "MEMORY.md", f.memory / "linked.md");
    auto r = f.expire(p, false);
    check(r.expired == 0 && fs::exists(f.blob(original.revision)),
          "linked active source bypassed protected capture");
  }
  {
    Fixture f;
    auto original = f.old();
    auto changed = p;
    changed.before_expire = [&](const auto &) {
      std::ofstream(f.memory / "MEMORY.md") << "new required pin " + original.revision;
    };
    auto r = f.expire(changed, false);
    check(r.expired == 0 && r.deferred == 1 && fs::exists(f.blob(original.revision)),
          "detected source correction did not preserve original");
  }
  {
    Fixture f;
    auto original = f.old();
    auto interrupted = p;
    interrupted.observer = [](auto phase) {
      if (phase == MemoryPublishPhase::AfterReplace)
        throw 1;
    };
    auto r = f.expire(interrupted, false);
    check(r.expired == 0 && r.uncertain == 1 &&
              r.outcomes[0]["commit_state"] == "expired_unconfirmed" &&
              fs::exists(f.memory / r.outcomes[0]["intent_path"].asString()),
          "uncertain expiry lost its intent or reported confirmed success");
  }
  {
    Fixture f;
    auto original = f.old();
    bool cancel = false;
    auto cancelled = p;
    cancelled.cancelled = [&] { return cancel; };
    cancelled.observer = [&](auto phase) {
      if (phase == MemoryPublishPhase::Archived)
        cancel = true;
    };
    auto r = f.expire(cancelled, false);
    check(r.expired == 0 && fs::exists(f.blob(original.revision)),
          "cancellation after intent deleted original");
    check(f.expire(p).expired == 1, "verified existing expiry intent prevented local recovery");
  }
  {
    Fixture f;
    f.old();
    for (int i = 0; i < 3; ++i)
      f.publish("MEMORY.md", "many " + std::to_string(i));
    for (const auto &file : fs::recursive_directory_iterator(f.memory / ".revisions"))
      if (file.is_regular_file())
        fs::last_write_time(file.path(),
                            fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    auto bounded = p;
    bounded.max_expirations = 1;
    auto r = f.expire(bounded, false);
    check(r.expired == 1 && r.deferred == r.selected - 1 && r.budget_exhausted,
          "operation cap lost independent expiry or pending count");
  }
  {
    Fixture f;
    const auto original = f.old();
    fs::create_directories(f.memory / "lots");
    for (int i = 0; i < 4097; ++i)
      std::ofstream(f.memory / "lots" / std::to_string(i));
    auto r = f.expire(p, false);
    check(r.expired == 0 && r.budget_exhausted && fs::exists(f.blob(original.revision)),
          "incomplete active source inventory allowed expiry");
  }
  {
    Fixture f;
    const auto original = f.old();
    for (int i = 0; i < 9; ++i)
      f.publish("large" + std::to_string(i) + ".md", std::string(kMemoryDocumentMaxBytes, 'x'));
    auto r = f.expire(p, false);
    check(r.expired == 0 && r.budget_exhausted && fs::exists(f.blob(original.revision)),
          "pin-byte cap silently truncated active sources");
  }
  {
    Fixture f;
    const auto original = f.old();
    agentd::DaemonConfig cfg;
    cfg.state_dir = f.state.string();
    cfg.tools = "host";
    cfg.host_policy = HostToolsetPolicyMode::ReadOnly;
    agentd::MemoryRetentionPolicy policy;
    policy.archive_max_days = 7;
    agentd::MemoryRetentionStats r;
    check(!agentd::memory_retention_enforce(cfg, policy, &r, &f.error) &&
              fs::exists(f.blob(original.revision)),
          "readonly policy expired history");
    cfg.host_policy = HostToolsetPolicyMode::Full;
    check(agentd::memory_retention_enforce(cfg, policy, &r, &f.error) &&
              r.archive_expiry["expired"].asUInt64() == 1,
          "existing retention path did not expose confirmed archive outcome");
  }
  {
    Fixture f;
    const auto original = f.old();
    const auto blob = f.blob(original.revision);
    auto limited = p;
    limited.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto r = f.expire(limited, false);
    check(r.budget_exhausted && fs::exists(blob), "expired deadline allowed unlink");
    fs::create_directories(f.memory / ".expiry" / "metadata");
    for (int i = 0; i < 2048; ++i)
      std::ofstream(f.memory / ".expiry" / "metadata" / std::to_string(i)) << "{}";
    auto capped = f.expire(p, false);
    check(capped.budget_exhausted && capped.deferred == 1 && fs::exists(blob),
          "intent capacity silently removed history or lost pending work");
  }
  {
    Fixture f;
    const auto daily = f.publish("2000-01-01.md", "newly retired original\n");
    {
      MemoryDocumentTransaction guard;
      check(guard.begin(f.memory, "2000-01-01.md", daily.revision, {}, &f.error) ==
                MemoryStoreStatus::Ok,
            f.error);
      check(memory_document_retain(f.memory, "2000-01-01.md", daily, &f.error), f.error);
    }
    fs::last_write_time(f.blob(daily.revision),
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
    agentd::DaemonConfig cfg;
    cfg.state_dir = f.state.string();
    cfg.tools = "host";
    cfg.host_policy = HostToolsetPolicyMode::Full;
    agentd::MemoryRetentionPolicy policy;
    policy.daily_max_days = 1;
    policy.archive_max_days = 7;
    agentd::MemoryRetentionStats r;
    check(agentd::memory_retention_enforce(cfg, policy, &r, &f.error) &&
              r.daily_deleted_count == 1 && r.archive_expiry["expired"].asUInt64() == 0,
          "combined pass expired its newly issued retirement receipt");
    MemoryDocument original;
    check(memory_document_read_revision(f.memory, "2000-01-01.md", daily.revision, &original,
                                        &f.error) == MemoryStoreStatus::Ok &&
              original.text == daily.text,
          "retention returned a dangling original reference");
  }
  {
    Fixture f;
    const auto original = f.old();
    MemoryDocumentTransaction guard;
    check(guard.begin(f.memory, "STRUCTURED.md", std::nullopt, {}, &f.error) ==
              MemoryStoreStatus::Ok,
          f.error);
    bool removed = false;
    check(guard.expire_revision_blob("MEMORY.md", original.revision, "{}", &removed, &f.error) ==
                  MemoryStoreStatus::Invalid &&
              !removed,
          "archive expiry accepted an active source path");
  }
#if !defined(_WIN32)
  {
    Fixture f;
    f.old();
    pid_t workers[2];
    for (int i = 0; i < 2; ++i) {
      workers[i] = fork();
      check(workers[i] >= 0, "concurrent expiry fork");
      if (workers[i] == 0) {
        MemoryArchiveReport r;
        std::string why;
        if (!memory_archive_expire(f.memory, p, &r, &why))
          _exit(5);
        std::ofstream result(f.state / ("worker" + std::to_string(i) + ".json"));
        result << Json::writeString(Json::StreamWriterBuilder{}, memory_archive_report(r));
        result.close();
        _exit(0);
      }
    }
    int count = 0;
    for (int i = 0; i < 2; ++i) {
      int status = 0;
      check(waitpid(workers[i], &status, 0) == workers[i] && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "concurrent expiry failed");
      std::ifstream result(f.state / ("worker" + std::to_string(i) + ".json"));
      Json::Value r;
      result >> r;
      count += r["expired"].asInt();
    }
    check(count == 1, "two processes counted or expired the same original twice");
  }
  for (const auto kill_phase :
       {MemoryPublishPhase::BeforeReplace, MemoryPublishPhase::AfterReplace}) {
    Fixture f;
    const auto original = f.old();
    const auto blob = f.blob(original.revision);
    const auto pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
      auto crash = p;
      crash.observer = [&](auto phase) {
        if (phase == kill_phase)
          ::kill(getpid(), SIGKILL);
      };
      MemoryArchiveReport r;
      memory_archive_expire(f.memory, crash, &r, &f.error);
      _exit(4);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "expected process death at archive boundary");
    check(fs::exists(blob) == (kill_phase == MemoryPublishPhase::BeforeReplace),
          "process death archive visibility mismatch");
    auto recovered = f.expire(p);
    check(recovered.expired == (kill_phase == MemoryPublishPhase::BeforeReplace ? 1u : 0u),
          "fresh local pass replayed an absent archive");
    size_t intents = 0;
    for (const auto &file : fs::recursive_directory_iterator(f.memory / ".expiry"))
      if (file.is_regular_file())
        ++intents;
    check(intents == 1, "crash lost or duplicated expiry intent");
  }
#endif
  std::cout << "Explicit archive expiry, active pins, intent durability, capture/operation bounds "
               "and crash recovery passed\n";
}
