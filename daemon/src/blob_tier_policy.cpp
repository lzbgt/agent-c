#include "blob_tier_policy.h"

#include "blob_object_store.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace agentd {
namespace {

int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::string blob_rel_path_from_hex(const std::string& hex) {
  if (hex.size() < 4) return "";
  std::string rel = "blobs/sha256/";
  rel.reserve(7 + hex.size());
  rel.append(hex.substr(0, 2));
  rel.push_back('/');
  rel.append(hex.substr(2, 2));
  rel.push_back('/');
  rel.append(hex);
  return rel;
}

bool read_file_bytes(const std::filesystem::path& path, std::string* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  out->clear();
  std::ifstream is(path, std::ios::binary);
  if (!is.is_open()) {
    if (out_error) *out_error = "failed to open blob file";
    return false;
  }
  is.seekg(0, std::ios::end);
  std::streamoff len = is.tellg();
  if (len < 0) len = 0;
  is.seekg(0, std::ios::beg);
  out->resize((size_t)len);
  if (len > 0) {
    is.read(&(*out)[0], len);
    if (!is) {
      if (out_error) *out_error = "failed to read blob file";
      out->clear();
      return false;
    }
  }
  return true;
}

bool file_exists_and_size(const std::filesystem::path& path, int64_t* out_size) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
  const auto sz = std::filesystem::file_size(path, ec);
  if (ec) return false;
  if (out_size) *out_size = (int64_t)sz;
  return true;
}

void maybe_push_error(BlobTierEnforceStats* stats, const std::string& msg) {
  if (!stats || msg.empty()) return;
  if (stats->errors.size() >= 50) return;
  stats->errors.push_back(msg);
}

}  // namespace

