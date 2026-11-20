#include "memory_store.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace host_tools_internal;
namespace fs = std::filesystem;
using Status = MemoryStoreStatus;
void check(bool ok, const std::string& reason) { if (!ok) { std::cerr << reason << '\n';std::exit(1); } }
int main() {
  const auto root = fs::temp_directory_path() / ("memory-store-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string error;MemoryDocument original;
  check(memory_document_read(root, "STRUCTURED.md", &original, &error) == Status::Ok && !original.exists && original.revision == "absent", "absent is not a readable revision");
  const auto write = [&](const std::string& text, const std::string& revision) {
    MemoryDocumentTransaction transaction;
    check(transaction.begin(root, "STRUCTURED.md", revision, {}, &error) == Status::Ok, error);
    check(transaction.commit(text, &error) == Status::Ok, error);
    return transaction.document();
  };
  auto first = write("original\n", "absent");
  check(memory_revision_valid(first.revision) && first.revision != "absent" && !first.source_id.empty(), "missing content/source identity");
  {
    MemoryDocumentTransaction transaction;
    check(transaction.begin(root, "STRUCTURED.md", first.revision, {}, &error) == Status::Ok, error);
    check(transaction.commit(first.text, &error) == Status::Ok && !transaction.published(), "identical bytes changed the source");
    check(transaction.commit("second commit", &error) == Status::Invalid, "snapshot was used for two commits");
  }
  auto second = write("corrected\n", first.revision);
  {
    MemoryDocumentTransaction stale;
    check(stale.begin(root, "STRUCTURED.md", first.revision, {}, &error) == Status::Conflict && stale.document().revision == second.revision, "stale writer did not conflict");
    MemoryDocument historical;
    check(memory_document_read_revision(root, "STRUCTURED.md", first.revision, &historical, &error) == Status::Ok && historical.text == first.text, "original evidence was replaced by current text");
    check(memory_document_read_revision(root, "STRUCTURED.md", second.revision, &historical, &error) == Status::Ok && historical.text == second.text, "current exact revision unavailable");
  }
  {
    std::atomic<int> success{0}, conflicts{0};std::vector<std::thread> writers;
    for (int i = 0; i < 2; ++i) writers.emplace_back([&, i] {
      MemoryDocumentTransaction transaction;std::string why;
      const auto status = transaction.begin(root, "STRUCTURED.md", second.revision, {}, &why);
      if (status == Status::Conflict) { ++conflicts;return; }
      check(status == Status::Ok && transaction.commit("writer " + std::to_string(i), &why) == Status::Ok, why);++success;
    });
    for (auto& writer : writers) writer.join();
    check(success == 1 && conflicts == 1, "two concurrent replacements both accepted one revision");
  }
  {
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) writers.emplace_back([&, i] {
      for (int j = 0; j < 8; ++j) {
        MemoryDocumentTransaction transaction;std::string why;
        check(transaction.begin(root, "daily.md", std::nullopt, {}, &why) == Status::Ok, why);
        const auto text = transaction.document().text + std::to_string(i) + ":" + std::to_string(j) + "\n";
        check(transaction.commit(text, &why) == Status::Ok, why);
      }
    });
    for (auto& writer : writers) writer.join();
    MemoryDocument daily;check(memory_document_read(root, "daily.md", &daily, &error) == Status::Ok, error);
    check(std::count(daily.text.begin(), daily.text.end(), '\n') == 32, "concurrent append lost notes");
  }
  {
    MemoryDocumentTransaction outside;
    check(outside.begin(root, "STRUCTURED.md", std::nullopt, {}, &error) == Status::Ok, error);
    { std::ofstream edit(root / "STRUCTURED.md");edit << "outside edit"; }
    check(outside.commit("must not overwrite", &error) == Status::Conflict, "external edit was overwritten");
  }
  MemoryDocument current;check(memory_document_read(root, "STRUCTURED.md", &current, &error) == Status::Ok, error);
  {
    MemoryDocumentTransaction cancelled;
    check(cancelled.begin(root, "STRUCTURED.md", current.revision, [] { return true; }, &error) == Status::Cancelled, "cancelled writer acquired publication");
    MemoryDocumentTransaction large;
    check(large.begin(root, "STRUCTURED.md", current.revision, {}, &error) == Status::Ok, error);
    check(large.commit(std::string(kMemoryDocumentMaxBytes + 1, 'x'), &error) == Status::Invalid, "oversized document published");
  }
  {
    MemoryDocumentTransaction interrupted;
    check(interrupted.begin(root, "STRUCTURED.md", current.revision, {}, &error) == Status::Ok, error);
    check(interrupted.commit("not published", &error, 100, [](auto phase) { if (phase == MemoryPublishPhase::BeforeReplace) throw 1; }) == Status::Unavailable, "pre-replace interruption changed authority");
    MemoryDocument unchanged;check(memory_document_read(root, "STRUCTURED.md", &unchanged, &error) == Status::Ok && unchanged.revision == current.revision, "failed update truncated current memory");
  }
  {
    MemoryDocumentTransaction interrupted;
    check(interrupted.begin(root, "STRUCTURED.md", current.revision, {}, &error) == Status::Ok, error);
    check(interrupted.commit("published but interrupted", &error, 100, [](auto phase) { if (phase == MemoryPublishPhase::AfterReplace) throw 1; }) == Status::Uncertain && interrupted.published(), "post-replace interruption falsely reported failure without publication");
  }
#if !defined(_WIN32)
  {
    check(memory_document_read(root, "STRUCTURED.md", &current, &error) == Status::Ok, error);
    std::vector<pid_t> children;
    for (int i = 0; i < 2; ++i) {
      const auto child = ::fork();check(child >= 0, "CAS fork failed");
      if (child == 0) {
        MemoryDocumentTransaction transaction;std::string why;
        const auto status = transaction.begin(root, "STRUCTURED.md", current.revision, {}, &why);
        if (status == Status::Conflict) _exit(4);
        if (status != Status::Ok || transaction.commit("process " + std::to_string(i), &why) != Status::Ok) _exit(5);
        _exit(0);
      }
      children.push_back(child);
    }
    int success = 0, conflicts = 0;
    for (const auto child : children) {
      int status = 0;check(::waitpid(child, &status, 0) == child && WIFEXITED(status), "CAS child failed");
      if (WEXITSTATUS(status) == 0) ++success;
      else if (WEXITSTATUS(status) == 4) ++conflicts;
    }
    check(success == 1 && conflicts == 1, "OS writer lock accepted two stale processes");
  }
  for (const auto phase : {MemoryPublishPhase::BeforeReplace, MemoryPublishPhase::AfterReplace}) {
    check(memory_document_read(root, "STRUCTURED.md", &current, &error) == Status::Ok, error);
    int signal_pipe[2];check(::pipe(signal_pipe) == 0, "pipe failed");
    const auto child = ::fork();check(child >= 0, "fork failed");
    if (child == 0) {
      ::close(signal_pipe[0]);MemoryDocumentTransaction transaction;std::string why;
      if (transaction.begin(root, "STRUCTURED.md", current.revision, {}, &why) != Status::Ok) _exit(2);
      transaction.commit("complete new bytes", &why, 100, [&](auto checkpoint) {
        if (checkpoint == phase) { const char ready = 'R';::write(signal_pipe[1], &ready, 1);for (;;) ::pause(); }
      });
      _exit(3);
    }
    ::close(signal_pipe[1]);char ready = 0;
    pollfd signal_fd{signal_pipe[0], POLLIN, 0};
    if (::poll(&signal_fd, 1, 3000) != 1) {
      ::kill(child, SIGKILL);::waitpid(child, nullptr, 0);check(false, "crash boundary deadline exceeded");
    }
    check(::read(signal_pipe[0], &ready, 1) == 1 && ready == 'R', "crash boundary not reached");
    ::close(signal_pipe[0]);::kill(child, SIGKILL);int status = 0;::waitpid(child, &status, 0);
    MemoryDocument recovered;check(memory_document_read(root, "STRUCTURED.md", &recovered, &error) == Status::Ok, error);
    check(recovered.text == (phase == MemoryPublishPhase::BeforeReplace ? current.text : "complete new bytes"), "process death exposed partial memory bytes");
    MemoryDocument old;check(memory_document_read_revision(root, "STRUCTURED.md", current.revision, &old, &error) == Status::Ok && old.text == current.text, "process death lost original evidence");
    auto follow = write("after restart", recovered.revision);check(follow.text == "after restart", "dead writer retained its lock");
  }
  {
    fs::create_symlink(root / "STRUCTURED.md", root / "linked.md");
    MemoryDocument linked;check(memory_document_read(root, "linked.md", &linked, &error) == Status::Invalid, "linked source bypassed scope");
  }
#endif
  {
    const auto broken_root = root / "blocked-archive";
    {
      MemoryDocumentTransaction initial;
      check(initial.begin(broken_root, "MEMORY.md", "absent", {}, &error) == Status::Ok && initial.commit("old", &error) == Status::Ok, error);
    }
    { std::ofstream block(broken_root / ".revisions");block << "not a directory"; }
    MemoryDocumentTransaction fail_closed;
    check(fail_closed.begin(broken_root, "MEMORY.md", std::nullopt, {}, &error) == Status::Ok, error);
    check(fail_closed.commit("must not publish", &error) == Status::Unavailable && !fail_closed.published(), "archive failure still published");
    MemoryDocument unchanged;check(memory_document_read(broken_root, "MEMORY.md", &unchanged, &error) == Status::Ok && unchanged.text == "old", "archive failure lost current bytes");
  }
  {
    const auto evidence_root = root / "corrupt-original";
    std::string original_revision;
    {
      MemoryDocumentTransaction first;
      check(first.begin(evidence_root, "MEMORY.md", "absent", {}, &error) == Status::Ok && first.commit("unique original", &error) == Status::Ok, error);
      original_revision = first.document().revision;
    }
    {
      MemoryDocumentTransaction next;
      check(next.begin(evidence_root, "MEMORY.md", original_revision, {}, &error) == Status::Ok && next.commit("current correction", &error) == Status::Ok, error);
    }
    bool damaged = false;
    for (const auto& file : fs::recursive_directory_iterator(evidence_root / ".revisions")) {
      if (file.path().filename() == original_revision.substr(7) + ".blob") {
        std::ofstream corrupt(file.path());corrupt << "tampered bytes";damaged = true;
      }
    }
    check(damaged, "original archive fixture missing");
    MemoryDocument rejected;
    check(memory_document_read_revision(evidence_root, "MEMORY.md", original_revision, &rejected, &error) == Status::Unavailable, "corrupt original silently accepted or substituted current bytes");
  }
  fs::remove_all(root);
  std::cout << "Atomic memory revisions, conflict/concurrency, cancellation, size, archived originals and process-crash checks passed\n";
}
