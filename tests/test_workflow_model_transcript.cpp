#include "workflow_model_effect.h"
#include "tool_loop.h"
#include "openai_tool_provider.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>

namespace {
void check(bool value, const std::string& why) {
  if (!value) { std::cerr << why << '\n'; std::exit(1); }
}
Json::Value result(bool ok) { Json::Value value; value["ok"] = ok; value["error"] = ok ? "" : "provider unavailable"; return value; }
struct Fake {
  int providers = 0, tools = 0, approvals = 0;
  agent_status_t status = AGENT_OK;
  bool tool_response = true, malformed = false;
  std::string tool_result = R"({"ok":true,"data":{"output":"receipted"}})";
  agent_tool_provider_t provider() {
    return {this, [](void* ctx, const agent_tool_provider_request_t*, agent_tool_provider_response_t* out) {
      auto& f = *static_cast<Fake*>(ctx); ++f.providers; *out = {};
      if (f.status != AGENT_OK) {
        const std::string error = "synthetic failure";
        agent_string_set_copy(&out->error_message, error.data(), error.size()); return f.status;
      }
      agent_string_set_copy(&out->assistant_content, "original", 8);
      if (f.tool_response) {
        out->tool_calls = static_cast<agent_tool_call_t*>(agent_malloc(sizeof(agent_tool_call_t)));
        check(out->tool_calls != nullptr, "allocation"); *out->tool_calls = {}; out->tool_call_count = 1;
        agent_string_set_copy(&out->tool_calls[0].id, "original-call", 13);
        agent_string_set_copy(&out->tool_calls[0].name, "proc_exec", 9);
        agent_string_set_copy(&out->tool_calls[0].arguments_json, "{}", 2);
        if (f.malformed) { agent_string_free(&out->tool_calls[0].arguments_json); out->tool_calls[0].arguments_json.len = 2; }
      }
      return AGENT_OK;
    }};
  }
  agent_tool_executor_t executor() {
    return {this, [](void* ctx, const char*, const char*, agent_string_t* out) {
      auto& f = *static_cast<Fake*>(ctx); ++f.tools;
      return agent_string_set_copy(out, f.tool_result.data(), f.tool_result.size());
    }};
  }
};
agent_status_t generate(agent_tool_provider_t provider, size_t step = 0, const char* model = "stub") {
  agent_tool_provider_request_t request{}; request.model = model; request.step = step;
  agent_tool_provider_response_t out{};
  const auto status = provider.generate(provider.ctx, &request, &out);
  if (status == AGENT_OK && out.tool_call_count) check(std::string(out.tool_calls[0].id.data, out.tool_calls[0].id.len) == "original-call", "call identity lost");
  agent_tool_provider_response_free(&out); return status;
}
agent_status_t execute(agent_tool_executor_t executor, const char* args = "{}") {
  agent_string_t out{}; const auto status = executor.execute(executor.ctx, "proc_exec", args, &out);
  if (status == AGENT_OK) check(std::string(out.data, out.len).find("receipted") != std::string::npos, "receipt changed");
  agent_string_free(&out); return status;
}
void configure(agentd::WorkflowModelEffect& guard, ToolLoopOptions& options, const char* model = "stub") {
  Json::Value scope; scope["cwd"] = "/synthetic";
  guard.bind_scope(scope);
  Json::Value identity; identity["model"] = model;
  guard.configure_transcript(&options, identity);
}
}
int main() {
  using namespace agentd;
  const auto root = std::filesystem::temp_directory_path() /
    ("model-transcript-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.sqlite").string();
  AgentDb db; std::string error; check(db.open(path, &error), error);
  AgentDb::WorkflowRow wf; wf.workflow_id = "wf"; wf.status = "queued"; wf.spec_json = "{}";
  wf.created_unix_ms = wf.updated_unix_ms = 1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const char* id : {"resume", "crash", "pending", "config", "request", "tool", "corrupt", "budget", "context",
                         "cas", "stop", "malformed", "limit", "unconfigured", "receipt_fail", "terminal_limit", "fresh_policy", "zero_steps", "reasoning", "reasoning_large", "vision_complete", "vision_shape", "vision_resume", "vision_request", "vision_fallback", "vision_budget",
                         "vision_large", "vision_input", "vision_invalid", "vision_session", "vision_stop", "vision_order"}) {
    AgentDb::WorkflowTaskRow task; task.workflow_id = "wf"; task.task_id = id; task.status = "queued";
    task.request_json = "{}"; task.depends_on_json = "[]";
    task.created_unix_ms = task.updated_unix_ms = 1; tasks.push_back(task);
  }
  check(db.create_workflow(wf, tasks, &error), error);
  const std::string task_request = R"({"prompt":"work","tools":"host","no_session":true})";
  auto ctx = [&](const char* id) { return WorkflowEffectContext{&db, "wf", id}; };
  auto sql = [&](const std::string& text) {
    sqlite3* raw = nullptr; check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "fixture open");
    check(sqlite3_exec(raw, text.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(raw)); sqlite3_close(raw);
  };
  auto checkpoint = [&](const char* id, Fake& fake) {
    WorkflowModelEffect guard(ctx(id), task_request); check(guard.preflight().isNull(), "checkpoint preflight");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2; configure(guard, options);
    auto provider = guard.wrap_provider(fake.provider()); auto executor = guard.wrap(fake.executor());
    check(generate(provider) == AGENT_OK && execute(executor) == AGENT_OK, "checkpoint prefix");
    fake.status = AGENT_ERR_INTERNAL;
    check(generate(provider, 1) == AGENT_ERR_INTERNAL, "provider failure");
    const auto out = guard.finish(result(false));
    check(out["retryable"] == true && out["effect"]["resume_available"] == true &&
      out["steps_executed"] == 2 && out["tool_calls_total"] == 1 && out["needs_input"] != true, out.toStyledString());
    fake.status = AGENT_OK; return out["effect"]["effect_id"].asString();
  };
  Fake resumed;
  const auto effect_id = checkpoint("resume", resumed);
  db.close(); check(db.open(path, &error), error);
  {
    WorkflowModelEffect guard(ctx("resume"), task_request); check(guard.preflight().isNull() && guard.has_checkpoint(), "restore prefix");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2;
    guard.set_fresh_budget(true, 0, true, 1); configure(guard, options);
    check(options.max_steps == 2 && options.max_tool_calls_total == 2, "core replay traversal limits");
    auto executor = guard.wrap(resumed.executor(), [&](const char*, const char*) { ++resumed.approvals; return AGENT_ERR_INTERNAL; });
    auto provider = options.provider_decorator(options.provider_decorator_ctx, resumed.provider(), nullptr);
    check(generate(provider) == AGENT_OK && execute(executor) == AGENT_OK, "cache replay asked fresh permission or grant");
    resumed.tool_response = false;
    check(generate(provider, 1) == AGENT_OK, "fresh provider frontier");
    const auto out = guard.finish(result(true));
    check(out["ok"] == true && out["effect"]["state"] == "completed" &&
      out["steps_executed"] == 1 && out["tool_calls_total"] == 0 &&
      out["effect"]["replayed_provider_calls"].asUInt64() == 1 && out["effect"]["replayed_tool_calls"].asUInt64() == 1 &&
      resumed.tools == 1 && resumed.providers == 3 && resumed.approvals == 0, out.toStyledString());
    WorkflowModelEffect done(ctx("resume"), task_request);
    check(done.preflight()["effect"]["replayed"] == true, "completed checkpoint receipt");
  }
  // Death after an acknowledged tool is resumable, unlike an admitted call
  // whose executor never returned a bounded terminal receipt.
  {
    Fake fake; WorkflowModelEffect guard(ctx("crash"), task_request);
    check(guard.preflight().isNull(), "crash preflight"); ToolLoopOptions options; configure(guard, options);
    check(generate(guard.wrap_provider(fake.provider())) == AGENT_OK && execute(guard.wrap(fake.executor())) == AGENT_OK, "crash prefix");
    WorkflowModelEffect restored(ctx("crash"), task_request);
    check(restored.preflight().isNull() && restored.has_checkpoint(), "crash lost durable tool receipt");
  }
  {
    Fake fake; WorkflowModelEffect guard(ctx("pending"), task_request); check(guard.preflight().isNull(), "pending preflight");
    ToolLoopOptions options; configure(guard, options); check(generate(guard.wrap_provider(fake.provider())) == AGENT_OK, "pending provider");
    fake.tool_result = "unbounded"; check(execute(guard.wrap(fake.executor())) != AGENT_OK, "missing receipt accepted");
    WorkflowModelEffect restored(ctx("pending"), task_request);
    check(restored.preflight()["needs_input"] == true, "pending host call was replayable");
  }
  for (const char* id : {"config", "request", "tool", "unconfigured"}) {
    Fake fake; checkpoint(id, fake); const auto providers = fake.providers;
    WorkflowModelEffect guard(ctx(id), task_request); check(guard.preflight().isNull(), "mismatch preflight");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2;
    if (std::string(id) != "unconfigured") configure(guard, options, std::string(id) == "config" ? "changed" : "stub");
    auto provider = guard.wrap_provider(fake.provider()); auto executor = guard.wrap(fake.executor());
    if (std::string(id) == "config") check(generate(provider) != AGENT_OK, "changed context provider call");
    if (std::string(id) == "request") check(generate(provider, 0, "changed") != AGENT_OK, "changed request provider call");
    if (std::string(id) == "tool") { check(generate(provider) == AGENT_OK, "tool mismatch prefix"); check(execute(executor, "{\"changed\":true}") != AGENT_OK, "changed tool allowed"); }
    if (std::string(id) == "unconfigured") check(execute(executor) != AGENT_OK, "unconfigured replay dispatched");
    check(fake.providers == providers && fake.tools == 1, "mismatch made fresh I/O");
    check(guard.finish(result(true))["ok"] == false, "mismatch became success");
  }
  {
    Fake fake; checkpoint("corrupt", fake);
    sql("UPDATE workflow_effects SET receipt_json=replace(receipt_json,'receipted','tampered') WHERE task_id='corrupt'");
    WorkflowModelEffect guard(ctx("corrupt"), task_request);
    check(guard.preflight()["needs_input"] == true && fake.tools == 1, "corrupt receipt replayed");
  }
  {
    Fake fake; checkpoint("budget", fake);
    WorkflowModelEffect guard(ctx("budget"), task_request); check(guard.preflight().isNull(), "budget preflight");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2;
    guard.set_fresh_budget(true, 0, true, 1); configure(guard, options);
    auto provider = guard.wrap_provider(fake.provider()); auto executor = guard.wrap(fake.executor());
    check(generate(provider) == AGENT_OK && execute(executor) == AGENT_OK && generate(provider, 1) == AGENT_OK, "budget replay");
    check(execute(executor) == AGENT_ERR_LIMIT && generate(provider, 2) != AGENT_OK, "frontier budget bypass");
    const auto out = guard.finish(result(false));
    check(out["retryable"] == false && out["effect"]["resume_available"] == true && out["needs_input"] != true &&
      out["steps_executed"] == 1 && out["tool_calls_total"] == 0 && fake.tools == 1, "budget refused known progress or lost usage");
  }
  {
    Fake fake; WorkflowModelEffect guard(ctx("context"), task_request); check(guard.preflight().isNull(), "context preflight");
    ToolLoopOptions options; configure(guard, options); auto provider = guard.wrap_provider(fake.provider());
    fake.status = AGENT_ERR_CONTEXT_TOO_LONG; check(generate(provider) == AGENT_ERR_CONTEXT_TOO_LONG, "context status");
    fake.status = AGENT_OK; check(generate(provider, 1) == AGENT_OK && execute(guard.wrap(fake.executor())) == AGENT_OK, "context prefix");
    WorkflowModelEffect restored(ctx("context"), task_request); check(restored.preflight().isNull(), "context restore");
    configure(restored, options); provider = restored.wrap_provider(fake.provider());
    check(generate(provider) == AGENT_ERR_CONTEXT_TOO_LONG && generate(provider, 1) == AGENT_OK &&
      execute(restored.wrap(fake.executor())) == AGENT_OK && fake.tools == 1 && fake.providers == 2, "context error control flow not replayed");
  }
  {
    Fake fake; const auto id = checkpoint("cas", fake);
    AgentDb::WorkflowEffectRow row, saved; bool found = false;
    check(db.get_workflow_effect(id, &row, &found, &error) && found, "CAS lookup");
    check(db.checkpoint_workflow_model_effect(row, row.receipt_json, false, &saved, &error), error);
    check(!db.checkpoint_workflow_model_effect(row, row.receipt_json, true, &saved, &error), "stale checkpoint admitted");
    check(row.attempt == saved.attempt, "checkpoint counted a redispatch");
  }
  {
    Fake fake; checkpoint("stop", fake);
    WorkflowModelEffect guard(ctx("stop"), task_request); check(guard.preflight().isNull(), "stop preflight");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2; configure(guard, options);
    auto provider = guard.wrap_provider(fake.provider()); auto executor = guard.wrap(fake.executor());
    check(generate(provider) == AGENT_OK && execute(executor) == AGENT_OK && generate(provider, 1) == AGENT_OK, "stop prefix");
    sql("UPDATE workflows SET cancel_requested=1 WHERE workflow_id='wf'");
    check(execute(executor) != AGENT_OK && fake.tools == 1, "DB owner stop ignored");
    sql("UPDATE workflows SET cancel_requested=0 WHERE workflow_id='wf'");
  }
  {
    Fake fake; fake.malformed = true; WorkflowModelEffect guard(ctx("malformed"), task_request);
    check(guard.preflight().isNull(), "malformed preflight"); ToolLoopOptions options; configure(guard, options);
    check(generate(guard.wrap_provider(fake.provider())) != AGENT_OK && execute(guard.wrap(fake.executor())) != AGENT_OK && !fake.tools,
      "invalid response pointer allowed dispatch");
  }
  {
    Fake fake; fake.tool_response = false; WorkflowModelEffect guard(ctx("limit"), task_request);
    check(guard.preflight().isNull(), "limit preflight"); ToolLoopOptions options; configure(guard, options);
    auto provider = guard.wrap_provider(fake.provider());
    for (size_t i = 0; i < 512; ++i) check(generate(provider, i) == AGENT_OK, "bounded history failed early");
    check(generate(provider, 512) != AGENT_OK && execute(guard.wrap(fake.executor())) != AGENT_OK && !fake.tools, "record cap ignored");
  }
  {
    Fake fake; WorkflowModelEffect guard(ctx("receipt_fail"), task_request);
    check(guard.preflight().isNull(), "receipt failure preflight"); ToolLoopOptions options; configure(guard, options);
    check(generate(guard.wrap_provider(fake.provider())) == AGENT_OK, "receipt failure provider");
    sql("CREATE TRIGGER reject_checkpoint BEFORE UPDATE OF receipt_json ON workflow_effects WHEN NEW.task_id='receipt_fail' AND OLD.state='dispatching' BEGIN SELECT RAISE(FAIL,'lost receipt'); END");
    check(execute(guard.wrap(fake.executor())) != AGENT_OK && fake.tools == 1, "failed receipt save ignored");
    sql("DROP TRIGGER reject_checkpoint");
    WorkflowModelEffect restored(ctx("receipt_fail"), task_request);
    check(restored.preflight()["needs_input"] == true, "failed receipt persistence was replayable");
  }
  {
    Fake fake; WorkflowModelEffect guard(ctx("terminal_limit"), task_request);
    check(guard.preflight().isNull(), "terminal limit preflight"); ToolLoopOptions options; configure(guard, options);
    check(generate(guard.wrap_provider(fake.provider())) == AGENT_OK && execute(guard.wrap(fake.executor())) == AGENT_OK, "terminal prefix");
    const auto out = guard.finish(result(false)); // Core step/repetition/argument limit, no provider failure.
    check(out["retryable"] == false && out["effect"]["resume_available"] == true && out["needs_input"] != true,
      "terminal core limit mislabeled receipted host actions as unknown");
  }
  {
    Fake fake; checkpoint("fresh_policy", fake);
    WorkflowModelEffect guard(ctx("fresh_policy"), task_request); check(guard.preflight().isNull(), "fresh policy preflight");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2; configure(guard, options);
    auto provider = guard.wrap_provider(fake.provider());
    auto executor = guard.wrap(fake.executor(), [&](const char*, const char*) { ++fake.approvals; return AGENT_ERR_INTERNAL; });
    check(generate(provider) == AGENT_OK && execute(executor) == AGENT_OK && generate(provider, 1) == AGENT_OK, "policy replay");
    check(execute(executor) != AGENT_OK && fake.tools == 1 && fake.approvals == 1, "fresh call skipped policy");
    const auto out = guard.finish(result(false));
    check(out["retryable"] == false && out["effect"]["resume_available"] == true, "policy denial resumed automatically");
  }
  {
    Fake fake; checkpoint("zero_steps", fake);
    WorkflowModelEffect guard(ctx("zero_steps"), task_request); check(guard.preflight().isNull(), "zero steps preflight");
    ToolLoopOptions options; options.max_steps = 4; options.max_tool_calls_total = 2;
    guard.set_fresh_budget(true, 0, true, 0); configure(guard, options);
    auto provider = guard.wrap_provider(fake.provider()); auto executor = guard.wrap(fake.executor());
    check(generate(provider) == AGENT_OK && execute(executor) == AGENT_OK, "zero steps cached prefix");
    check(generate(provider, 1) == AGENT_ERR_LIMIT && fake.providers == 2 && fake.tools == 1, "zero step frontier called provider");
    const auto out = guard.finish(result(false));
    check(out["steps_executed"] == 0 && out["tool_calls_total"] == 0 && out["retryable"] == false, "zero step accounting");
  }
  {
    Fake fake; OpenAIToolProviderCtx original;
    original.reasoning_by_tool_call_ids["original-call"] = "synthetic original reasoning";
    WorkflowModelEffect guard(ctx("reasoning"), task_request); check(guard.preflight().isNull(), "reasoning preflight");
    ToolLoopOptions options; configure(guard, options);
    check(generate(guard.wrap_provider(fake.provider(), &original)) == AGENT_OK &&
      execute(guard.wrap(fake.executor())) == AGENT_OK, "reasoning prefix");
    WorkflowModelEffect restored(ctx("reasoning"), task_request); check(restored.preflight().isNull(), "reasoning restore");
    configure(restored, options); OpenAIToolProviderCtx replacement;
    check(generate(restored.wrap_provider(fake.provider(), &replacement)) == AGENT_OK &&
      replacement.reasoning_by_tool_call_ids.at("original-call") == "synthetic original reasoning" &&
      fake.providers == 1, "required provider metadata was not restored");
    WorkflowModelEffect unavailable(ctx("reasoning"), task_request); check(unavailable.preflight().isNull(), "reasoning unavailable preflight");
    configure(unavailable, options);
    check(generate(unavailable.wrap_provider(fake.provider())) != AGENT_OK && fake.providers == 1,
      "required metadata silently discarded by unsupported adapter");
  }
  {
    Fake fake; OpenAIToolProviderCtx provider_context;
    provider_context.reasoning_by_tool_call_ids["original-call"] = std::string(2 * 1024 * 1024 + 1, 'x');
    WorkflowModelEffect guard(ctx("reasoning_large"), task_request); check(guard.preflight().isNull(), "large reasoning preflight");
    ToolLoopOptions options; configure(guard, options);
    check(generate(guard.wrap_provider(fake.provider(), &provider_context)) != AGENT_OK &&
      execute(guard.wrap(fake.executor())) != AGENT_OK && !fake.tools, "unbounded reasoning admitted a host call");
  }

  auto vision_request = [] {
    Json::Value value; value["model"]="stub"; value["messages"][0]["role"]="user";
    value["messages"][0]["content"]="synthetic-original-image-private-marker";
    value["max_completion_tokens"]=32; return value;
  };
  auto vision_result = [] {
    Json::Value value; value["text"]="original image description"; value["error"]="";
    value["http_status"]=200; value["usage"]["prompt_tokens"]=3;
    value["usage"]["completion_tokens"]=1; value["usage"]["total_tokens"]=4; return value;
  };
  for(const char* id:{"vision_complete","vision_resume","vision_request","vision_fallback"}) {
    Fake fake; int preparations=0;auto prepared=vision_result();
    if(std::string(id)=="vision_fallback"){prepared["text"]="";prepared["error"]="original unavailable";prepared["http_status"]=503;}
    auto invoke=[&]{++preparations;return prepared;};
    WorkflowModelEffect first(ctx(id),task_request);check(first.preflight().isNull(),"vision initial");
    ToolLoopOptions opt;opt.max_steps=3;opt.max_tool_calls_total=1;configure(first,opt);
    Json::Value value;bool replayed=false;
    check(first.vision_prefetch(vision_request(),invoke,&value,&replayed)&&!replayed&&value==prepared,"vision original");
    auto provider=first.wrap_provider(fake.provider());
    check(generate(provider)==AGENT_OK&&execute(first.wrap(fake.executor()))==AGENT_OK,"vision host prefix");
    if(std::string(id)=="vision_complete"){
      fake.tool_response=false;check(generate(provider,1)==AGENT_OK,"vision complete provider");
      check(first.finish(result(true))["ok"]==true,"vision complete receipt");
      WorkflowModelEffect cached(ctx(id),task_request);const auto replay=cached.preflight();
      check(replay["ok"]==true&&replay["steps_executed"]==0&&replay["effect"]["prefetch_calls"]==0&&
            replay["effect"]["original_usage"]["prefetch_calls"]==1&&preparations==1,"vision complete receipt charged new analysis");
      continue;
    }
    fake.status=AGENT_ERR_INTERNAL;check(generate(provider,1)==AGENT_ERR_INTERNAL,"vision frontier failure");
    const auto failed=first.finish(result(false));
    check(failed["steps_executed"]==3&&failed["effect"]["prefetch_calls"]==1&&failed["retryable"]==true,"vision fresh accounting");
    AgentDb::WorkflowEffectRow row;bool found=false;
    check(db.get_workflow_effect(failed["effect"]["effect_id"].asString(),&row,&found,&error)&&found,"vision checkpoint");
    check(row.receipt_json.find("synthetic-original-image-private-marker")==std::string::npos,"image payload copied into receipt");
    WorkflowModelEffect resumed(ctx(id),task_request);check(resumed.preflight().isNull(),"vision restore");
    ToolLoopOptions resumed_opt;resumed_opt.max_steps=3;resumed_opt.max_tool_calls_total=1;
    resumed.set_fresh_budget(true,0,true,1);configure(resumed,resumed_opt);
    auto request=vision_request();request["max_completion_tokens"]=1; // Smaller remaining token grant is not changed image intent.
    if(std::string(id)=="vision_request"){
      request["messages"][0]["content"]="changed image";
      check(!resumed.vision_prefetch(request,invoke,&value,&replayed)&&preparations==1,"changed image reinterpreted");
      check(resumed.finish(result(false))["needs_input"]==true,"changed image not blocked");continue;
    }
    check(resumed.vision_prefetch(request,invoke,&value,&replayed)&&replayed&&value==prepared&&preparations==1,"vision regenerated on resume");
    fake.status=AGENT_OK;provider=resumed.wrap_provider(fake.provider());
    check(generate(provider)==AGENT_OK&&execute(resumed.wrap(fake.executor()))==AGENT_OK,"vision cached tools");
    fake.tool_response=false;check(generate(provider,1)==AGENT_OK,"vision final frontier");
    const auto done=resumed.finish(result(true));
    check(done["ok"]==true&&done["steps_executed"]==1&&done["tool_calls_total"]==0&&
          done["effect"]["replayed_prefetch_calls"].asUInt64()==1&&done["effect"]["prefetch_calls"]==0&&fake.tools==1,"vision replay metering: "+done.toStyledString());
  }
  for(const char* id:{"vision_shape","vision_budget","vision_large","vision_input","vision_invalid","vision_session","vision_stop","vision_order"}){
    int preparations=0;auto context=ctx(id);bool stopped=false;context.should_cancel=[&]{return stopped;};
    WorkflowModelEffect guard(context,task_request);check(guard.preflight().isNull(),"vision refusal preflight");
    if(std::string(id)=="vision_session"){
      std::vector<AgentDb::MessageRow> rows;int64_t revision=0;
      check(db.load_session_messages(id,&rows,&error,&revision),error);
      check(guard.bind_session(id,revision).isNull(),"vision bind");
      check(db.replace_session_messages(id,{{"user","newer"}},1,&error),error);
    }
    ToolLoopOptions opt;opt.max_steps=2;
    if(std::string(id)=="vision_budget")guard.set_fresh_budget(false,0,true,0);
    configure(guard,opt);
    auto request=vision_request(),prepared=vision_result();
    if(std::string(id)=="vision_large")prepared["text"]=std::string(2*1024*1024+1,'x');
    if(std::string(id)=="vision_shape")request="invalid";
    if(std::string(id)=="vision_input")request["messages"][0]["content"]=std::string(8*1024*1024+1,'x');
    if(std::string(id)=="vision_invalid")prepared["usage"]["total_tokens"]=-1;
    if(std::string(id)=="vision_stop")stopped=true;
    auto invoke=[&]{++preparations;return prepared;};Json::Value value;bool replayed=false;
    if(std::string(id)=="vision_order")check(guard.vision_prefetch(request,invoke,&value,&replayed),"first vision order");
    check(!guard.vision_prefetch(request,invoke,&value,&replayed),"invalid vision request accepted");
    const auto out=guard.finish(result(false));check(out["ok"]==false,"invalid vision succeeded");
    const int expected=(std::string(id)=="vision_large"||std::string(id)=="vision_invalid"||std::string(id)=="vision_order")?1:0;
    check(preparations==expected,"vision refusal made unexpected provider call");
    if(std::string(id)=="vision_session")check(out["blocker"]["kind"]=="model_session_changed","vision ignored newer session");
    if(std::string(id)=="vision_stop")check(out["cancelled"]==true,"vision ignored owner stop");
  }
  db.close(); std::filesystem::remove_all(root);
  std::cout << "Transcript replay, exact call identities, zero-cost grants, policy, CAS, corruption, pending receipts and owner stops passed\n";
}
