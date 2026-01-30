#include "file_persistor.h"

#include "session_store.h"

#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <new>

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
  return std::filesystem::current_path().string();
}

struct FilePersistorCtx {
  SessionStoreConfig cfg;
};

static agent_status_t fp_load(void* vctx, const char* session_id, agent_session_t** out_session) {
  if (!vctx || !session_id || !out_session) return AGENT_ERR_INVALID_ARGUMENT;
  const auto* ctx = static_cast<const FilePersistorCtx*>(vctx);
  return session_store_load(ctx->cfg, session_id, out_session);
}

static agent_status_t fp_save(void* vctx, const char* session_id, const agent_session_t* session) {
  if (!vctx || !session_id || !session) return AGENT_ERR_INVALID_ARGUMENT;
  const auto* ctx = static_cast<const FilePersistorCtx*>(vctx);
  return session_store_save(ctx->cfg, session_id, session);
}

static agent_status_t fp_del(void* vctx, const char* session_id) {
  if (!vctx || !session_id) return AGENT_ERR_INVALID_ARGUMENT;
  const auto* ctx = static_cast<const FilePersistorCtx*>(vctx);
  return session_store_delete(ctx->cfg, session_id);
}

static agent_status_t fp_list(void* vctx, agent_session_id_sink_fn sink, void* sink_ctx) {
  if (!vctx || !sink) return AGENT_ERR_INVALID_ARGUMENT;
  const auto* ctx = static_cast<const FilePersistorCtx*>(vctx);
  std::vector<std::string> ids;
  agent_status_t st = session_store_list(ctx->cfg, &ids);
  if (st != AGENT_OK) return st;
  for (const auto& id : ids) {
    sink(sink_ctx, id.c_str());
  }
  return AGENT_OK;
}

static void fp_destroy(void* vctx) {
  auto* ctx = static_cast<FilePersistorCtx*>(vctx);
  delete ctx;
}

agent_status_t agent_file_persistor_create(const char* sessions_root_dir_or_null, agent_persistor_t* out) {
  if (!out) return AGENT_ERR_INVALID_ARGUMENT;
  *out = agent_persistor_t{};

  std::string root;
  if (sessions_root_dir_or_null && sessions_root_dir_or_null[0]) {
    root = sessions_root_dir_or_null;
  } else {
    root = (std::filesystem::path(home_dir_best_effort()) / ".agent" / "sessions").string();
  }

  auto* ctx = new (std::nothrow) FilePersistorCtx();
  if (!ctx) return AGENT_ERR_OOM;
  ctx->cfg.root_dir = root;

  out->ctx = ctx;
  out->load = fp_load;
  out->save = fp_save;
  out->del = fp_del;
  out->list = fp_list;
  out->destroy = fp_destroy;
  return AGENT_OK;
}