bool blob_tier_enforce(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  const BlobTierPolicy& policy,
  BlobTierEnforceStats* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = BlobTierEnforceStats();
  if (!out_stats) {
    if (out_error) *out_error = "missing output";
    return false;
  }
  out_stats->generated_utc_ms = now_utc_ms();

  if (!db_or_null || !db_or_null->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  if (cfg.state_dir.empty()) {
    if (out_error) *out_error = "state_dir not configured";
    return false;
  }

  const int64_t now_ms = out_stats->generated_utc_ms;
  const bool promote_enabled = policy.promote_after_ms > 0;
  const bool object_mode = (cfg.blob_store_mode == "object");
  const bool force_evict = policy.force_evict_all;
  const bool evict_by_age = policy.local_max_age_ms > 0;
  const bool evict_by_size = policy.local_max_bytes > 0;

  const std::filesystem::path state_root(cfg.state_dir);

  if (promote_enabled) {
    if (!object_mode) {
      maybe_push_error(out_stats, "promotion skipped: blob_store_mode is not object");
    } else {
      std::string cfg_err;
      if (!blob_object_store_is_configured(cfg, &cfg_err)) {
        maybe_push_error(out_stats, cfg_err.empty() ? "promotion skipped: object store not configured" : cfg_err);
      } else {
        std::vector<AgentDb::BlobManifestRow> locals;
        std::string db_err;
        if (!db_or_null->list_blob_tier_candidates("local", policy.max_rows, &locals, &db_err)) {
          maybe_push_error(out_stats, db_err.empty() ? "failed to list local blobs" : db_err);
        } else {
          for (const auto& row : locals) {
            if (policy.promote_after_ms <= 0) break;
            if (row.ref_count <= 0) continue;
            if (row.created_utc_ms <= 0) continue;
            if (now_ms - row.created_utc_ms < policy.promote_after_ms) continue;
            if (row.sha256_hex.size() != 64) {
              maybe_push_error(out_stats, "skip promotion (invalid sha256): " + row.blob_id);
              continue;
            }
            const std::string rel = blob_rel_path_from_hex(row.sha256_hex);
            if (rel.empty()) {
              maybe_push_error(out_stats, "skip promotion (invalid path): " + row.blob_id);
              continue;
            }
            const std::filesystem::path abs_path = (state_root / rel).lexically_normal();
            int64_t file_size = 0;
            if (!file_exists_and_size(abs_path, &file_size)) {
              maybe_push_error(out_stats, "skip promotion (missing file): " + row.blob_id);
              continue;
            }
            if (policy.promote_max_bytes > 0 && file_size > policy.promote_max_bytes) {
              maybe_push_error(out_stats, "skip promotion (file too large): " + row.blob_id);
              continue;
            }

            if (!policy.dry_run) {
              std::string bytes;
              std::string rerr;
              if (!read_file_bytes(abs_path, &bytes, &rerr)) {
                maybe_push_error(out_stats, rerr.empty() ? "promotion read failed: " + row.blob_id : rerr);
                continue;
              }
              const std::string object_key = blob_object_store_key_for_hex(cfg, row.sha256_hex);
              if (object_key.empty()) {
                maybe_push_error(out_stats, "promotion failed (object key): " + row.blob_id);
                continue;
              }
              std::string etag;
              std::string perr;
              if (!blob_object_store_put(cfg, object_key, bytes, row.mime, now_ms, &etag, &perr)) {
                maybe_push_error(out_stats, perr.empty() ? "promotion failed: " + row.blob_id : perr);
                continue;
              }
              std::string uerr;
              (void)db_or_null->update_blob_manifest_location(
                row.blob_id,
                row.mime,
                "object",
                object_key,
                etag,
                row.storage_class,
                now_ms,
                &uerr);
              if (!uerr.empty()) {
                maybe_push_error(out_stats, "promotion db update failed: " + row.blob_id);
              }
              if (cfg.blob_store_cache_mode == "none") {
                std::error_code ec;
                std::filesystem::remove(abs_path, ec);
              }
            }
            out_stats->promoted_count += 1;
            out_stats->promoted_bytes += file_size > 0 ? file_size : row.size_bytes;
          }
        }
      }
    }
  }

  if (force_evict || evict_by_age || evict_by_size) {
    std::vector<AgentDb::BlobManifestRow> objects;
    std::string db_err;
    if (!db_or_null->list_blob_tier_candidates("object", policy.max_rows, &objects, &db_err)) {
      maybe_push_error(out_stats, db_err.empty() ? "failed to list object blobs" : db_err);
    } else {
      struct CacheItem {
        AgentDb::BlobManifestRow row;
        std::filesystem::path path;
        int64_t size = 0;
        bool exists = false;
      };
      std::vector<CacheItem> items;
      items.reserve(objects.size());
      for (const auto& row : objects) {
        CacheItem item;
        item.row = row;
        if (row.sha256_hex.size() == 64) {
          const std::string rel = blob_rel_path_from_hex(row.sha256_hex);
          if (!rel.empty()) {
            item.path = (state_root / rel).lexically_normal();
            item.exists = file_exists_and_size(item.path, &item.size);
          }
        }
        if (item.exists) {
          out_stats->total_local_bytes_before += item.size;
        }
        items.push_back(std::move(item));
      }

      auto evict_item = [&](CacheItem& item) {
        if (!item.exists) return;
        if (!policy.dry_run) {
          std::error_code ec;
          std::filesystem::remove(item.path, ec);
        }
        out_stats->evicted_count += 1;
        out_stats->evicted_bytes += item.size;
        item.exists = false;
        item.size = 0;
      };

      if (force_evict) {
        for (auto& item : items) evict_item(item);
      } else if (evict_by_age) {
        for (auto& item : items) {
          if (!item.exists) continue;
          const int64_t last = item.row.last_access_utc_ms;
          if (last > 0 && now_ms - last >= policy.local_max_age_ms) {
            evict_item(item);
          }
        }
      }

      int64_t total_after_age = 0;
      for (const auto& item : items) {
        if (item.exists) total_after_age += item.size;
      }

      if (evict_by_size && total_after_age > policy.local_max_bytes) {
        int64_t total = total_after_age;
        for (auto& item : items) {
          if (!item.exists) continue;
          if (total <= policy.local_max_bytes) break;
          const int64_t sz = item.size;
          evict_item(item);
          total -= sz;
        }
      }

      for (const auto& item : items) {
        if (item.exists) out_stats->total_local_bytes_after += item.size;
      }
    }
  }

  return true;
}

}  // namespace agentd
