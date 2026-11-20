#include "memory_retrieval.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace host_tools_internal;
namespace fs = std::filesystem;
namespace {
std::string compact(const Json::Value &v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}
double elapsed(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}
Json::Value ids(const MemoryRetrievalResult &result) {
  Json::Value out(Json::arrayValue);
  for (const auto &row : result.context)
    out.append(row["id"]);
  return out;
}
} // namespace
int main(int argc, char **argv) {
  try {
    fs::path root, manifest_path, output;
    for (int i = 1; i < argc; ++i) {
      const std::string flag = argv[i];
      if (i + 1 < argc && flag == "--memory-root")
        root = argv[++i];
      else if (i + 1 < argc && flag == "--manifest")
        manifest_path = argv[++i];
      else if (i + 1 < argc && flag == "--output")
        output = argv[++i];
      else
        throw std::runtime_error("usage: agent-memory-lexical-timing --memory-root ISOLATED_COPY "
                                 "--manifest FILE --output NEW.json");
    }
    if (root.empty() || manifest_path.empty() || output.empty() ||
        fs::file_size(manifest_path) > 1024 * 1024)
      throw std::runtime_error("missing inputs or oversized timing manifest");
    Json::Value manifest;
    Json::CharReaderBuilder reader;
    reader["rejectDupKeys"] = true;
    reader["failIfExtra"] = true;
    reader["allowComments"] = false;
    std::ifstream stream(manifest_path);
    std::string error;
    if (!Json::parseFromStream(reader, stream, &manifest, &error) || !manifest.isObject() ||
        manifest.get("schema", "") != "agent.memory.screen.input.v1" ||
        !manifest["cases"].isArray() || manifest["cases"].empty() || manifest["cases"].size() > 20)
      throw std::runtime_error("invalid timing manifest");
    root = fs::absolute(root).lexically_normal();
    output = fs::absolute(output).lexically_normal();
    fs::create_directories(output.parent_path());
    Json::Value receipt;
    receipt["schema"] = "agent.memory.lexical.timing.v1";
    receipt["status"] = "prepared";
    receipt["timing_scope"] = "cold retrieval index followed by warm identical query; OS page "
                              "cache is not flushed; capture is measured separately";
    receipt["provider_requests"] = 0;
    receipt["answerer_calls"] = 0;
    receipt["cases"] = Json::Value(Json::arrayValue);
    std::string revision = "absent";
    auto save = [&] {
      MemoryDocumentTransaction tx;
      if (tx.begin(output.parent_path(), output.filename().string(), revision, {}, &error) !=
              MemoryStoreStatus::Ok ||
          tx.commit(compact(receipt) + "\n", &error) != MemoryStoreStatus::Ok)
        throw std::runtime_error("timing output exists or publication failed; inspect original");
      revision = tx.document().revision;
    };
    save();
    for (const auto &c : manifest["cases"]) {
      if (!c.isObject() || !c["id"].isString() || !c["question"].isString() ||
          c["question"].asString().empty() || c["question"].asString().size() > 4096)
        throw std::runtime_error("invalid timing case");
      const auto &selected = c.isMember("files") ? c["files"] : manifest["files"];
      if (!selected.isArray() || selected.empty() || selected.size() > 128)
        throw std::runtime_error("invalid timing source scope");
      std::vector<std::string> files;
      for (const auto &name : selected) {
        bool declared = false;
        for (const auto &allowed : manifest["files"])
          if (allowed == name)
            declared = true;
        if (!name.isString() || !declared || fs::path(name.asString()).is_absolute())
          throw std::runtime_error("timing case broadens corpus");
        files.push_back((root / name.asString()).string());
      }
      MemoryViewMode view;
      if (!c.get("view", "current").isString() ||
          !memory_view_mode(c.get("view", "current").asString(), &view))
        throw std::runtime_error("invalid timing view");
      auto start = std::chrono::steady_clock::now();
      MemoryViewSnapshot snapshot;
      if (!memory_view_capture(root, files, view, &snapshot, &error))
        throw std::runtime_error("timing capture failed");
      const double capture_ms = elapsed(start);
      {
        MemoryCacheLease lease;
        if (lease.begin(root, {}, &error) != MemoryStoreStatus::Ok || !lease.purge(&error))
          throw std::runtime_error("cannot clear isolated retrieval index");
      }
      MemoryRetrievalOptions options;
      options.strategy = MemoryRetrievalStrategy::Lexical;
      MemoryRetrievalResult cold, warm;
      start = std::chrono::steady_clock::now();
      if (!memory_retrieve_evidence(snapshot, c["question"].asString(), options, {}, &cold,
                                    &error) ||
          !cold.report.get("complete", false).asBool() ||
          cold.report.get("index_unavailable", false).asBool())
        throw std::runtime_error("cold index retrieval incomplete");
      const double cold_ms = elapsed(start);
      start = std::chrono::steady_clock::now();
      if (!memory_retrieve_evidence(snapshot, c["question"].asString(), options, {}, &warm,
                                    &error) ||
          !warm.report.get("complete", false).asBool() ||
          warm.report.get("index_unavailable", false).asBool())
        throw std::runtime_error("warm index retrieval incomplete");
      const double warm_ms = elapsed(start);
      if (ids(cold) != ids(warm))
        throw std::runtime_error("cold/warm evidence changed");
      Json::Value row;
      row["case"] = c["id"];
      row["view"] = memory_view_name(view);
      row["capture_ms"] = capture_ms;
      row["cold_index_ms"] = cold_ms;
      row["warm_index_ms"] = warm_ms;
      row["cold_engine"] = cold.report["last_lexical_engine"];
      row["warm_engine"] = warm.report["last_lexical_engine"];
      row["evidence_ids"] = ids(cold);
      receipt["cases"].append(row);
      save();
    }
    receipt["status"] = "complete";
    save();
    std::cout << compact(receipt) << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
