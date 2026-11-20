#if defined(AGENT_HAVE_JSONCPP)
#include "memory_promotion.h"
#include "agent_sha256.h"
#include "memory_structured.h"
#include <algorithm>
#include <set>
#include <sstream>

namespace host_tools_internal {
namespace {
std::string compact(const Json::Value &value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}
std::string digest(const std::string &bytes) {
  char hash[65]{};
  agent_sha256_hex_of_bytes(bytes.data(), bytes.size(), hash);
  return hash;
}
std::string trim(const std::string &value) {
  auto first = value.find_first_not_of(" \r\n\t");
  return first == std::string::npos
             ? ""
             : value.substr(first, value.find_last_not_of(" \r\n\t") - first + 1);
}
std::string lower(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
bool fail(std::string *error, const std::string &text) {
  if (error)
    *error = text;
  return false;
}
std::string head(const Json::Value &document, const std::string &key) {
  return document["items"].isMember(key) ? memory_record_revision(document["items"][key])
                                         : "absent";
}
bool valid_evidence(const Json::Value &ref) {
  if (!ref.isObject() || ref.get("schema", "") != "agent.memory.evidence.v1" ||
      !ref["path"].isString() || !ref["source_id"].isString() || !ref["revision"].isString() ||
      !memory_revision_valid(ref["revision"].asString()) || ref["revision"] == "absent" ||
      !ref["span"].isObject() || ref["span"].get("kind", "") != "bytes" ||
      !ref["span"]["begin"].isUInt64() || !ref["span"]["end"].isUInt64() ||
      ref["span"]["begin"].asUInt64() >= ref["span"]["end"].asUInt64() ||
      ref["span"]["end"].asUInt64() > kMemoryDocumentMaxBytes)
    return false;
  MemoryPassage p;
  p.path = ref["path"].asString();
  p.source_id = ref["source_id"].asString();
  p.revision = ref["revision"].asString();
  p.begin = ref["span"]["begin"].asUInt64();
  p.end = ref["span"]["end"].asUInt64();
  return compact(memory_passage_evidence(p)) == compact(ref);
}
bool valid_id(const std::string &id) {
  return id.size() == 71 && id.rfind("marker:", 0) == 0 &&
         memory_revision_valid("sha256:" + id.substr(7));
}
Json::Value initial_ledger() {
  Json::Value ledger;
  ledger["schema"] = "agent.memory.promotion.v2";
  ledger["consumed"] = Json::Value(Json::objectValue);
  ledger["receipt_sources"] = Json::Value(Json::arrayValue);
  ledger["receipts"] = Json::Value(Json::objectValue);
  return ledger;
}
Json::Value receipt_for(const MemoryMarker &marker, const std::string &before,
                        const std::string &reason) {
  Json::Value receipt;
  receipt["key"] = marker.key;
  receipt["source_id"] = marker.source.source_id;
  receipt["evidence"] = memory_passage_evidence(marker.source);
  receipt["before_record_revision"] = before;
  receipt["result_record_revision"] = before;
  receipt["outcome"] = reason.empty() ? "applied" : "rejected";
  receipt["reason"] = reason;
  return receipt;
}
} // namespace
std::string memory_record_revision(const Json::Value &record) {
  return "sha256:" + digest(compact(record));
}
std::string memory_marker_id(const MemoryMarker &marker) {
  Json::Value intent;
  intent["schema"] = "agent.memory.marker.v1";
  intent["source_id"] = marker.source.source_id;
  intent["key"] = marker.key;
  intent["kind"] = marker.kind;
  intent["status"] = marker.status;
  intent["value"] = marker.value;
  if (!marker.base.empty())
    intent["expected_record_revision"] = marker.base;
  return "marker:" + digest(compact(intent));
}
bool memory_marker_fields(const std::string &line, MemoryMarker *marker, std::string *error) {
  if (error)
    error->clear();
  if (!marker)
    return false;
  *marker = {};
  auto text = trim(line);
  if (!text.empty() && (text[0] == '-' || text[0] == '*' || text[0] == '+'))
    text = trim(text.substr(1));
  const auto space = text.find_first_of(" \t");
  if (space == std::string::npos) {
    if (lower(text) == "@mem" || lower(text) == "@memory")
      return fail(error, "marker requires fields");
    return false;
  }
  const auto prefix = lower(text.substr(0, space));
  if (prefix != "@mem" && prefix != "@memory")
    return false;
  text = trim(text.substr(space + 1));
  marker->kind = "fact";
  marker->status = "active";
  if (!text.empty() && text[0] == '{') {
    Json::CharReaderBuilder reader;
    reader["failIfExtra"] = true;
    reader["rejectDupKeys"] = true;
    Json::Value value;
    std::istringstream stream(text);
    std::string parse_error;
    if (!Json::parseFromStream(reader, stream, &value, &parse_error) || !value.isObject())
      return fail(error, "marker JSON must be one object on one line");
    const std::set<std::string> fields = {"key", "kind", "status", "value",
                                          "expected_record_revision"};
    for (const auto &name : value.getMemberNames())
      if (!fields.count(name))
        return fail(error, "unknown marker field: " + name);
    if (!value["key"].isString() || !value["value"].isString())
      return fail(error, "marker requires string key and value");
    marker->key = trim(value["key"].asString());
    marker->value = value["value"].asString();
    for (const auto *field : {"kind", "status", "expected_record_revision"})
      if (value.isMember(field) && !value[field].isString())
        return fail(error, std::string("marker field must be string: ") + field);
    if (value.isMember("kind"))
      marker->kind = lower(trim(value["kind"].asString()));
    if (value.isMember("status"))
      marker->status = lower(trim(value["status"].asString()));
    if (value.isMember("expected_record_revision")) {
      marker->base = value["expected_record_revision"].asString();
      if (!memory_revision_valid(marker->base))
        return fail(error, "invalid marker expected_record_revision");
    }
  } else {
    const auto kind_end = text.find_first_of(" \t");
    if (kind_end == std::string::npos)
      return fail(error, "marker requires kind, key and value");
    marker->kind = lower(trim(text.substr(0, kind_end)));
    text = trim(text.substr(kind_end + 1));
    const auto equals = text.find('=');
    if (equals == std::string::npos)
      return fail(error, "marker requires key = value");
    marker->key = trim(text.substr(0, equals));
    marker->value = text.substr(equals + 1);
    if (marker->kind == "deprecated") {
      marker->kind = "fact";
      marker->status = "deprecated";
    }
  }
  if (marker->kind == "pref")
    marker->kind = "preference";
  if (marker->kind != "fact" && marker->kind != "preference" && marker->kind != "task")
    return fail(error, "unsupported marker kind");
  if (marker->status != "active" && marker->status != "deprecated")
    return fail(error, "unsupported marker status");
  int stripped = 0;
  marker->value = trim(memory_strip_private_blocks(marker->value, &stripped));
  if (marker->key.empty() || marker->key.size() > 200 || marker->value.empty() ||
      marker->value.size() > 16384)
    return fail(error, "marker key/value is empty or exceeds its budget");
  return true;
}
bool memory_marker_scan(const std::string &path, const MemoryDocument &document,
                        MemoryMarkerScan *scan, std::string *error) {
  if (!scan)
    return false;
  *scan = {};
  if (!document.exists)
    return true;
  if (std::count(document.text.begin(), document.text.end(), '\n') > 20000)
    return fail(error, "marker source exceeds 20000 lines");
  const bool machine = document.text.find("<!-- AGENT_MEMORY_V1_BEGIN -->") != std::string::npos;
  bool notes = !machine, fenced = false;
  char fence = 0;
  size_t offset = 0;
  int number = 0, excluded_level = 0;
  std::istringstream input(document.text);
  std::string line;
  while (std::getline(input, line)) {
    ++number;
    const auto end = offset + line.size() + (offset + line.size() < document.text.size() ? 1 : 0);
    const auto clean = trim(line);
    if (machine && clean == "<!-- AGENT_MEMORY_V1_NOTES_BEGIN -->")
      notes = true;
    else if (machine && clean == "<!-- AGENT_MEMORY_V1_NOTES_END -->")
      notes = false;
    else if (notes) {
      const bool delimiter = clean.rfind("```", 0) == 0 || clean.rfind("~~~", 0) == 0;
      const bool code =
          fenced || delimiter || line.rfind("    ", 0) == 0 || line.rfind("\t", 0) == 0;
      if (delimiter && (!fenced || clean[0] == fence)) {
        fenced = !fenced;
        fence = clean[0];
      }
      if (!code) {
        const auto level = clean.find_first_not_of('#');
        if (level != std::string::npos && level > 0 && level <= 6 && clean[level] == ' ') {
          if (excluded_level && int(level) <= excluded_level)
            excluded_level = 0;
          const auto heading = lower(trim(clean.substr(level)));
          const std::set<std::string> historical = {
              "deprecated", "history",  "historical", "superseded", "inactive", "obsolete",
              "deleted",    "archived", "已废弃",     "历史",       "已删除"};
          if (historical.count(heading))
            excluded_level = int(level);
        }
        if (excluded_level) {
          offset = end;
          continue;
        }
        MemoryMarker marker;
        std::string reason;
        MemoryPassage source;
        source.path = path;
        source.source_id = document.source_id;
        source.revision = document.revision;
        source.begin = offset;
        source.end = end;
        source.line = number;
        source.end_line = number;
        if (memory_marker_fields(line, &marker, &reason)) {
          marker.source = std::move(source);
          marker.id = memory_marker_id(marker);
          scan->markers.push_back(std::move(marker));
          if (scan->markers.size() > kMemoryPromotionObservationLimit)
            return fail(error, "marker observation budget exceeded");
        } else if (!reason.empty()) {
          Json::Value issue;
          issue["evidence"] = memory_passage_evidence(source);
          issue["error"] = reason;
          scan->errors.append(issue);
          if (scan->errors.size() > 64)
            return fail(error, "marker parse-error budget exceeded");
        }
      }
    }
    offset = end;
  }
  return true;
}
bool memory_promotion_capture(const std::filesystem::path &root,
                              const std::vector<std::string> &files, size_t max_bytes,
                              size_t max_keys, MemoryPromotionPlan *plan, std::string *error) {
  if (!plan)
    return false;
  *plan = {};
  plan->snapshot.root = std::filesystem::absolute(root).lexically_normal();
  plan->snapshot.mode = MemoryViewMode::Historical;
  plan->snapshot.as_of_utc = memory_view_now_utc();
  plan->max_keys = std::clamp<size_t>(max_keys, 1, 1000);
  if (files.size() > 128)
    return fail(error, "promotion source budget exceeded");
  size_t bytes = 0;
  std::set<std::string> requested;
  for (const auto &file : files)
    requested.insert(std::filesystem::absolute(file).lexically_normal().string());
  std::vector<std::string> selected(requested.begin(), requested.end());
  selected.push_back((plan->snapshot.root / "STRUCTURED.md").string());
  for (const auto &file : selected) {
    const auto path = std::filesystem::absolute(file)
                          .lexically_normal()
                          .lexically_relative(plan->snapshot.root)
                          .generic_string();
    if (plan->snapshot.sources.count(path))
      continue;
    if (plan->snapshot.sources.size() >= 128)
      return fail(error, "promotion source/dependency budget exceeded");
    const auto extension = lower(std::filesystem::path(path).extension().string());
    if (extension != ".md" && extension != ".markdown")
      return fail(error, "promotion source is not Markdown");
    for (const auto &part : std::filesystem::path(path))
      if (!part.empty() && part.string()[0] == '.')
        return fail(error, "private promotion source is not eligible");
    MemoryDocument document;
    if (memory_document_read(root, path, &document, error) != MemoryStoreStatus::Ok)
      return false;
    bytes += document.text.size();
    if (bytes > 64 * 1024 * 1024 ||
        (path != "STRUCTURED.md" &&
         document.text.size() > std::min(max_bytes, kMemoryDocumentMaxBytes)))
      return fail(error, "promotion source byte budget exceeded: " + path);
    plan->snapshot.sources.emplace(path, std::move(document));
  }
  Json::Value authority;
  if (!memory_structured_parse_doc(plan->snapshot.sources.at("STRUCTURED.md").text, &authority,
                                   error) ||
      !memory_promotion_ledger_valid(authority, error))
    return false;
  const auto &ledger = authority["promotion_ledger"];
  std::set<std::string> new_ids;
  for (const auto &source : plan->snapshot.sources) {
    if (!requested.count((plan->snapshot.root / source.first).string()))
      continue;
    MemoryMarkerScan scan;
    if (!memory_marker_scan(source.first, source.second, &scan, error))
      return false;
    for (const auto &marker : scan.markers)
      if (!ledger["receipts"].isMember(marker.id) && !ledger["consumed"].isMember(marker.id))
        new_ids.insert(marker.id);
    plan->markers.insert(plan->markers.end(), std::make_move_iterator(scan.markers.begin()),
                         std::make_move_iterator(scan.markers.end()));
    for (const auto &issue : scan.errors)
      plan->errors.append(issue);
    if (new_ids.size() > kMemoryPromotionReceiptLimit ||
        plan->markers.size() > kMemoryPromotionObservationLimit || plan->errors.size() > 64)
      return fail(error, "promotion new-intent/observation/error budget exceeded");
  }
  return memory_view_validate(plan->snapshot, error);
}
bool memory_promotion_validate_dependencies(MemoryViewSnapshot *snapshot, std::string *error) {
  if (!snapshot)
    return false;
  size_t bytes = 0, references = 0, scanned_markers = 0;
  for (const auto &source : snapshot->sources)
    bytes += source.second.text.size();
  std::map<std::string, std::map<std::string, MemoryMarker>> scans;
  for (auto &passage : snapshot->passages) {
    if (passage.pointer.empty() || passage.historical || !passage.record.isMember("derivation"))
      continue;
    const auto &derivation = passage.record["derivation"];
    auto invalidate = [&] {
      passage.historical = true;
      passage.eligibility_reason = "promotion_source_changed_or_unavailable";
    };
    if (!derivation.isObject() ||
        derivation.get("schema", "") != "agent.memory.marker_derivation.v1" ||
        !derivation["markers"].isArray() || derivation["markers"].empty()) {
      invalidate();
      continue;
    }
    for (const auto &original : derivation["markers"]) {
      if (++references > 20000)
        return fail(error, "promotion dependency reference budget exceeded");
      if (!original.isObject() || !original["id"].isString() || !original["evidence"].isObject() ||
          !valid_evidence(original["evidence"])) {
        invalidate();
        break;
      }
      const auto path = original["evidence"]["path"].asString();
      if (!snapshot->sources.count(path)) {
        if (snapshot->sources.size() >= 128)
          return fail(error, "promotion dependency source budget exceeded");
        MemoryDocument document;
        if (memory_document_read(snapshot->root, path, &document, error) != MemoryStoreStatus::Ok)
          return false;
        bytes += document.text.size();
        if (bytes > 64 * 1024 * 1024)
          return fail(error, "promotion dependency exceeds 64 MiB");
        snapshot->sources.emplace(path, std::move(document));
      }
      if (!scans.count(path)) {
        MemoryMarkerScan scan;
        if (!memory_marker_scan(path, snapshot->sources.at(path), &scan, error))
          return false;
        scanned_markers += scan.markers.size();
        if (scanned_markers > kMemoryPromotionObservationLimit)
          return fail(error, "promotion dependency marker budget exceeded");
        auto &markers = scans[path];
        for (auto &marker : scan.markers)
          markers.emplace(marker.id, std::move(marker));
      }
      const auto found = scans.at(path).find(original["id"].asString());
      if (found == scans.at(path).end()) {
        invalidate();
        break;
      }
      const auto &marker = found->second;
      if (marker.key != passage.key || Json::Value(marker.value) != passage.record["value"] ||
          marker.kind != passage.kind || marker.status != passage.status) {
        invalidate();
        break;
      }
      passage.supporting_evidence.append(memory_passage_evidence(marker.source));
    }
  }
  return true;
}
namespace {
bool valid_receipt(const std::string &id, const Json::Value &receipt) {
  return valid_id(id) && receipt.isObject() && receipt["key"].isString() &&
         receipt["source_id"].isString() && receipt["before_record_revision"].isString() &&
         receipt["result_record_revision"].isString() && valid_evidence(receipt["evidence"]) &&
         receipt["evidence"]["source_id"] == receipt["source_id"] &&
         memory_revision_valid(receipt["before_record_revision"].asString()) &&
         memory_revision_valid(receipt["result_record_revision"].asString()) &&
         (receipt.get("outcome", "") == "applied" || receipt.get("outcome", "") == "rejected") &&
         receipt["reason"].isString();
}
Json::Value original_receipt_ref(const Json::Value &ledger, const std::string &id) {
  const auto &source = ledger["receipt_sources"][ledger["consumed"][id].asUInt()];
  MemoryPassage passage;
  passage.path = "STRUCTURED.md";
  passage.source_id = source["source_id"].asString();
  passage.revision = source["revision"].asString();
  passage.pointer = "/promotion_ledger/receipts/" + memory_pointer_token(id);
  return memory_passage_evidence(passage);
}
bool compact_prior_receipts(const Json::Value &original, const MemoryPromotionPlan &plan,
                            MemoryPromotionUpdate *update) {
  const auto &prior = original["promotion_ledger"]["receipts"];
  auto &ledger = update->ledger;
  const auto found = plan.snapshot.sources.find("STRUCTURED.md");
  if (update->compacted || !prior.isObject() || prior.empty() ||
      found == plan.snapshot.sources.end() || !found->second.exists ||
      ledger["consumed"].size() + prior.size() > kMemoryPromotionConsumedLimit ||
      ledger["receipt_sources"].size() >= kMemoryPromotionReceiptSources)
    return false;
  Json::Value source;
  source["source_id"] = found->second.source_id;
  source["revision"] = found->second.revision;
  source["evidence_revisions"] = Json::Value(Json::arrayValue);
  std::set<std::string> pins;
  const auto index = ledger["receipt_sources"].size();
  for (const auto &id : prior.getMemberNames()) {
    ledger["consumed"][id] = index;
    ledger["receipts"].removeMember(id);
    pins.insert(prior[id]["evidence"]["revision"].asString());
  }
  for (const auto &revision : pins)
    source["evidence_revisions"].append(revision);
  ledger["receipt_sources"].append(source);
  update->compacted = prior.size();
  return true;
}
} // namespace
bool memory_promotion_ledger_valid(const Json::Value &document, std::string *error) {
  if (!document.isMember("promotion_ledger"))
    return true;
  const auto &ledger = document["promotion_ledger"];
  if (!ledger.isObject() || ledger.get("schema", "") != "agent.memory.promotion.v2" ||
      !ledger["receipts"].isObject() || ledger["receipts"].size() > kMemoryPromotionReceiptLimit ||
      !ledger["consumed"].isObject() || ledger["consumed"].size() > kMemoryPromotionConsumedLimit ||
      !ledger["receipt_sources"].isArray() ||
      ledger["receipt_sources"].size() > kMemoryPromotionReceiptSources)
    return fail(error,
                "invalid promotion ledger; restore a verified original instead of resetting it");
  std::vector<size_t> counts(ledger["receipt_sources"].size(), 0);
  for (const auto &id : ledger["receipts"].getMemberNames())
    if (!valid_receipt(id, ledger["receipts"][id]))
      return fail(error, "corrupt promotion receipt; source unchanged");
  for (const auto &id : ledger["consumed"].getMemberNames()) {
    const auto &index = ledger["consumed"][id];
    if (!valid_id(id) || ledger["receipts"].isMember(id) || !index.isUInt() ||
        index.asUInt() >= counts.size())
      return fail(error, "invalid or overlapping compacted receipt identity");
    ++counts[index.asUInt()];
  }
  std::set<std::string> revisions;
  for (Json::ArrayIndex i = 0; i < ledger["receipt_sources"].size(); ++i) {
    const auto &source = ledger["receipt_sources"][i];
    if (!source.isObject() || !source["source_id"].isString() ||
        source["source_id"].asString().size() != 71 ||
        source["source_id"].asString().rfind("memory:", 0) != 0 ||
        !memory_revision_valid("sha256:" + source["source_id"].asString().substr(7)) ||
        !source["revision"].isString() || source["revision"] == "absent" ||
        !memory_revision_valid(source["revision"].asString()) ||
        !revisions.insert(source["revision"].asString()).second || !counts[i] ||
        !source["evidence_revisions"].isArray() || source["evidence_revisions"].empty() ||
        source["evidence_revisions"].size() > counts[i])
      return fail(error, "invalid original receipt source or pin set");
    std::set<std::string> pins;
    for (const auto &pin : source["evidence_revisions"])
      if (!pin.isString() || pin == "absent" || !memory_revision_valid(pin.asString()) ||
          !pins.insert(pin.asString()).second)
        return fail(error, "invalid compacted receipt evidence pin");
  }
  return true;
}
bool memory_promotion_prepare(const std::filesystem::path &root, const Json::Value &document,
                              const MemoryPromotionPlan &plan, MemoryPromotionUpdate *update,
                              std::string *error, bool retain_sources) {
  if (!update || !memory_promotion_ledger_valid(document, error))
    return false;
  *update = {};
  update->ledger =
      document.isMember("promotion_ledger") ? document["promotion_ledger"] : initial_ledger();
  if (std::filesystem::weakly_canonical(root) !=
      std::filesystem::weakly_canonical(plan.snapshot.root))
    return fail(error, "promotion root mismatch");
  if (!memory_view_validate(plan.snapshot, error))
    return false;
  std::map<Json::ArrayIndex, Json::Value> original_tables;
  size_t captured_bytes = 0;
  for (const auto &source : plan.snapshot.sources)
    captured_bytes += source.second.text.size();
  auto load_receipt = [&](const std::string &id) -> bool {
    if (update->ledger["receipts"].isMember(id)) {
      update->resolved_receipts[id] = update->ledger["receipts"][id];
      return true;
    }
    const auto index = update->ledger["consumed"][id].asUInt();
    const auto &source = update->ledger["receipt_sources"][index];
    if (!original_tables.count(index)) {
      MemoryDocument original;
      if (memory_document_read_revision(root, "STRUCTURED.md", source["revision"].asString(),
                                        &original, error) != MemoryStoreStatus::Ok ||
          original.source_id != source["source_id"].asString())
        return fail(error, "compacted receipt original missing, corrupt or wrong scope; consumed "
                           "intent stays held");
      captured_bytes += original.text.size();
      if (captured_bytes > 64 * 1024 * 1024)
        return fail(error, "compacted receipt lookup exceeds 64 MiB; no intent replayed");
      Json::Value parsed;
      if (!memory_structured_decode(original.text, &parsed, error) ||
          !parsed["promotion_ledger"].isObject() ||
          !parsed["promotion_ledger"]["receipts"].isObject())
        return fail(error, "invalid original promotion receipt table");
      original_tables[index] = parsed["promotion_ledger"]["receipts"];
      ++update->archive_reads;
      update->archive_bytes += original.text.size();
    }
    const auto &receipt = original_tables[index][id];
    if (!valid_receipt(id, receipt))
      return fail(error, "original compacted receipt absent or invalid; intent stays consumed");
    bool pinned = false;
    for (const auto &pin : source["evidence_revisions"])
      if (pin == receipt["evidence"]["revision"])
        pinned = true;
    if (!pinned)
      return fail(error, "compacted receipt original evidence pin is missing");
    update->resolved_receipts[id] = receipt;
    return true;
  };
  std::map<std::string, std::vector<MemoryMarker>> groups;
  std::set<std::string> ids;
  for (const auto &marker : plan.markers) {
    if (!ids.insert(marker.id).second)
      continue;
    if (marker.id != memory_marker_id(marker))
      return fail(error, "promotion marker identity mismatch");
    const auto source = plan.snapshot.sources.find(marker.source.path);
    if (source == plan.snapshot.sources.end() ||
        marker.source.source_id != source->second.source_id ||
        marker.source.revision != source->second.revision ||
        marker.source.begin >= marker.source.end || marker.source.end > source->second.text.size())
      return fail(error, "promotion marker source/span mismatch");
    MemoryMarker grounded;
    if (!memory_marker_fields(source->second.text.substr(marker.source.begin,
                                                         marker.source.end - marker.source.begin),
                              &grounded, error))
      return fail(error, "promotion marker no longer matches captured source");
    grounded.source = marker.source;
    if (memory_marker_id(grounded) != marker.id)
      return fail(error, "promotion intent differs from original bytes");
    if (update->ledger["receipts"].isMember(marker.id) ||
        update->ledger["consumed"].isMember(marker.id)) {
      if (!load_receipt(marker.id))
        return false;
      const auto &old = update->resolved_receipts.at(marker.id);
      if (old["key"] != marker.key || old["source_id"] != marker.source.source_id)
        return fail(error, "promotion receipt identity mismatch");
    } else
      groups[marker.key].push_back(marker);
  }
  size_t keys = 0;
  for (const auto &group : groups) {
    if (keys < plan.max_keys &&
        update->ledger["receipts"].size() + group.second.size() > kMemoryPromotionReceiptLimit)
      (void)compact_prior_receipts(document, plan, update);
    if (keys >= plan.max_keys ||
        update->ledger["receipts"].size() + group.second.size() > kMemoryPromotionReceiptLimit) {
      update->deferred += group.second.size();
      continue;
    }
    ++keys;
    const auto before = head(document, group.first);
    std::vector<MemoryMarker> eligible;
    std::map<std::string, std::string> reasons;
    for (const auto &marker : group.second) {
      if (before != "absent" && marker.base.empty())
        reasons[marker.id] = "unanchored_replacement";
      else if (!marker.base.empty() && marker.base != before)
        reasons[marker.id] = "record_revision_conflict";
      else
        eligible.push_back(marker);
    }
    if (!eligible.empty()) {
      const auto &first = eligible.front();
      const bool conflict =
          std::any_of(eligible.begin() + 1, eligible.end(), [&](const auto &marker) {
            return marker.kind != first.kind || marker.status != first.status ||
                   marker.value != first.value;
          });
      if (conflict)
        for (const auto &marker : eligible)
          reasons[marker.id] = "competing_observations";
      else {
        Json::Value entry;
        entry["key"] = first.key;
        entry["kind"] = first.kind;
        entry["status"] = first.status;
        entry["value"] = first.value;
        entry["sources"] = Json::Value(Json::arrayValue);
        for (const auto &marker : eligible) {
          entry["sources"].append(marker.id);
          entry["sources"].append(marker.source.path + "#L" + std::to_string(marker.source.line));
        }
        update->entries.append(entry);
        update->accepted[group.first] = eligible;
      }
    }
    for (const auto &marker : group.second) {
      update->ledger["receipts"][marker.id] = receipt_for(marker, before, reasons[marker.id]);
      ++update->new_receipts;
    }
  }
  // Originals are actual captured heads, never the proposed structured output.
  if (retain_sources)
    for (const auto &source : plan.snapshot.sources)
      if (source.second.exists && !memory_document_retain(root, source.first, source.second, error))
        return false;
  return true;
}
namespace {
void build_promotion_report(const Json::Value &document, const MemoryPromotionPlan &plan,
                            MemoryPromotionUpdate *update) {
  auto &report = update->report;
  report["schema"] = "agent.memory.promotion.report.v1";
  report["decisions"] = Json::Value(Json::arrayValue);
  report["errors"] = plan.errors;
  int unresolved = 0, duplicates = 0;
  size_t decisions = 0;
  Json::Value fresh(Json::arrayValue), pending_rows(Json::arrayValue), ordinary(Json::arrayValue);
  std::set<std::string> ids;
  for (const auto &marker : plan.markers) {
    if (!ids.insert(marker.id).second)
      continue;
    const bool archived = update->ledger["consumed"].isMember(marker.id);
    if (!update->ledger["receipts"].isMember(marker.id) &&
        !update->resolved_receipts.count(marker.id))
      continue;
    auto receipt =
        archived ? update->resolved_receipts.at(marker.id) : update->ledger["receipts"][marker.id];
    receipt["receipt_storage"] = archived ? "archived" : "current";
    if (archived)
      receipt["original_receipt_evidence"] = original_receipt_ref(update->ledger, marker.id);
    receipt["id"] = marker.id;
    const auto current = head(document, marker.key);
    receipt["current_record_revision"] = current;
    const bool pending =
        receipt["outcome"] == "rejected" && receipt["before_record_revision"] == current;
    receipt["unresolved"] = pending;
    if (pending)
      ++unresolved;
    const bool prior =
        !update->accepted.count(marker.key) ||
        std::none_of(update->accepted[marker.key].begin(), update->accepted[marker.key].end(),
                     [&](const auto &item) { return item.id == marker.id; });
    if (prior && receipt["outcome"] == "applied")
      ++duplicates;
    ++decisions;
    auto &rows = !update->resolved_receipts.count(marker.id) ? fresh
                 : pending                                   ? pending_rows
                                                             : ordinary;
    if (rows.size() < kMemoryPromotionReceiptLimit)
      rows.append(receipt);
  }
  for (const auto *rows : {&fresh, &pending_rows, &ordinary})
    for (const auto &row : *rows)
      if (report["decisions"].size() < kMemoryPromotionReceiptLimit)
        report["decisions"].append(row);
  report["decisions_omitted"] = Json::UInt64(decisions - report["decisions"].size());
  report["details_incomplete"] = decisions > report["decisions"].size();
  report["observations_checked"] = Json::UInt64(ids.size());
  report["new_receipts"] = Json::UInt64(update->new_receipts);
  report["duplicate_applied"] = duplicates;
  report["unresolved"] = unresolved;
  report["deferred"] = Json::UInt64(update->deferred);
  report["complete"] = unresolved == 0 && update->deferred == 0 && plan.errors.empty();
  report["receipt_count"] = update->ledger["receipts"].size();
  report["compacted_receipts"] = Json::UInt64(update->compacted);
  report["consumed_receipt_count"] = update->ledger["consumed"].size();
  report["receipt_source_count"] = update->ledger["receipt_sources"].size();
  report["archive_reads"] = Json::UInt64(update->archive_reads);
  report["archive_bytes"] = Json::UInt64(update->archive_bytes);
}
} // namespace
Json::Value memory_promotion_preview_report(const Json::Value &document,
                                            const MemoryPromotionPlan &plan,
                                            MemoryPromotionUpdate *update) {
  build_promotion_report(document, plan, update);
  auto result = update->report;
  result["preview"] = true;
  result["would_apply_keys"] = update->entries.size();
  result["would_record_receipts"] = Json::UInt64(update->new_receipts);
  result["would_compact_receipts"] = Json::UInt64(update->compacted);
  result.removeMember("new_receipts");
  result.removeMember("compacted_receipts");
  for (auto &row : result["decisions"])
    if (!update->resolved_receipts.count(row["id"].asString()))
      row["proposed"] = true;
  return result;
}
bool memory_promotion_finish(Json::Value *document, const MemoryPromotionPlan &plan,
                             MemoryPromotionUpdate *update, bool *metadata_changed,
                             std::string *error) {
  if (!document || !update || !metadata_changed)
    return false;
  for (const auto &group : update->accepted) {
    Json::Value derivation;
    derivation["schema"] = "agent.memory.marker_derivation.v1";
    derivation["markers"] = Json::Value(Json::arrayValue);
    for (const auto &marker : group.second) {
      Json::Value source;
      source["id"] = marker.id;
      source["evidence"] = memory_passage_evidence(marker.source);
      derivation["markers"].append(source);
    }
    (*document)["items"][group.first]["derivation"] = derivation;
    const auto after = head(*document, group.first);
    for (const auto &marker : group.second)
      update->ledger["receipts"][marker.id]["result_record_revision"] = after;
  }
  build_promotion_report(*document, plan, update);
  *metadata_changed = update->new_receipts > 0 || update->compacted > 0;
  if (*metadata_changed)
    (*document)["promotion_ledger"] = update->ledger;
  return memory_promotion_ledger_valid(*document, error) &&
         memory_view_validate(plan.snapshot, error);
}
} // namespace host_tools_internal
#endif
