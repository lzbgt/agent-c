#include "memory_retrieval.h"
#if defined(AGENT_HAVE_JSONCPP)
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace host_tools_internal {
namespace {
std::string compact(const Json::Value &v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}
std::string id(const MemorySearchHit &hit) {
  return memory_passage_evidence(hit.passage)["id"].asString();
}
struct Term {
  std::string text, seed;
  size_t begin = 0, end = 0, frequency = 0;
};
template <class Emit>
bool visit_terms(const std::string &text, const std::string &seed, Emit emit,
                 const std::function<bool()> &cancelled = {}) {
  size_t begin = 0;
  auto token = [](unsigned char c) {
    return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
  };
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i % 4096 == 0 && cancelled && cancelled())
      return false;
    if (i < text.size() && token(static_cast<unsigned char>(text[i])))
      continue;
    size_t end = i;
    while (end > begin && text[end - 1] == '.')
      --end;
    if (end - begin >= 3 && end - begin <= 64)
      emit(Term{text.substr(begin, end - begin), seed, begin, end, 0});
    begin = i + 1;
  }
  return true;
}
std::vector<Term> terms(const std::string &text, const std::string &seed) {
  std::vector<Term> out;
  visit_terms(text, seed, [&](const Term &term) { out.push_back(term); });
  return out;
}
std::string lower(std::string value) {
  for (char &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
bool valid(const MemoryRetrievalOptions &o) {
  return o.initial_candidates >= 1 && o.initial_candidates <= 20 && o.new_candidates >= 1 &&
         o.new_candidates <= 20 && o.retained >= 1 && o.retained <= 5 && o.final_records >= 1 &&
         o.final_records <= 8 && o.expansion_terms >= 1 && o.expansion_terms <= 20 &&
         o.snippet_bytes >= 80 && o.snippet_bytes <= 4000 && o.context_bytes >= 2 &&
         o.context_bytes <= 65536 && o.scoring_input_bytes >= 1 && o.scoring_input_bytes <= 32768 &&
         std::isfinite(o.seed_threshold) && o.seed_threshold >= 0 && o.seed_threshold <= 1 &&
         o.timeout.count() >= 1 && o.timeout.count() <= 60000;
}
} // namespace
std::string memory_retrieval_strategy_name(MemoryRetrievalStrategy strategy) {
  switch (strategy) {
  case MemoryRetrievalStrategy::Lexical:
    return "lexical";
  case MemoryRetrievalStrategy::Rerank:
    return "rerank";
  case MemoryRetrievalStrategy::Expand:
    return "expand";
  }
  return "invalid";
}
Json::Value memory_scoring_payload(const MemoryScoringRequest &request) {
  Json::Value result;
  result["original_question"] = request.question;
  for (auto field : {"accumulated", "candidates"})
    result[field] = Json::Value(Json::arrayValue);
  auto append = [&](const char *field, const auto &passages) {
    for (const auto &p : passages) {
      Json::Value row;
      row["id"] = p.id;
      row["text"] = p.text;
      result[field].append(row);
    }
  };
  append("accumulated", request.accumulated);
  append("candidates", request.candidates);
  return result;
}
bool memory_retrieve_evidence(const MemoryViewSnapshot &snapshot, const std::string &question,
                              const MemoryRetrievalOptions &o, const MemoryScorer &scorer,
                              MemoryRetrievalResult *out, std::string *error) {
  if (!out)
    return false;
  *out = {};
  if (error)
    error->clear();
  const auto start = std::chrono::steady_clock::now(), deadline = start + o.timeout;
  auto &r = out->report;
  r["schema"] = "agent.memory.retrieval.v1";
  r["strategy"] = memory_retrieval_strategy_name(o.strategy);
  r["view"] = memory_view_name(snapshot.mode);
  r["complete"] = false;
  r["status"] = "invalid";
  r["stages"] = Json::Value(Json::arrayValue);
  r["trace"] = Json::Value(Json::arrayValue);
  r["lexical_queries"] = 0;
  r["scorer_calls"] = 0;
  r["candidate_judgments"] = 0;
  r["scoring_input_bytes"] = Json::UInt64(0);
  r["lexical_query_mode"] = "question_terms_v1";
  auto stop = [&] {
    return (o.cancelled && o.cancelled()) || std::chrono::steady_clock::now() >= deadline;
  };
  auto fail = [&](const std::string &status, const std::string &reason) {
    out->hits.clear();
    out->context = Json::Value(Json::arrayValue);
    r["status"] = status;
    r["complete"] = false;
    r["reason"] = reason;
    if (error)
      *error = reason;
    return false;
  };
  auto fence = [&] {
    if (stop())
      return fail(o.cancelled && o.cancelled() ? "cancelled" : "deadline",
                  "retrieval cancelled or deadline exhausted");
    std::string why;
    if (!memory_view_validate(snapshot, &why))
      return fail("source_changed", why);
    return true;
  };
  if (!valid(o) || r["strategy"] == "invalid" || question.empty() || question.size() > 4096)
    return fail("invalid", "invalid retrieval options or question budget");
  size_t query_terms_omitted = 0;
  for (const auto &term : memory_question_terms(question, &query_terms_omitted))
    r["question_terms"].append(term);
  r["question_terms_omitted"] = Json::UInt64(query_terms_omitted);
  if (!fence())
    return false;
  std::vector<MemorySearchHit> initial;
  auto retrieve = [&](const std::string &query, int limit, std::vector<MemorySearchHit> *hits) {
    if (!fence())
      return false;
    std::string engine, why, index_error;
    r["lexical_queries"] = r["lexical_queries"].asUInt() + 1;
    if (!memory_retrieve_snapshot(snapshot, query, true, false, limit, int(o.snippet_bytes), hits,
                                  &engine, &why, &index_error, 2, true))
      return fail("retrieval_failed", why);
    r["last_lexical_engine"] = engine;
    if (!index_error.empty())
      r["index_unavailable"] = true;
    return fence();
  };
  if (!retrieve(question, int(o.initial_candidates), &initial))
    return false;
  r["initial_candidates"] = Json::UInt64(initial.size());
  auto pool = initial;
  bool fallback = false;
  auto score = [&](std::vector<MemorySearchHit> *candidates,
                   const std::vector<MemorySearchHit> &accumulated) {
    if (!fence())
      return false;
    MemoryScoringRequest request;
    request.question = question;
    request.cancelled = stop;
    for (const auto &hit : *candidates)
      request.candidates.push_back({id(hit), hit.snippet});
    for (const auto &hit : accumulated)
      request.accumulated.push_back({id(hit), hit.snippet});
    const auto input = compact(memory_scoring_payload(request)).size();
    MemoryScoringResult result;
    std::string why;
    bool ok = false;
    if (input > o.scoring_input_bytes)
      why = "scoring input byte budget exhausted";
    else if (!scorer)
      why = "scorer unavailable";
    else {
      r["scorer_calls"] = r["scorer_calls"].asUInt() + 1;
      r["candidate_judgments"] = r["candidate_judgments"].asUInt() + unsigned(candidates->size());
      r["scoring_input_bytes"] = r["scoring_input_bytes"].asUInt64() + Json::UInt64(input);
      try {
        ok = scorer(request, &result, &why);
      } catch (...) {
        why = "scorer threw; no retry";
      }
    }
    Json::Value stage;
    stage["candidates"] = Json::UInt64(candidates->size());
    stage["input_bytes"] = Json::UInt64(input);
    stage["provider"] = result.provider;
    stage["model"] = result.model;
    stage["prompt_revision"] = result.prompt_revision;
    stage["usage"] = result.usage;
    stage["inputs"] = memory_scoring_payload(request);
    stage["references"] = Json::Value(Json::arrayValue);
    for (const auto &hit : *candidates)
      stage["references"].append(memory_passage_evidence(hit.passage));
    std::map<std::string, double> scores;
    if (ok) {
      std::set<std::string> expected;
      for (const auto &hit : *candidates)
        expected.insert(id(hit));
      if (expected.size() != candidates->size() || result.scores.size() != expected.size() ||
          result.provider.empty() || result.provider.size() > 128 || result.model.empty() ||
          result.model.size() > 128 || result.prompt_revision.empty() ||
          result.prompt_revision.size() > 128)
        ok = false;
      for (const auto &entry : result.scores)
        if (!expected.count(entry.first) || !std::isfinite(entry.second) || entry.second < 0 ||
            entry.second > 1 || !scores.emplace(entry).second)
          ok = false;
      if (!ok)
        why = "invalid scorer identity, candidate IDs or finite [0,1] scores";
    }
    if (ok)
      for (const auto &entry : scores)
        stage["scores"][entry.first] = entry.second;
    stage["ok"] = ok;
    if (!ok)
      stage["reason"] = why.substr(0, 256);
    r["stages"].append(stage);
    if (!fence())
      return false;
    if (!ok) {
      fallback = true;
      pool = initial;
      r["fallback_reason"] = why.substr(0, 256);
      return false;
    }
    for (auto &hit : *candidates)
      hit.score = scores.at(id(hit));
    std::stable_sort(candidates->begin(), candidates->end(),
                     [](const auto &a, const auto &b) { return a.score > b.score; });
    return true;
  };
  if (o.strategy != MemoryRetrievalStrategy::Lexical && !pool.empty()) {
    if (score(&pool, {})) {
      if (o.strategy == MemoryRetrievalStrategy::Expand) {
        std::vector<MemorySearchHit> retained;
        for (const auto &hit : pool)
          if (retained.size() < o.retained)
            retained.push_back(hit);
        std::map<std::string, Term> unique;
        static const std::set<std::string> common = {
            "the",  "and", "for",  "that", "this", "with",   "from",   "into",    "are",   "was",
            "were", "has", "have", "not",  "but",  "its",    "what",   "which",   "where", "when",
            "who",  "how", "can",  "will", "must", "should", "source", "evidence"};
        const auto question_lower = lower(question);
        for (const auto &hit : retained) {
          if (hit.score < o.seed_threshold)
            continue;
          for (auto term : terms(hit.snippet, id(hit)))
            if (!common.count(lower(term.text)) &&
                question_lower.find(lower(term.text)) == std::string::npos)
              unique.emplace(lower(term.text), std::move(term));
        }
        for (const auto &passage : snapshot.passages) {
          if (stop())
            return fail("deadline", "retrieval stopped during term selection");
          std::set<std::string> present;
          if (!visit_terms(
                  passage.text, "",
                  [&](const Term &term) {
                    const auto value = lower(term.text);
                    if (unique.count(value))
                      present.insert(value);
                  },
                  stop))
            return fail("deadline", "retrieval stopped during term selection");
          for (auto &entry : unique)
            if (present.count(entry.first))
              ++entry.second.frequency;
        }
        std::vector<Term> ranked;
        for (const auto &entry : unique)
          ranked.push_back(entry.second);
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
          return a.frequency != b.frequency ? a.frequency < b.frequency : a.text < b.text;
        });
        if (ranked.size() > o.expansion_terms)
          r["terms_not_selected"] = Json::UInt64(ranked.size() - o.expansion_terms);
        ranked.resize(std::min(ranked.size(), o.expansion_terms));
        // Search terms separately and interleave their lists, avoiding starvation by one seed.
        std::vector<std::vector<MemorySearchHit>> lists;
        for (const auto &term : ranked) {
          std::vector<MemorySearchHit> hits;
          if (!retrieve(term.text, int(o.new_candidates + o.initial_candidates), &hits))
            return false;
          lists.push_back(std::move(hits));
        }
        std::set<std::string> seen;
        for (const auto &hit : initial)
          seen.insert(id(hit));
        std::vector<MemorySearchHit> fresh;
        for (size_t row = 0;
             row < o.new_candidates + o.initial_candidates && fresh.size() < o.new_candidates;
             ++row)
          for (size_t column = 0; column < lists.size() && fresh.size() < o.new_candidates;
               ++column) {
            if (row >= lists[column].size())
              continue;
            const auto &hit = lists[column][row];
            if (!seen.insert(id(hit)).second)
              continue;
            fresh.push_back(hit);
            Json::Value trace;
            const auto &term = ranked[column];
            trace["seed_id"] = term.seed;
            trace["term"] = term.text;
            trace["seed_text_span"]["begin"] = Json::UInt64(term.begin);
            trace["seed_text_span"]["end"] = Json::UInt64(term.end);
            trace["result_evidence"] = memory_passage_evidence(hit.passage);
            r["trace"].append(trace);
          }
        r["expansion_terms"] = Json::UInt64(ranked.size());
        r["new_candidates"] = Json::UInt64(fresh.size());
        r["retained_candidates"] = Json::UInt64(retained.size());
        if (!fresh.empty()) {
          pool = retained;
          pool.insert(pool.end(), fresh.begin(), fresh.end());
          (void)score(&pool, retained);
        }
      }
    }
  }
  if (!fence())
    return false;
  size_t context_omitted = 0;
  for (const auto &hit : pool) {
    if (out->hits.size() >= o.final_records)
      break;
    Json::Value row;
    row["id"] = id(hit);
    row["evidence"] = memory_passage_evidence(hit.passage);
    row["text"] = hit.snippet;
    row["score"] = hit.score;
    row["source_text_bytes"] = Json::UInt64(hit.passage.text.size());
    row["text_clipped"] = hit.snippet != hit.passage.text;
    row["historical"] = hit.passage.historical;
    row["eligibility_reason"] = hit.passage.eligibility_reason;
    auto proposed = out->context;
    proposed.append(row);
    if (compact(proposed).size() > o.context_bytes) {
      ++context_omitted;
      continue;
    }
    out->context = std::move(proposed);
    out->hits.push_back(hit);
  }
  r["context_bytes"] = Json::UInt64(compact(out->context).size());
  r["context_omitted"] = Json::UInt64(context_omitted);
  r["selection_omitted"] = Json::UInt64(pool.size() - out->hits.size() - context_omitted);
  r["final_records"] = Json::UInt64(out->hits.size());
  r["complete"] = !fallback && context_omitted == 0 && query_terms_omitted == 0;
  r["status"] = fallback              ? "lexical_fallback"
                : context_omitted     ? "context_budget"
                : query_terms_omitted ? "query_budget"
                                      : "complete";
  r["elapsed_ms"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count());
  return fence();
}
} // namespace host_tools_internal
#endif
