#include "toolset_host_internal.h"
#include "work_ledger.h"

#include <memory>
#include <cstring>

namespace host_tools_internal {
agent_status_t tool_work_state(HostToolCtx* ctx, const char* arguments_json, agent_string_t* out_result) {
  if (!ctx || !arguments_json || !out_result) return AGENT_ERR_INVALID_ARGUMENT;
  Json::Value request, response(Json::objectValue);
  std::string error;
  const auto len = std::strlen(arguments_json);
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  bool ok = len <= 64 * 1024 && reader->parse(arguments_json, arguments_json + len, &request, &error) && request.isObject();
  if (!ok) error = "expected work request object of at most 64 KiB";
  if (ok && request.get("op", "read") != "read" &&
      (ctx->policy == HostToolsetPolicyMode::ReadOnly || !ctx->memory_write_allowed)) {
    ok = false; error = "work mutation disabled by policy";
  }
  if (ok) {
    const auto session = session_root_dir(ctx);
    const auto root = session.empty() ? ctx->root / ".agent" : session / "work";
    ok = work_ledger_apply(root / "work.sqlite", request, &response, &error);
  }
  if (!ok) { response = Json::Value(Json::objectValue); response["ok"] = false; response["error"] = error; }
  Json::StreamWriterBuilder writer; writer["indentation"] = "";
  return set_result(out_result, Json::writeString(writer, response));
}
}  // namespace host_tools_internal
