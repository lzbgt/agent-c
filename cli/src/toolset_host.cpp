#include "toolset_host.h"
#include "toolset_host_internal.h"

#include <filesystem>
#include <new>
#include <string>

namespace {

using namespace host_tools_internal;
static agent_status_t host_tools_execute(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  if (!vctx || !tool_name || !arguments_json || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  HostToolCtx* ctx = (HostToolCtx*)vctx;
  const std::string name(tool_name);
  if (ctx->policy == HostToolsetPolicyMode::ReadOnly) {
    if (name == "shell_exec" || name == "proc_exec" || name == "file_apply_patch") {
#if !defined(AGENT_HAVE_JSONCPP)
      return set_result(out_result, "{\"ok\":false,\"error\":\"tool disabled by policy\",\"data\":{}}");
#else
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "tool disabled by policy";
      Json::Value d(Json::objectValue);
      d["tool_name"] = name;
      d["policy"] = "readonly";
      o["data"] = d;
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      return set_result(out_result, Json::writeString(wb, o));
#endif
    }
  }
  if (!ctx->exec_enabled) {
    if (name == "shell_exec" || name == "proc_exec") {
#if !defined(AGENT_HAVE_JSONCPP)
      return set_result(out_result, "{\"ok\":false,\"error\":\"tool disabled by sandbox\",\"data\":{}}");
#else
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "tool disabled by sandbox";
      Json::Value d(Json::objectValue);
      d["tool_name"] = name;
      d["sandbox"] = "scoped";
      o["data"] = d;
      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      return set_result(out_result, Json::writeString(wb, o));
#endif
    }
  }
  if (name == "shell_exec") {
    return tool_shell_exec(ctx, arguments_json, out_result);
  }
  if (name == "proc_exec") {
    return tool_proc_exec(ctx, arguments_json, out_result);
  }
  if (name == "file_apply_patch") {
    return tool_file_apply_patch(ctx, arguments_json, out_result);
  }
  if (name == "fs_stat") {
    return tool_fs_stat(ctx, arguments_json, out_result);
  }
  if (name == "fs_list") {
    return tool_fs_list(ctx, arguments_json, out_result);
  }
  if (name == "fs_find") {
    return tool_fs_find(ctx, arguments_json, out_result);
  }
  if (name == "fs_read") {
    return tool_fs_read(ctx, arguments_json, out_result);
  }
  if (name == "text_search") {
    return tool_text_search(ctx, arguments_json, out_result);
  }
  if (name == "artifact_register") {
    return tool_artifact_register(ctx, arguments_json, out_result);
  }
  if (name == "scene_apply") {
    return tool_scene_apply(ctx, arguments_json, out_result);
  }
  if (name == "ui_action") {
    return tool_ui_action(ctx, arguments_json, out_result);
  }
  if (name == "ui_wait_event") {
    return tool_ui_wait_event(ctx, arguments_json, out_result);
  }
  if (name == "ui_wait_any") {
    return tool_ui_wait_any(ctx, arguments_json, out_result);
  }
  if (name == "ui_wait_all") {
    return tool_ui_wait_all(ctx, arguments_json, out_result);
  }
  if (name == "client_wait_event") {
    return tool_client_wait_event(ctx, arguments_json, out_result);
  }
  if (name == "client_wait_any") {
    return tool_client_wait_any(ctx, arguments_json, out_result);
  }
  if (name == "client_wait_all") {
    return tool_client_wait_all(ctx, arguments_json, out_result);
  }
  if (name == "client_peek") {
    return tool_client_peek(ctx, arguments_json, out_result);
  }
  if (name == "memory_write") {
    return tool_memory_write(ctx, arguments_json, out_result);
  }
  if (name == "memory_observe") {
    return tool_memory_observe(ctx, arguments_json, out_result);
  }
  if (name == "memory_get") {
    return tool_memory_get(ctx, arguments_json, out_result);
  }
  if (name == "memory_search") {
    return tool_memory_search(ctx, arguments_json, out_result);
  }
  if (name == "memory_timeline") {
    return tool_memory_timeline(ctx, arguments_json, out_result);
  }
  if (name == "memory_structured_query") {
    return tool_memory_structured_query(ctx, arguments_json, out_result);
  }
  if (name == "memory_put") {
    return tool_memory_put(ctx, arguments_json, out_result);
  }
  // Keep the response machine-readable so the LLM can reason about failures.
  return set_result(out_result, "{\"ok\":false,\"error\":\"unknown tool\",\"data\":{}}");
}

static agent_status_t add_tool(agent_tool_registry_t* r, const char* name, const char* desc, const char* params_json) {
  return agent_tool_registry_add(r, name, desc, params_json);
}

} // namespace

agent_status_t toolset_host_create(const HostToolsetConfig& cfg, agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor) {
  if (!out_registry || !out_executor) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_registry = nullptr;
  out_executor->ctx = nullptr;
  out_executor->execute = nullptr;

  agent_tool_registry_t* r = nullptr;
  HostToolCtx* ctx = nullptr;
  agent_status_t st = agent_tool_registry_create(&r);
  if (st != AGENT_OK) {
    return st;
  }

  // Process exec and patch (host-only tooling).
  if (cfg.policy == HostToolsetPolicyMode::Full) {
    if (cfg.enable_process_exec) {
      st = add_tool(
        r,
        "shell_exec",
        "Execute /bin/sh -lc <cmd>. Returns JSON envelope: {ok, error?, data:{exit_code, timed_out, output}}. ok is a hint; judge success from output.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"cmd\":{\"type\":\"string\"},"
        "  \"cwd\":{\"type\":\"string\",\"description\":\"Optional working directory for the command.\"},"
        "  \"timeout_ms\":{\"type\":\"integer\"},"
        "  \"max_output_bytes\":{\"type\":\"integer\"}"
        "},"
        "\"required\":[\"cmd\"]"
        "}"
      );
      if (st != AGENT_OK) goto fail;

      st = add_tool(
        r,
        "proc_exec",
        "Execute a process without a shell (posix_spawnp). Returns JSON envelope: {ok, error?, data:{argv, exit_code, timed_out, truncated, output}}.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "  \"cwd\":{\"type\":\"string\",\"description\":\"Optional working directory for the process.\"},"
        "  \"timeout_ms\":{\"type\":\"integer\"},"
        "  \"max_output_bytes\":{\"type\":\"integer\"}"
        "},"
        "\"required\":[\"argv\"]"
        "}"
      );
      if (st != AGENT_OK) goto fail;
    }

    st = add_tool(
      r,
      "file_apply_patch",
      "Apply a unified-diff patch (uses `git apply`). Returns JSON envelope: {ok, error?, data:{patch, check, apply?}}.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"patch\":{\"type\":\"string\"},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"max_output_bytes\":{\"type\":\"integer\"},"
      "  \"unsafe_paths\":{\"type\":\"boolean\",\"description\":\"Only honored in unrestricted mode (root_dir empty).\"}"
      "},"
      "\"required\":[\"patch\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;
  }

  // Read-only filesystem tools (host-only):
  // These exist to reduce token waste by bounding outputs and supporting pagination.
  st = add_tool(
    r,
    "fs_stat",
    "Stat a file or directory (host-side). Returns JSON envelope with metadata and a human-readable `data.output`.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"File/directory path (relative to tools root unless yolo/unrestricted).\"},"
    "  \"count_lines\":{\"type\":\"boolean\",\"description\":\"When true, count lines for small text files (bounded).\"},"
    "  \"max_count_bytes\":{\"type\":\"integer\",\"description\":\"Only count lines when file size <= this many bytes (default: 2097152).\"},"
    "  \"max_count_lines\":{\"type\":\"integer\",\"description\":\"Stop counting after this many lines (default: 200000).\"}"
    "},"
    "\"required\":[\"path\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "fs_list",
    "List a directory (host-side). Prefer this over `ls`/`find` when you need bounded output for token efficiency.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"Directory path (default: .)\"},"
    "  \"recursive\":{\"type\":\"boolean\"},"
    "  \"max_entries\":{\"type\":\"integer\",\"description\":\"Max entries to return (default: 200)\"},"
    "  \"max_depth\":{\"type\":\"integer\",\"description\":\"Max recursion depth when recursive=true (default: 4)\"},"
    "  \"include_hidden\":{\"type\":\"boolean\"},"
    "  \"respect_gitignore\":{\"type\":\"boolean\",\"description\":\"When true, skip paths matched by the repo .gitignore (best-effort).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"},"
    "  \"exclude_globs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional glob patterns (fnmatch) applied to returned entry paths.\"}"
    "}"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "fs_find",
    "Find files/dirs under a path with bounded output (host-side). Prefer this over `find`/`tree` for predictable token usage.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\",\"description\":\"File or directory path (default: .)\"},"
    "  \"recursive\":{\"type\":\"boolean\",\"description\":\"When path is a directory, recurse (default: true).\"},"
    "  \"max_results\":{\"type\":\"integer\",\"description\":\"Max entries to return (default: 200)\"},"
    "  \"max_depth\":{\"type\":\"integer\",\"description\":\"Max recursion depth when recursive=true (default: 6)\"},"
    "  \"include_hidden\":{\"type\":\"boolean\",\"description\":\"Include dotfiles (default: false).\"},"
    "  \"respect_gitignore\":{\"type\":\"boolean\",\"description\":\"When true, skip paths matched by the repo .gitignore (best-effort).\"},"
    "  \"type\":{\"type\":\"string\",\"description\":\"Entry type filter: any|file|dir (default: any).\"},"
    "  \"name_substring\":{\"type\":\"string\",\"description\":\"Optional substring filter on basename.\"},"
    "  \"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional file extension filters (e.g. [\\\".cpp\\\",\\\".h\\\"]).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"},"
    "  \"exclude_globs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional glob patterns (fnmatch) applied to returned entry paths.\"}"
    "}"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "fs_read",
    "Read a text file with bounded output + pagination. Prefer this over `cat`/`sed` when you need predictable token usage.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"path\":{\"type\":\"string\"},"
    "  \"start_line\":{\"type\":\"integer\",\"description\":\"1-based starting line (default: 1)\"},"
    "  \"end_line\":{\"type\":\"integer\",\"description\":\"Optional 1-based end line (inclusive). 0 means unset.\"},"
    "  \"max_lines\":{\"type\":\"integer\",\"description\":\"Max lines to return (default: 200)\"},"
    "  \"max_chars\":{\"type\":\"integer\",\"description\":\"Max characters to return (default: 20000)\"},"
    "  \"with_line_numbers\":{\"type\":\"boolean\"}"
    "},"
    "\"required\":[\"path\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "text_search",
    "Search for a substring in files under a path (token-safe, bounded output). Prefer this over `grep -R` for predictable output size.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"query\":{\"type\":\"string\",\"description\":\"Substring to search for.\"},"
    "  \"path\":{\"type\":\"string\",\"description\":\"File or directory path (default: .)\"},"
    "  \"recursive\":{\"type\":\"boolean\",\"description\":\"When path is a directory, recurse (default: true).\"},"
    "  \"case_sensitive\":{\"type\":\"boolean\",\"description\":\"Case-sensitive search (default: false).\"},"
    "  \"include_hidden\":{\"type\":\"boolean\",\"description\":\"Include dotfiles (default: false).\"},"
    "  \"respect_gitignore\":{\"type\":\"boolean\",\"description\":\"When true, skip paths matched by the repo .gitignore (best-effort).\"},"
    "  \"max_results\":{\"type\":\"integer\",\"description\":\"Max matches to return (default: 200).\"},"
    "  \"max_file_bytes\":{\"type\":\"integer\",\"description\":\"Skip files larger than this many bytes (default: 524288). 0 disables size limit.\"},"
    "  \"max_line_chars\":{\"type\":\"integer\",\"description\":\"Max chars per snippet line (default: 400).\"},"
    "  \"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional file extension filters (e.g. [\\\".cpp\\\",\\\".h\\\"]).\"},"
    "  \"use_default_excludes\":{\"type\":\"boolean\",\"description\":\"When true (default), skips common huge dirs like node_modules/build/dist unless explicitly requested.\"},"
    "  \"exclude_names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extra directory/file basenames to exclude.\"},"
    "  \"exclude_globs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional glob patterns (fnmatch) applied to returned match paths.\"}"
    "},"
    "\"required\":[\"query\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  if (!cfg.sessions_root_dir.empty()) {
    if (cfg.policy == HostToolsetPolicyMode::Full) {
      st = add_tool(
        r,
        "memory_write",
        "Append a durable memory note into the daemon state directory (Markdown; canonical source-of-truth). Layers: core|daily|session. Intended for facts/preferences you want to persist across runs.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"text\":{\"type\":\"string\",\"description\":\"Markdown text to append.\"},"
        "  \"layer\":{\"type\":\"string\",\"description\":\"core|daily|session (default: daily).\"},"
        "  \"path\":{\"type\":\"string\",\"description\":\"Optional relative .md path under the daemon memory directory (overrides layer).\"},"
        "  \"title\":{\"type\":\"string\",\"description\":\"Optional short title for a generated heading.\"},"
        "  \"with_heading\":{\"type\":\"boolean\",\"description\":\"When true (default), prepend a timestamp heading.\"}"
        "},"
        "\"required\":[\"text\"]"
        "}"
      );
      if (st != AGENT_OK) goto fail;

      st = add_tool(
        r,
        "memory_observe",
        "Record a structured observation into durable memory (daily log) with optional tags/citations. Use this for high-signal facts discovered during tool use.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"text\":{\"type\":\"string\",\"description\":\"Observation text (Markdown).\"},"
        "  \"source\":{\"type\":\"string\",\"description\":\"Optional source label (tool, file, url).\"},"
        "  \"trace_id\":{\"type\":\"string\",\"description\":\"Optional trace_id for correlation.\"},"
        "  \"citation\":{\"type\":\"string\",\"description\":\"Optional citation hint (path:line).\"},"
        "  \"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional tag list.\"},"
        "  \"importance\":{\"type\":\"integer\",\"description\":\"Optional importance score (0-5).\"}"
        "},"
        "\"required\":[\"text\"]"
        "}"
      );
      if (st != AGENT_OK) goto fail;
    }

    st = add_tool(
      r,
      "memory_search",
      "Search durable memory Markdown files under the daemon state directory (bounded, explainable: returns path + line + snippet + tier + citation). Prefer default ranked mode when available.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"query\":{\"type\":\"string\"},"
      "  \"max_results\":{\"type\":\"integer\",\"description\":\"Max matches to return (default: 20).\"},"
      "  \"daily_days\":{\"type\":\"integer\",\"description\":\"Scan the last N daily memory files (default: 14). 0 disables daily scan.\"},"
      "  \"case_sensitive\":{\"type\":\"boolean\",\"description\":\"Case-sensitive match (default: false).\"},"
      "  \"use_index\":{\"type\":\"boolean\",\"description\":\"When true (default), use a ranked on-disk index (SQLite FTS5) if available; otherwise fall back to substring scan.\"},"
      "  \"tiered\":{\"type\":\"boolean\",\"description\":\"When true, group results by tier (core/structured/session/daily) and include token estimates.\"},"
      "  \"context_lines\":{\"type\":\"integer\",\"description\":\"Context lines around the match (default: 2).\"},"
      "  \"max_snippet_chars\":{\"type\":\"integer\",\"description\":\"Max chars per snippet (default: 600).\"}"
      "},"
      "\"required\":[\"query\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "memory_timeline",
      "Retrieve bounded context around a memory citation (path:line). Use this after memory_search to inspect the surrounding lines without loading entire files.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"citation\":{\"type\":\"string\",\"description\":\"Citation in the form path:line (preferred).\"},"
      "  \"path\":{\"type\":\"string\",\"description\":\"Relative .md path under memory root (optional if citation provided).\"},"
      "  \"line\":{\"type\":\"integer\",\"description\":\"1-based line number (optional if citation provided).\"},"
      "  \"context_lines\":{\"type\":\"integer\",\"description\":\"Lines of context around the target (default: 3).\"},"
      "  \"max_chars\":{\"type\":\"integer\",\"description\":\"Max chars returned (default: 2000).\"}"
      "},"
      "\"required\":[]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "memory_structured_query",
      "Query structured memory records (facts/preferences/tasks) from a structured memory Markdown file (default: STRUCTURED.md). Prefer this over substring search when you know stable keys/prefixes.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"path\":{\"type\":\"string\",\"description\":\"Relative .md path under the daemon memory directory (default: STRUCTURED.md).\"},"
      "  \"key\":{\"type\":\"string\",\"description\":\"Exact key to fetch (optional).\"},"
      "  \"key_prefix\":{\"type\":\"string\",\"description\":\"Key prefix filter (optional).\"},"
      "  \"key_case_insensitive\":{\"type\":\"boolean\",\"description\":\"When true, key/prefix matching is case-insensitive (default: false).\"},"
      "  \"source_contains\":{\"type\":\"string\",\"description\":\"Optional substring filter applied to record sources[] (e.g. trace/workflow/session correlation).\"},"
      "  \"source_case_insensitive\":{\"type\":\"boolean\",\"description\":\"When true, source_contains matching is case-insensitive (default: false).\"},"
      "  \"updated_since_utc\":{\"type\":\"string\",\"description\":\"Optional lower bound on record updated_utc (ISO UTC like 2026-02-05T00:00:00Z).\"},"
      "  \"updated_until_utc\":{\"type\":\"string\",\"description\":\"Optional upper bound on record updated_utc (ISO UTC like 2026-02-05T23:59:59Z).\"},"
      "  \"order_by\":{\"type\":\"string\",\"description\":\"Sort order: key_asc (default) or updated_desc.\"},"
      "  \"kinds\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional kinds filter: fact|preference|task.\"},"
      "  \"status\":{\"type\":\"string\",\"description\":\"Status filter (default: active). Use any to include deprecated.\"},"
      "  \"include_sources\":{\"type\":\"boolean\",\"description\":\"Include sources[] in results (default: true).\"},"
      "  \"include_versions\":{\"type\":\"boolean\",\"description\":\"Include versions[] history in results (default: false).\"},"
      "  \"limit\":{\"type\":\"integer\",\"description\":\"Max matched records (default: 50; max: 200).\"}"
      "}"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "memory_get",
      "Read a durable memory Markdown file under the daemon state directory (bounded/paginated).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"path\":{\"type\":\"string\",\"description\":\"Relative .md path under the daemon memory directory (e.g. MEMORY.md or 2026-02-01.md).\"},"
      "  \"from_line\":{\"type\":\"integer\",\"description\":\"1-based starting line (default: 1).\"},"
      "  \"max_lines\":{\"type\":\"integer\",\"description\":\"Max lines to return (default: 200).\"}"
      "},"
      "\"required\":[\"path\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    if (cfg.policy == HostToolsetPolicyMode::Full) {
      st = add_tool(
        r,
        "memory_put",
        "Overwrite a durable memory Markdown file under the daemon state directory (used for consolidation: deprecate outdated facts, keep core memory accurate). Supports legacy full-text overwrite or structured upserts.",
        "{"
        "\"type\":\"object\","
        "\"properties\":{"
        "  \"path\":{\"type\":\"string\",\"description\":\"Relative .md path under the daemon memory directory.\"},"
        "  \"text\":{\"type\":\"string\",\"description\":\"Legacy mode: full file contents to write.\"},"
        "  \"entries\":{\"type\":\"array\",\"description\":\"Structured mode: upsert durable facts/preferences/tasks.\",\"items\":{"
        "    \"type\":\"object\","
        "    \"properties\":{"
        "      \"key\":{\"type\":\"string\"},"
        "      \"kind\":{\"type\":\"string\",\"description\":\"fact|preference|task (default: fact).\"},"
        "      \"value\":{\"type\":\"string\"},"
        "      \"status\":{\"type\":\"string\",\"description\":\"active|deprecated (default: active).\"},"
        "      \"source\":{\"type\":\"string\",\"description\":\"Optional provenance/source string.\"}"
        "    },"
        "    \"required\":[\"key\",\"value\"]"
        "  }},"
        "  \"checkpoint\":{\"type\":\"boolean\",\"description\":\"When true (default), write a time-stamped JSON snapshot under memory/checkpoints/ for structured updates.\"},"
        "  \"keep_checkpoints\":{\"type\":\"integer\",\"description\":\"How many structured checkpoints to retain (default: 100).\"}"
        "},"
        "\"required\":[\"path\"],"
        "\"anyOf\":[{\"required\":[\"text\"]},{\"required\":[\"entries\"]}]"
        "}"
      );
      if (st != AGENT_OK) goto fail;
    }
  }

