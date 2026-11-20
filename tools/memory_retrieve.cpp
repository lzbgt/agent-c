#include "agent_sha256.h"
#include "memory_retrieval.h"
#include "typesafe_scorer.h"
#include <csignal>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

using namespace host_tools_internal;
namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
std::string compact(const Json::Value &v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}
std::string hash(const std::string &s) {
  char digest[65]{};
  agent_sha256_hex_of_bytes(s.data(), s.size(), digest);
  return std::string("sha256:") + digest;
}
Json::Value read_manifest(const fs::path &path) {
  std::error_code ec;
  if (fs::file_size(path, ec) > 1024 * 1024 || ec)
    throw std::runtime_error("manifest unavailable or exceeds 1 MiB");
  std::ifstream stream(path);
  Json::Value doc;
  Json::CharReaderBuilder r;
  r["rejectDupKeys"] = true;
  r["allowComments"] = false;
  r["failIfExtra"] = true;
  r["stackLimit"] = 64;
  std::string error;
  if (!Json::parseFromStream(r, stream, &doc, &error) || !doc.isObject())
    throw std::runtime_error("invalid manifest JSON");
  return doc;
}
std::vector<std::string> files(const fs::path &root, const Json::Value &list) {
  if (!list.isArray() || list.empty() || list.size() > 128)
    throw std::runtime_error("manifest requires 1..128 explicit source files");
  std::vector<std::string> result;
  for (const auto &name : list) {
    if (!name.isString() || fs::path(name.asString()).is_absolute())
      throw std::runtime_error("source names must be relative paths");
    result.push_back((root / name.asString()).string());
  }
  return result;
}
} // namespace
int main(int argc, char **argv) {
  std::signal(SIGINT, interrupt);
  std::signal(SIGTERM, interrupt);
  try {
    fs::path root, manifest_path, output, dotenv;
    bool canary = false, lexical_only = false;
    for (int i = 1; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--help") {
        std::cout << "agent-memory-retrieve --memory-root DIR --manifest FILE --output NEW.json "
                     "[--lexical-only | --typesafe-env DOTENV]\n"
                     "agent-memory-retrieve --canary --typesafe-env DOTENV --output NEW.json\n"
                     "Foreground research only. Use public/sanitized explicit sources. Existing "
                     "output refuses paid replay. No production service is started.\n";
        return 0;
      }
      if (flag == "--canary")
        canary = true;
      else if (flag == "--lexical-only")
        lexical_only = true;
      else if (i + 1 < argc && flag == "--memory-root")
        root = argv[++i];
      else if (i + 1 < argc && flag == "--manifest")
        manifest_path = argv[++i];
      else if (i + 1 < argc && flag == "--output")
        output = argv[++i];
      else if (i + 1 < argc && flag == "--typesafe-env")
        dotenv = argv[++i];
      else
        throw std::runtime_error("invalid research command arguments; use --help");
    }
    if (output.empty() || (canary && lexical_only) || (!lexical_only && dotenv.empty()) ||
        (!canary && (root.empty() || manifest_path.empty())))
      throw std::runtime_error("missing research inputs; use --help");
    output = fs::absolute(output).lexically_normal();
    fs::create_directories(output.parent_path());
    std::string error, revision = "absent";
    Json::Value receipt;
    receipt["schema"] = "agent.memory.screen.v1";
    receipt["status"] = "prepared";
    receipt["canary"] = canary;
    receipt["answerer_evaluation"] = "not performed by this retrieval command";
    receipt["arms"] = Json::Value(Json::arrayValue);
    auto save = [&] {
      MemoryDocumentTransaction tx;
      if (tx.begin(output.parent_path(), output.filename().string(), revision, {}, &error) !=
              MemoryStoreStatus::Ok ||
          tx.commit(compact(receipt) + "\n", &error) != MemoryStoreStatus::Ok)
        throw std::runtime_error(
            "research receipt publication failed or output already exists; no automatic replay");
      revision = tx.document().revision;
    };
    Json::Value manifest;
    MemoryViewSnapshot corpus;
    if (!canary) {
      root = fs::absolute(root).lexically_normal();
      manifest = read_manifest(manifest_path);
      if (manifest.get("schema", "") != "agent.memory.screen.input.v1" ||
          !manifest["cases"].isArray() || manifest["cases"].empty() ||
          manifest["cases"].size() > 20)
        throw std::runtime_error("screen requires 1..20 cases in agent.memory.screen.input.v1");
      if (!memory_view_capture(root, files(root, manifest["files"]), MemoryViewMode::Historical,
                               &corpus, &error))
        throw std::runtime_error("cannot capture declared corpus");
      std::set<std::string> ids;
      for (const auto &c : manifest["cases"]) {
        MemoryViewMode view;
        if (!c.isObject() || !c["id"].isString() ||
            !std::regex_match(c["id"].asString(), std::regex("[A-Za-z0-9_-]{1,64}")) ||
            !ids.insert(c["id"].asString()).second || !c["question"].isString() ||
            c["question"].asString().empty() || c["question"].asString().size() > 4096 ||
            !c.get("view", "current").isString() ||
            !memory_view_mode(c.get("view", "current").asString(), &view))
          throw std::runtime_error("invalid or duplicate held-out case");
      }
      receipt["manifest_revision"] = hash(compact(manifest));
      for (const auto &source : corpus.sources) {
        Json::Value row;
        row["path"] = source.first;
        row["source_id"] = source.second.source_id;
        row["revision"] = source.second.revision;
        receipt["sources"].append(row);
      }
    }
    TypeSafeScorerConfig config;
    if (canary) {
      config.max_attempts = 2;
      config.max_judgments = 2;
      config.attempts_per_call = 1;
      config.input_token_budget = kTypeSafeRequestTokenReserve;
      config.total_timeout = std::chrono::milliseconds(30000);
    }
    if (!lexical_only && !typesafe_key_from_dotenv(dotenv, &config.api_key, &error))
      throw std::runtime_error(error);
    TypeSafeScorer client(config);
    receipt["scorer_model_pin"] = config.model;
    receipt["budget_before_dispatch"] =
        lexical_only ? Json::Value(Json::nullValue) : client.usage();
    if (canary) {
      receipt["public_canary"]["question"] = "Which city hosts Cedarport's pump supplier?";
      receipt["public_canary"]["candidates"][0] = "Cedarport buys pumps from Juniper Works.";
      receipt["public_canary"]["candidates"][1] = "Today's weather in Kingston is sunny.";
    }
    save(); // Durable admission before any authenticated request. Existing file refuses a restart.
    bool complete = true;
    auto cancelled = [] { return interrupted != 0; };
    MemoryScorer scorer = [&](const auto &q, auto *result, auto *why) {
      return client.score(q, result, why);
    };
    if (canary) {
      Json::Value models;
      if (!client.discover(&models, cancelled, &error))
        complete = false;
      else {
        receipt["account_models"] = models;
        MemoryScoringRequest q;
        q.question = receipt["public_canary"]["question"].asString();
        q.cancelled = cancelled;
        for (int i = 0; i < 2; ++i)
          q.candidates.push_back({"evidence:" + std::string(64, char('0' + i)),
                                  receipt["public_canary"]["candidates"][i].asString()});
        MemoryScoringResult result;
        complete = client.score(q, &result, &error);
        receipt["returned_model"] = result.model;
        for (const auto &score : result.scores)
          receipt["scores"][score.first] = score.second;
      }
    } else
      for (const auto &c : manifest["cases"]) {
        if (cancelled() || !memory_view_validate(corpus, &error)) {
          complete = false;
          break;
        }
        MemoryViewMode view;
        memory_view_mode(c.get("view", "current").asString(), &view);
        MemoryViewSnapshot snapshot;
        const auto selected = c.isMember("files") ? c["files"] : manifest["files"];
        // A case can narrow the frozen corpus, never introduce another source.
        for (const auto &path : selected)
          if (!path.isString() || !corpus.sources.count(path.asString()))
            throw std::runtime_error("case scope is outside frozen corpus");
        if (!memory_view_capture(root, files(root, selected), view, &snapshot, &error) ||
            !memory_view_validate(corpus, &error)) {
          complete = false;
          break;
        }
        for (const auto strategy :
             {MemoryRetrievalStrategy::Lexical, MemoryRetrievalStrategy::Rerank,
              MemoryRetrievalStrategy::Expand}) {
          if (lexical_only && strategy != MemoryRetrievalStrategy::Lexical)
            continue;
          receipt["in_flight"]["case"] = c["id"];
          receipt["in_flight"]["strategy"] = memory_retrieval_strategy_name(strategy);
          receipt["usage"] = lexical_only ? Json::Value(Json::nullValue) : client.usage();
          save();
          MemoryRetrievalOptions options;
          options.strategy = strategy;
          options.cancelled = cancelled;
          MemoryRetrievalResult result;
          const bool ok = memory_retrieve_evidence(snapshot, c["question"].asString(), options,
                                                   scorer, &result, &error);
          Json::Value arm;
          arm["case"] = c["id"];
          arm["strategy"] = memory_retrieval_strategy_name(strategy);
          arm["question"] = c["question"];
          arm["report"] = result.report;
          arm["context"] = result.context;
          arm["ok"] = ok;
          receipt["arms"].append(arm);
          receipt.removeMember("in_flight");
          receipt["usage"] = lexical_only ? Json::Value(Json::nullValue) : client.usage();
          save();
          if (!ok || !result.report.get("complete", false).asBool()) {
            complete = false;
            break;
          }
        }
        if (!complete)
          break; // Preserve completed arms; never silently continue past a scorer/budget failure.
      }
    receipt["status"] = complete ? "complete" : "incomplete";
    receipt["usage"] = lexical_only ? Json::Value(Json::nullValue) : client.usage();
    if (interrupted && error.empty())
      error = "research cancelled; completed arms preserved";
    if (!error.empty())
      receipt["reason"] = error;
    save();
    std::cout << compact(receipt) << '\n';
    return complete ? 0 : 2;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
