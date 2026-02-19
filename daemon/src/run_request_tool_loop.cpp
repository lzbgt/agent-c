#include "run_request_tool_loop.h"

#include "daemon_config.h"
#include "job_manager.h"
#include "openai_client.h"
#include "openai_provider.h"
#include "provider_util.h"
#include "run_endpoints_internal.h"
#include "run_multimodal.h"

#include "agent/agent.h"
#include "agent/multimodal_prefix.h"

#include <json/json.h>

#include <algorithm>
#include <memory>
#include <sstream>

namespace agentd {
namespace {

void inject_trace_id_into_events(Json::Value* arr, const std::string& trace_id) {
  if (!arr || !arr->isArray() || trace_id.empty()) return;
  for (Json::ArrayIndex i = 0; i < arr->size(); i++) {
    Json::Value& ev = (*arr)[i];
    if (!ev.isObject()) continue;
    if (!ev.isMember("trace_id")) ev["trace_id"] = trace_id;
  }
}

void inject_schema_into_events(Json::Value* arr) {
  if (!arr || !arr->isArray()) return;
  for (Json::ArrayIndex i = 0; i < arr->size(); i++) {
    Json::Value& ev = (*arr)[i];
    if (!ev.isObject()) continue;
    if (ev.isMember("schema")) continue;
    if (!ev.isMember("type") || !ev["type"].isString()) continue;
    const std::string type = ev["type"].asString();
    const char* schema = nullptr;
    if (type == "assistant_delta") schema = "run_event_payload_assistant_delta_v1";
    else if (type == "assistant_message") schema = "run_event_payload_assistant_message_v1";
    else if (type == "user_message") schema = "run_event_payload_user_message_v1";
    else if (type == "tool_call") schema = "run_event_payload_tool_call_v1";
    else if (type == "tool_result") schema = "run_event_payload_tool_result_v1";
    else if (type == "llm_usage") schema = "run_event_payload_llm_usage_v1";
    else if (type == "artifact") schema = "run_event_payload_artifact_v1";
    else if (type == "ui_action") schema = "run_event_payload_ui_action_v1";
    else if (type == "heartbeat") schema = "run_event_payload_heartbeat_v1";
    else if (type == "error") schema = "run_event_payload_error_v1";
    if (schema) ev["schema"] = schema;
  }
}

}  // namespace

RunRequestToolLoopResult run_request_tool_loop(const RunRequestToolLoopInput& in) {
  RunRequestToolLoopResult out;
  if (!in.daemon_cfg || !in.args || !in.run_cfg || !in.prompt || !in.prompt_for_llm || !in.trace_id ||
      !in.session_id || !in.tools || !in.mem_pol || !in.mem_query || !in.session || !in.registry || !in.executor ||
      !in.job_id || !in.heartbeat_last_any_event_ms || !in.heartbeat_last_non_ms || !in.heartbeat_phase) {
    out.ok = false;
    out.err = "tool loop input missing required fields";
    return out;
  }

  Json::Value pre_events(Json::arrayValue);
  std::string prompt_for_tool_loop = *in.prompt_for_llm;
  {
    const bool want_prefetch =
      !(in.args->isMember("vision_prefetch") && (*in.args)["vision_prefetch"].isBool() &&
        (*in.args)["vision_prefetch"].asBool() == false);

    Json::Value mm(Json::nullValue);
    std::string user_text = *in.prompt_for_llm;
    const bool has_mm = try_parse_multimodal_prefix(*in.prompt_for_llm, &mm, &user_text) && mm.isObject();
    const bool has_images = has_mm && mm.isMember("images") && mm["images"].isArray() && !mm["images"].empty();
    const bool should_prefetch =
      want_prefetch && has_images && provider_requires_tools_none_for_vision(in.run_cfg->base_url, in.run_cfg->model);

    if (should_prefetch) {
      out.vision_prefetch_attempted = true;
      std::string vision_desc;
      std::string v_err;
      long v_http = 0;
      try {
        const std::string pre_text =
          std::string("Describe the attached image(s) in detail so I can answer the user's request.\n")
          + "User request:\n"
          + *in.prompt;

        Json::Value root(Json::objectValue);
        root["model"] = in.run_cfg->model;
        root["stream"] = false;
        Json::Value messages(Json::arrayValue);

        {
          Json::Value sm(Json::objectValue);
          sm["role"] = "system";
          sm["content"] = "You are a vision captioning assistant. Output plain text.";
          messages.append(sm);
        }
        {
          Json::Value um(Json::objectValue);
          um["role"] = "user";
          um["content"] = multimodal_content_from_parts(pre_text, mm, /*allow_image_parts=*/true);
          messages.append(um);
        }

        root["messages"] = messages;
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        const std::string req_json = Json::writeString(wb, root);

        OpenAIRawResult raw = openai_chat_completions_raw(*in.run_cfg, req_json);
        v_http = raw.http_status;
        if (raw.http_status < 200 || raw.http_status >= 300) {
          v_err = openai_format_http_error(raw.http_status, raw.response_body);
        } else {
          vision_desc = try_extract_assistant_text_from_response_json(raw.response_body);
          if (vision_desc.empty()) {
            v_err = "vision prefetch returned empty assistant text";
          }
        }
      } catch (const std::exception& e) {
        v_err = std::string("vision prefetch threw exception: ") + e.what();
      } catch (...) {
        v_err = "vision prefetch threw unknown exception";
      }

      Json::Value ev(Json::objectValue);
      ev["type"] = "vision_prefetch";
      if (!in.trace_id->empty()) ev["trace_id"] = *in.trace_id;
      Json::Value d(Json::objectValue);
      d["ok"] = (bool)v_err.empty();
      d["provider"] = provider_from_base_url(in.run_cfg->base_url);
      d["model"] = in.run_cfg->model;
      if (v_http) d["http_status"] = (Json::Int64)v_http;
      if (!v_err.empty()) d["error"] = v_err;
      if (!vision_desc.empty()) {
        d["chars"] = (Json::UInt64)vision_desc.size();
        const size_t kPreview = 512;
        d["preview"] = vision_desc.size() <= kPreview ? vision_desc : (vision_desc.substr(0, kPreview) + "…");
      }
      ev["data"] = d;
      pre_events.append(ev);

      if (v_err.empty() && !vision_desc.empty()) {
        out.vision_prefetch_ok = true;
        Json::Value mm2 = mm;
        if (mm2.isObject() && mm2.isMember("images")) {
          mm2.removeMember("images");
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        prompt_for_tool_loop = std::string(kMultimodalPrefix) + Json::writeString(wb, mm2) + "\n" + user_text;
        prompt_for_tool_loop += "\n\n[Image description]\n";
        prompt_for_tool_loop += vision_desc;
      }
    }
  }

  ToolLoopOptions opt;
  opt.max_steps = in.max_steps;
  opt.max_tool_calls_total = in.max_tool_calls_total;
  opt.max_tool_calls_per_tool = in.max_tool_calls_per_tool;
  opt.max_tool_call_args_chars = in.max_tool_call_args_chars;
  opt.max_tool_result_chars = in.max_tool_result_chars;
  if (in.tool_call_limits) {
    opt.tool_call_limits = *in.tool_call_limits;
  }
  opt.verbose = in.verbose;
  opt.stream_assistant = in.stream_assistant;
  if (in.args->isMember("max_repeated_tool_calls") && (*in.args)["max_repeated_tool_calls"].isInt()) {
    const int v = (*in.args)["max_repeated_tool_calls"].asInt();
    if (v >= 0) opt.max_repeated_tool_calls = (size_t)v;
  }
  opt.max_capture_bytes =
    in.max_capture_bytes == 0 ? (size_t)64 * 1024 : std::min<size_t>(in.max_capture_bytes, (size_t)1024 * 1024);
  opt.max_chars = in.max_chars;
  opt.keep_last_messages = in.keep_last;
  if (in.args->isMember("force_tool") && (*in.args)["force_tool"].isString()) opt.force_tool = (*in.args)["force_tool"].asString();
  opt.require_tool_call =
    in.args->isMember("require_tool_call") && (*in.args)["require_tool_call"].isBool() ? (*in.args)["require_tool_call"].asBool() : false;

  DaemonJobEventHookCtx hook;
  if (!in.job_id->empty()) {
    hook.job_id = *in.job_id;
    hook.last_any_event_ms = in.heartbeat_last_any_event_ms;
    hook.last_non_heartbeat_ms = in.heartbeat_last_non_ms;
    hook.phase = in.heartbeat_phase;
    opt.on_event = daemon_job_on_tool_loop_event;
    opt.on_event_ctx = &hook;
  }
  if (in.should_cancel_or_null) {
    opt.should_cancel = in.should_cancel_or_null;
    opt.should_cancel_ctx = in.should_cancel_ctx_or_null;
  } else if (!in.job_id->empty()) {
    opt.should_cancel = [](void* vctx) -> bool {
      if (!vctx) return false;
      const auto* jid = static_cast<const std::string*>(vctx);
      return jid && job_is_cancel_requested(*jid);
    };
    opt.should_cancel_ctx = (void*)in.job_id;
  }

  struct SessionDel {
    void operator()(agent_session_t* s) const {
      if (s) agent_session_destroy(s);
    }
  };
  std::unique_ptr<agent_session_t, SessionDel> ephemeral_seed;
  const agent_session_t* seed_for_run = in.session;
  if (!in.no_default_system && *in.tools == "host" && !in.no_session) {
    std::string mem_ctx;
    std::string effective_query = *in.mem_query;
    if (effective_query.empty() && in.mem_pol->mode == MemoryContextMode::Search) {
      effective_query = *in.prompt_for_llm;
    }
    if (build_memory_context_text(in.daemon_cfg->state_dir, *in.session_id, *in.mem_pol, effective_query, &mem_ctx)) {
      if (agent_session_t* tmp = clone_session_with_memory_context(in.session, mem_ctx)) {
        ephemeral_seed.reset(tmp);
        seed_for_run = tmp;
      }
    }
  }

  try {
    out.ok = run_tool_loop(
      *in.run_cfg,
      seed_for_run,
      prompt_for_tool_loop,
      in.registry,
      in.executor,
      opt,
      in.trace_stream,
      &out.tool_loop_result,
      &out.err,
      &out.http_status,
      &out.http_body
    );
  } catch (const std::exception& e) {
    out.ok = false;
    out.err = std::string("tool loop threw exception: ") + e.what();
  } catch (...) {
    out.ok = false;
    out.err = "tool loop threw unknown exception";
  }
  out.assistant_text = out.tool_loop_result.final_assistant_text;
  if (!out.tool_loop_result.events_json.empty()) {
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream iss(out.tool_loop_result.events_json);
    Json::Value ev;
    if (Json::parseFromStream(rb, iss, &ev, &errs) && ev.isArray()) {
      out.events_out = ev;
      inject_trace_id_into_events(&out.events_out, *in.trace_id);
      inject_schema_into_events(&out.events_out);
    }
  }
  if (!pre_events.empty()) {
    if (!out.events_out.isArray()) out.events_out = Json::Value(Json::arrayValue);
    for (const auto& pe : pre_events) {
      out.events_out.append(pe);
    }
    inject_trace_id_into_events(&out.events_out, *in.trace_id);
    inject_schema_into_events(&out.events_out);
  }

  if (out.ok) {
    agent_session_add_message(in.session, AGENT_ROLE_USER, in.prompt->c_str());
    agent_session_add_message(in.session, AGENT_ROLE_ASSISTANT, out.assistant_text.c_str());
  }

  return out;
}

}  // namespace agentd