  st = add_tool(
    r,
    "artifact_register",
    "Register a host file (image/audio/video/etc) as an artifact for the UI to render. Returns JSON envelope with data.artifact metadata and playback hints.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"description\":\"image|audio|video|text|file\"},\"mime\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"autoplay\":{\"type\":\"boolean\"},\"repeat\":{\"type\":\"integer\"}},\"required\":[\"path\"]}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "scene_apply",
    "Update the server-owned Scene (durable, refresh-proof). Accepts an ops array (create/update/delete/clear/action). Returns JSON envelope with data.ops; the daemon persists the resulting scene state per session.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"ops\":{"
    "    \"type\":\"array\","
    "    \"description\":\"Array of Scene operations (create/update/delete/clear/action).\","
    "    \"items\":{\"type\":\"object\"}"
    "  }"
    "},"
    "\"required\":[\"ops\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  st = add_tool(
    r,
    "ui_action",
    "Request a UI action (no host side effects). Returns JSON envelope with data.action; the tool loop emits a derived ui_action event to the UI.",
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "  \"type\":{\"type\":\"string\",\"description\":\"Action type (e.g. notify, client_rpc).\"},"
    "  \"title\":{\"type\":\"string\"},"
    "  \"message\":{\"type\":\"string\"},"
    "  \"path\":{\"type\":\"string\",\"description\":\"Optional host path for presentation helpers (prefer artifact_register for files).\"},"
    "  \"mime\":{\"type\":\"string\"},"
    "  \"repeat\":{\"type\":\"integer\"},"
    "  \"autoplay\":{\"type\":\"boolean\"},"
    "  \"rpc_id\":{\"type\":\"string\",\"description\":\"Correlation id for client_rpc/collab_rpc.\"},"
    "  \"rpc\":{\"type\":\"object\",\"description\":\"Client RPC request payload (kind/args/etc).\"},"
    "  \"side_effects\":{\"type\":\"boolean\",\"description\":\"Advisory: indicates the action expects client-side side effects.\"},"
    "  \"auto_run\":{\"type\":\"boolean\",\"description\":\"Request client auto-run when permitted.\"},"
    "  \"auto\":{\"type\":\"boolean\",\"description\":\"Alias for auto_run.\"},"
    "  \"probe_id\":{\"type\":\"string\",\"description\":\"Legacy correlation id for client_probe.\"},"
    "  \"probe\":{\"type\":\"object\",\"description\":\"Legacy probe request payload.\"},"
    "  \"query_id\":{\"type\":\"string\",\"description\":\"Correlation id for request_client_state.\"}"
    "},"
    "\"required\":[\"type\"]"
    "}"
  );
  if (st != AGENT_OK) goto fail;

