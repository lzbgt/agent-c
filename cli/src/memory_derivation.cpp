#if defined(AGENT_HAVE_JSONCPP)
#include "memory_derivation.h"
#include <algorithm>
#include <ctime>
#include <set>
namespace host_tools_internal {
namespace {
bool fail(std::string *error, const std::string &reason) {
  if (error)
    *error = reason;
  return false;
}
Json::Value state(const std::string &value, const std::string &reason = "") {
  Json::Value out;
  out["schema"] = "agent.memory.derivation_state.v1";
  out["state"] = value;
  out["inputs_current"] = value == "current";
  out["claims_verified"] = false;
  if (!reason.empty())
    out["reason"] = reason;
  return out;
}
std::string compact(const Json::Value &value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}
bool utc(const std::string &value) {
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value[19] != 'Z')
    return false;
  for (size_t i = 0; i < value.size(); ++i)
    if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 &&
        (value[i] < '0' || value[i] > '9'))
      return false;
  const int y = std::stoi(value.substr(0, 4)), m = std::stoi(value.substr(5, 2)),
            d = std::stoi(value.substr(8, 2));
  const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return y >= 1970 && m >= 1 && m <= 12 && d >= 1 &&
         d <= days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) &&
         std::stoi(value.substr(11, 2)) <= 23 && std::stoi(value.substr(14, 2)) <= 59 &&
         std::stoi(value.substr(17, 2)) <= 59;
}
bool public_path(const std::string &path) {
  const std::filesystem::path name(path);
  if (path.empty() || name.is_absolute() || name.generic_string() != path ||
      name.lexically_normal().generic_string() != path || path.back() == '/' ||
      path.find('\\') != std::string::npos)
    return false;
  auto extension = name.extension().string();
  for (auto &c : extension)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  if (extension != ".md" && extension != ".markdown")
    return false;
  for (const auto &part : name)
    if (!part.empty() && part.string()[0] == '.')
      return false;
  return true;
}
} // namespace
Json::Value memory_derivation_make(const MemoryViewSnapshot &snapshot,
                                   const Json::Value &evidence) {
  Json::Value out;
  out["schema"] = "agent.memory.derivation.v1";
  out["view_policy"] = "agent.memory.view.v1";
  out["view"] = memory_view_name(snapshot.mode);
  out["as_of_utc"] = snapshot.as_of_utc;
  out["input_revisions"] = Json::Value(Json::arrayValue);
  out["evidence"] = evidence;
  for (const auto &source : snapshot.sources) {
    Json::Value row;
    row["path"] = source.first;
    row["revision"] = source.second.revision;
    row["source_id"] = source.second.source_id;
    out["input_revisions"].append(row);
  }
  return out;
}
bool memory_derivation_paths(const Json::Value &manifest, std::vector<std::string> *paths,
                             std::string *error) {
  if (!paths)
    return false;
  paths->clear();
  if (!manifest.isObject() || manifest.get("schema", "") != "agent.memory.derivation.v1" ||
      manifest.get("view_policy", "") != "agent.memory.view.v1" ||
      manifest.get("view", "") != "current" || !manifest["as_of_utc"].isString() ||
      !utc(manifest["as_of_utc"].asString()) || !manifest["input_revisions"].isArray() ||
      manifest["input_revisions"].empty() || manifest["input_revisions"].size() > 128 ||
      !manifest["evidence"].isArray() || manifest["evidence"].empty() ||
      manifest["evidence"].size() > 20000)
    return fail(error, "missing or unsupported derivation manifest");
  std::set<std::string> seen;
  for (const auto &source : manifest["input_revisions"]) {
    if (!source.isObject() || !source["path"].isString() ||
        !public_path(source["path"].asString()) || !source["revision"].isString() ||
        !memory_revision_valid(source["revision"].asString()) || !source["source_id"].isString() ||
        !seen.insert(source["path"].asString()).second)
      return fail(error, "invalid or duplicate derivation input");
    paths->push_back(source["path"].asString());
  }
  std::set<std::string> ids;
  for (const auto &ref : manifest["evidence"]) {
    if (!ref.isObject() || ref.get("schema", "") != "agent.memory.evidence.v1" ||
        !ref["id"].isString() || !ref["path"].isString() || !seen.count(ref["path"].asString()) ||
        !ids.insert(ref["id"].asString()).second)
      return fail(error, "invalid or duplicate derivation evidence");
  }
  return true;
}
static Json::Value evaluate(const Json::Value &manifest, const MemoryViewSnapshot &current,
                            const std::map<std::string, std::string> &eligible) {
  std::vector<std::string> paths;
  std::string error;
  if (!memory_derivation_paths(manifest, &paths, &error))
    return state("unverified", error);
  if (current.mode != MemoryViewMode::Current ||
      manifest["as_of_utc"].asString() > current.as_of_utc)
    return state("unverified", "invalid evaluation view or future input cutoff");
  for (const auto &source : manifest["input_revisions"]) {
    const auto found = current.sources.find(source["path"].asString());
    if (found == current.sources.end())
      return state("unavailable", "input was not captured");
    if (source["source_id"] != found->second.source_id)
      return state("unverified", "input scope/source mismatch");
    if (source["revision"] != found->second.revision)
      return state("stale", found->second.exists ? "input_revision_changed" : "input_retired");
  }
  for (const auto &ref : manifest["evidence"]) {
    const auto found = eligible.find(ref["id"].asString());
    if (found == eligible.end() || found->second != compact(ref))
      return state("stale", "evidence_ineligible_or_altered");
  }
  return state("current");
}
std::vector<Json::Value> memory_derivation_evaluate_many(const std::vector<Json::Value> &manifests,
                                                         const MemoryViewSnapshot &current) {
  std::map<std::string, std::string> eligible;
  auto add = [&](const Json::Value &ref) { eligible[ref["id"].asString()] = compact(ref); };
  for (const auto &passage : current.passages) {
    add(memory_passage_evidence(passage));
    for (const auto &ref : passage.supporting_evidence)
      add(ref);
  }
  std::vector<Json::Value> out;
  for (const auto &manifest : manifests)
    out.push_back(evaluate(manifest, current, eligible));
  return out;
}
Json::Value memory_derivation_evaluate(const Json::Value &manifest,
                                       const MemoryViewSnapshot &current) {
  return memory_derivation_evaluate_many({manifest}, current)[0];
}
bool memory_derivation_capture(const std::filesystem::path &root,
                               const std::vector<Json::Value> &manifests,
                               MemoryViewSnapshot *current, std::string *error) {
  std::set<std::string> selected;
  for (const auto &manifest : manifests) {
    std::vector<std::string> paths;
    std::string issue;
    if (!memory_derivation_paths(manifest, &paths, &issue))
      continue;
    selected.insert(paths.begin(), paths.end());
  }
  if (selected.size() > 128)
    return fail(error, "derived input union exceeds 128 sources");
  std::vector<std::string> files;
  for (const auto &path : selected)
    files.push_back((root / path).string());
  return memory_view_capture(root, files, MemoryViewMode::Current, current, error);
}
} // namespace host_tools_internal
#endif
