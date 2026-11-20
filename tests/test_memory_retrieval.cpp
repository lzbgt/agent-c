#include "memory_retrieval.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
using namespace host_tools_internal;
namespace fs = std::filesystem;
void check(bool ok, const std::string &why) {
  if (!ok) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
struct Fixture {
  fs::path root = fs::temp_directory_path() /
                  ("memory-hops-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string error;
  Fixture() {
    fs::create_directories(root);
    write("MEMORY.md",
          "Cedarport supplier is JuniperWorks.\n\nJuniperWorks is based in Riverton.\n\n设备编号 "
          "foo_bar uses 中文电机.\n\n## Deprecated\nJuniperWorks was based in Sundale.\n");
    write("foreign.md", "Cedarport foreign private scope only.\n");
  }
  ~Fixture() { fs::remove_all(root); }
  void write(const std::string &path, const std::string &text) {
    std::ofstream(root / path) << text;
  }
  MemoryViewSnapshot snapshot(MemoryViewMode mode = MemoryViewMode::Current) {
    MemoryViewSnapshot s;
    check(memory_view_capture(root, {(root / "MEMORY.md").string()}, mode, &s, &error), error);
    return s;
  }
};
MemoryScoringResult ranked(const MemoryScoringRequest &q) {
  MemoryScoringResult r;
  r.provider = "synthetic";
  r.model = "fixture-v1";
  r.prompt_revision = "fixture-v1";
  for (const auto &c : q.candidates) {
    check(c.text.find("Sundale") == std::string::npos &&
              c.text.find("foreign private") == std::string::npos,
          "ineligible scoring input");
    r.scores.emplace_back(c.id, c.text.find("Riverton") != std::string::npos ? 0.99 : 0.9);
  }
  return r;
}
int main() {
  Fixture f;
  MemoryRetrievalOptions o;
  o.strategy = MemoryRetrievalStrategy::Expand;
  o.initial_candidates = 1;
  const std::string question = "Where is Cedarport supplier located?";
  auto snapshot = f.snapshot();
  int calls = 0;
  MemoryScorer scorer = [&](const auto &q, auto *r, auto *) {
    ++calls;
    *r = ranked(q);
    return true;
  };
  MemoryRetrievalResult result;
  check(memory_retrieve_evidence(snapshot, question, o, scorer, &result, &f.error), f.error);
  check(calls == 2 && result.hits.size() == 2 &&
            result.hits[0].snippet.find("Riverton") != std::string::npos,
        "one expansion failed to bridge unseen evidence");
  check(result.report["complete"].asBool() && result.report["candidate_judgments"].asUInt() == 3 &&
            result.report["trace"].size() == 1,
        "wrong bounded expansion accounting");
  const auto &trace = result.report["trace"][0];
  const auto &seed = result.report["stages"][0]["inputs"]["candidates"][0];
  const auto begin = trace["seed_text_span"]["begin"].asUInt64(),
             end = trace["seed_text_span"]["end"].asUInt64();
  check(trace["seed_id"] == seed["id"] &&
            seed["text"].asString().substr(begin, end - begin) == trace["term"].asString(),
        "term lost exact scoring-text origin");
  check(result.report["stages"][1]["inputs"]["accumulated"].size() == 1,
        "expansion omitted accumulated evidence");
  for (const auto &row : result.context) {
    std::string original;
    Json::Value value;
    check(memory_evidence_read(f.root, row["evidence"], &original, &value, &f.error),
          "result lost original evidence");
    check(!row["historical"].asBool(), "current result is historical");
  }
  o.strategy = MemoryRetrievalStrategy::Lexical;
  calls = 0;
  check(memory_retrieve_evidence(snapshot, question, o, scorer, &result, &f.error) && calls == 0 &&
            result.hits.size() == 1,
        "cheap lexical path invoked a scorer");
  o.strategy = MemoryRetrievalStrategy::Rerank;
  check(memory_retrieve_evidence(snapshot, question, o, scorer, &result, &f.error) && calls == 1 &&
            result.hits.size() == 1,
        "rerank baseline expanded evidence");
  for (int failure = 0; failure < 9; ++failure) {
    calls = 0;
    MemoryScorer bad = [&](const auto &q, auto *r, auto *) {
      ++calls;
      *r = ranked(q);
      if (failure == 0)
        r->scores.clear();
      if (failure == 1)
        r->scores.push_back(r->scores.front());
      if (failure == 2)
        r->scores[0].first = "unknown-candidate";
      if (failure == 3)
        r->scores[0].second = std::nan("");
      if (failure == 4)
        r->scores[0].second = INFINITY;
      if (failure == 5)
        r->scores[0].second = -0.1;
      if (failure == 6)
        r->scores[0].second = 1.1;
      if (failure == 7)
        r->model.clear();
      if (failure == 8)
        throw std::runtime_error("do not expose this exception payload");
      return true;
    };
    o.strategy = MemoryRetrievalStrategy::Expand;
    check(memory_retrieve_evidence(snapshot, question, o, bad, &result, &f.error) && calls == 1 &&
              !result.report["complete"].asBool() &&
              result.report["status"] == "lexical_fallback" && result.hits.size() == 1,
          "malformed scorer did not preserve explicit bounded lexical fallback");
  }
  calls = 0;
  MemoryScorer second_fails = [&](const auto &q, auto *r, auto *why) {
    ++calls;
    *r = ranked(q);
    if (calls == 2) {
      *why = "synthetic cap";
      return false;
    }
    return true;
  };
  check(memory_retrieve_evidence(snapshot, question, o, second_fails, &result, &f.error) &&
            calls == 2 && result.report["stages"].size() == 2 && result.hits.size() == 1 &&
            result.report["stages"][0]["scores"].isObject() && !result.report["complete"].asBool(),
        "failed expansion lost completed scoring or emitted unscored new evidence");
  o.scoring_input_bytes = 1;
  calls = 0;
  check(memory_retrieve_evidence(snapshot, question, o, scorer, &result, &f.error) && calls == 0 &&
            !result.report["complete"].asBool(),
        "input byte cap reached a scorer");
  o.scoring_input_bytes = 24000;
  o.context_bytes = 2;
  o.strategy = MemoryRetrievalStrategy::Lexical;
  check(memory_retrieve_evidence(snapshot, question, o, scorer, &result, &f.error) &&
            result.context.empty() && result.report["status"] == "context_budget" &&
            result.report["context_bytes"].asUInt() == 2,
        "context cap cut an evidence reference or silently omitted a record");
  o.context_bytes = 16000;
  bool cancel = true;
  o.cancelled = [&] { return cancel; };
  check(!memory_retrieve_evidence(snapshot, question, o, scorer, &result, &f.error) &&
            result.context.empty(),
        "pre-cancellation returned output");
  cancel = false;
  o.strategy = MemoryRetrievalStrategy::Rerank;
  MemoryScorer cancelling = [&](const auto &q, auto *r, auto *) {
    *r = ranked(q);
    cancel = true;
    return true;
  };
  check(!memory_retrieve_evidence(snapshot, question, o, cancelling, &result, &f.error) &&
            result.context.empty(),
        "post-score cancellation emitted output");
  o.cancelled = {};
  o.timeout = std::chrono::milliseconds(10);
  MemoryScorer slow = [&](const auto &q, auto *r, auto *) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    *r = ranked(q);
    return true;
  };
  check(!memory_retrieve_evidence(snapshot, question, o, slow, &result, &f.error) &&
            result.context.empty(),
        "total deadline emitted late output");
  o.timeout = std::chrono::milliseconds(20000);
  auto broad = f.snapshot(MemoryViewMode::Historical);
  o.strategy = MemoryRetrievalStrategy::Lexical;
  o.initial_candidates = 20;
  check(memory_retrieve_evidence(broad, "Sundale", o, {}, &result, &f.error) &&
            !result.context.empty() && result.context[0]["historical"].asBool(),
        "historical original lost its classification");
  fs::remove(f.root / ".memory_retrieval_v2.sqlite3");
  fs::create_directory(f.root / ".memory_retrieval_v2.sqlite3");
  check(memory_retrieve_evidence(snapshot, "Which 设备编号 uses foo_bar?", o, {}, &result,
                                 &f.error) &&
            !result.hits.empty() && result.report["index_unavailable"].asBool(),
        "Chinese/code question fallback failed");
  std::string long_question;
  for (int i = 0; i < 40; ++i)
    long_question += "word" + std::to_string(i) + " ";
  check(memory_retrieve_evidence(snapshot, long_question, o, {}, &result, &f.error) &&
            result.report["question_terms_omitted"].asUInt() == 8 &&
            !result.report["complete"].asBool(),
        "question cap was silent");
  o.initial_candidates = 1;
  o.strategy = MemoryRetrievalStrategy::Rerank;
  MemoryScorer correction = [&](const auto &q, auto *r, auto *) {
    *r = ranked(q);
    f.write("MEMORY.md", "Cedarport correction.\n");
    return true;
  };
  check(!memory_retrieve_evidence(snapshot, question, o, correction, &result, &f.error) &&
            result.context.empty(),
        "concurrent correction leaked stale output");
  o.final_records = 9;
  check(!memory_retrieve_evidence(f.snapshot(), question, o, {}, &result, &f.error),
        "invalid record limit silently clamped");
  std::cout << "Memory retrieval: bridges, scope/currentness, typed scores, caps, fallback and "
               "cancellation passed\n";
}