  if (!cfg.session_id.empty() && (!cfg.sessions_root_dir.empty() || cfg.read_client_events_tail)) {
    st = add_tool(
      r,
      "ui_wait_event",
      "Wait for a client event (posted via agentd /api/v1/session/client_event) for this session. Useful for acknowledgements like artifact_rendered or client_rpc_result. (Deprecated name; prefer client_wait_event.)",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"type\":{\"type\":\"string\",\"description\":\"Client event type to wait for (e.g. artifact_rendered, client_rpc_result).\"},"
      "  \"client_id\":{\"type\":\"string\",\"description\":\"Optional filter for payload.client.id (e.g. webui, slack).\"},"
      "  \"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max wait time (default: 30000). 0 means no-wait (immediate timeout).\"},"
      "  \"after_unix_ms\":{\"type\":\"integer\",\"description\":\"Ignore events older than this timestamp (optional).\"},"
      "  \"path\":{\"type\":\"string\",\"description\":\"Optional filter for payload.data.path.\"},"
      "  \"data_match\":{\"type\":\"object\",\"description\":\"Optional partial-match filter applied to payload.data (nested objects supported; arrays match exactly).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes read from the end of the client event log (default: 262144).\"}"
      "},"
      "\"required\":[\"type\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "ui_wait_any",
      "Wait until any of multiple client event predicates matches (OR join). (Deprecated name; prefer client_wait_any.)",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"description\":\"List of event predicates to match.\",\"items\":{"
        "    \"type\":\"object\","
        "    \"properties\":{"
          "      \"type\":{\"type\":\"string\"},"
          "      \"client_id\":{\"type\":\"string\"},"
          "      \"after_unix_ms\":{\"type\":\"integer\"},"
          "      \"path\":{\"type\":\"string\"},"
          "      \"data_match\":{\"type\":\"object\"}"
        "    },"
        "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max wait time (default: 30000). 0 means no-wait (immediate timeout).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes read from the end of the client event log (default: 262144).\"},"
      "  \"max_files\":{\"type\":\"integer\",\"description\":\"Max rotated log files to consider (default: daemon/session store default).\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "ui_wait_all",
      "Wait until all of multiple client event predicates match (AND join). (Deprecated name; prefer client_wait_all.)",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"description\":\"List of event predicates to match.\",\"items\":{"
        "    \"type\":\"object\","
        "    \"properties\":{"
          "      \"type\":{\"type\":\"string\"},"
          "      \"client_id\":{\"type\":\"string\"},"
          "      \"after_unix_ms\":{\"type\":\"integer\"},"
          "      \"path\":{\"type\":\"string\"},"
          "      \"data_match\":{\"type\":\"object\"}"
        "    },"
        "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max wait time (default: 30000). 0 means no-wait (immediate timeout).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes read from the end of the client event log (default: 262144).\"},"
      "  \"max_files\":{\"type\":\"integer\",\"description\":\"Max rotated log files to consider (default: daemon/session store default).\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_wait_event",
      "Wait for a client event (posted via agentd /api/v1/session/client_event) for this session. This is the preferred name (client-agnostic).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"type\":{\"type\":\"string\"},"
      "  \"client_id\":{\"type\":\"string\"},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"after_unix_ms\":{\"type\":\"integer\"},"
      "  \"path\":{\"type\":\"string\"},"
      "  \"data_match\":{\"type\":\"object\"},"
      "  \"max_bytes\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"type\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_wait_any",
      "Wait until any predicate matches (OR join). Preferred name (client-agnostic).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"items\":{"
      "    \"type\":\"object\","
      "    \"properties\":{"
      "      \"type\":{\"type\":\"string\"},"
      "      \"client_id\":{\"type\":\"string\"},"
      "      \"after_unix_ms\":{\"type\":\"integer\"},"
      "      \"path\":{\"type\":\"string\"},"
      "      \"data_match\":{\"type\":\"object\"}"
      "    },"
      "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"max_bytes\":{\"type\":\"integer\"},"
      "  \"max_files\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_wait_all",
      "Wait until all predicates match (AND join). Preferred name (client-agnostic).",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"predicates\":{\"type\":\"array\",\"items\":{"
      "    \"type\":\"object\","
      "    \"properties\":{"
      "      \"type\":{\"type\":\"string\"},"
      "      \"client_id\":{\"type\":\"string\"},"
      "      \"after_unix_ms\":{\"type\":\"integer\"},"
      "      \"path\":{\"type\":\"string\"},"
      "      \"data_match\":{\"type\":\"object\"}"
      "    },"
      "    \"required\":[\"type\"]"
      "  }},"
      "  \"timeout_ms\":{\"type\":\"integer\"},"
      "  \"max_bytes\":{\"type\":\"integer\"},"
      "  \"max_files\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"predicates\"]"
      "}"
    );
    if (st != AGENT_OK) goto fail;

    st = add_tool(
      r,
      "client_peek",
      "Probe recent client event state for this session (non-blocking). Useful for reasoning about client environment/state without waiting.",
      "{"
      "\"type\":\"object\","
      "\"properties\":{"
      "  \"client_id\":{\"type\":\"string\",\"description\":\"Optional filter for client.id.\"},"
      "  \"event_type\":{\"type\":\"string\",\"description\":\"Optional filter for payload.type (e.g. client_state).\"},"
      "  \"max_bytes\":{\"type\":\"integer\",\"description\":\"Bytes to scan from the tail (default: 262144).\"},"
      "  \"max_files\":{\"type\":\"integer\",\"description\":\"Max rotated log files to consider.\"},"
      "  \"include_data\":{\"type\":\"boolean\",\"description\":\"When true, include a bounded view of the last event payload.\"},"
      "  \"max_data_bytes\":{\"type\":\"integer\",\"description\":\"Max bytes of last_event.data before it is summarized/truncated.\"}"
      "}"
      "}"
    );
    if (st != AGENT_OK) goto fail;
  }

  // Executor context (owned by host; for CLI we just heap-allocate).
  ctx = new (std::nothrow) HostToolCtx();
  if (!ctx) {
    st = AGENT_ERR_OOM;
    goto fail;
  }
  // Design requirement (agentd): tools have no path constraints (even in scoped mode).
  // We detect daemon usage via a non-empty session_id.
  ctx->unrestricted = cfg.root_dir.empty() || !cfg.session_id.empty();
  ctx->root = cfg.root_dir.empty() ? std::filesystem::current_path()
                                   : std::filesystem::path(cfg.root_dir);
  ctx->policy = cfg.policy;
  ctx->exec_enabled = (cfg.policy == HostToolsetPolicyMode::Full) && cfg.enable_process_exec;
  ctx->allow_symlinks = cfg.allow_symlinks;
  ctx->should_cancel = cfg.should_cancel;
  ctx->should_cancel_ctx = cfg.should_cancel_ctx;
  if (!cfg.sessions_root_dir.empty()) {
    ctx->sessions_root_dir = std::filesystem::path(cfg.sessions_root_dir);
  }
  ctx->session_id = cfg.session_id;
  ctx->read_client_events_tail_cb = cfg.read_client_events_tail;
  ctx->read_client_events_tail_ctx = cfg.read_client_events_tail_ctx;
  {
    // Normalize to reduce symlink/canonical mismatch (e.g. /var vs /private/var on macOS).
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(ctx->root, ec);
    if (!ec) {
      ctx->root = canon;
    } else {
      ctx->root = ctx->root.lexically_normal();
    }
  }
  if (!ctx->session_id.empty()) {
    // Best-effort per-session working directory. Artifacts are expected to live under:
    //   <sessions_root_dir>/session_<session_id>/{work,out}/...
    std::error_code ec;
    const auto sr = session_root_dir(ctx);
    const auto wd = session_work_dir(ctx);
    const auto outd = session_out_dir(ctx);
    if (!sr.empty()) (void)std::filesystem::create_directories(sr, ec);
    ec.clear();
    if (!wd.empty()) (void)std::filesystem::create_directories(wd, ec);
    ec.clear();
    if (!outd.empty()) (void)std::filesystem::create_directories(outd, ec);
  }

  out_executor->ctx = ctx;
  out_executor->execute = host_tools_execute;
  *out_registry = r;
  return AGENT_OK;

fail:
  if (ctx) {
    delete ctx;
  }
  agent_tool_registry_destroy(r);
  return st;
}

void toolset_host_destroy(agent_tool_executor_t* executor) {
  if (!executor) {
    return;
  }
  if (executor->ctx) {
    HostToolCtx* ctx = (HostToolCtx*)executor->ctx;
    delete ctx;
  }
  executor->ctx = nullptr;
  executor->execute = nullptr;
}
