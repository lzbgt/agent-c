#include "workflow_model_effect.h"
#include "tool_loop.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <thread>

namespace {
using agentd::AgentDb;
void check(bool ok, const std::string& why) { if (!ok) { std::cerr << why << '\n'; std::exit(1); } }
Json::Value result(bool ok) { Json::Value v; v["ok"]=ok; v["error"]=ok?"":"provider unavailable"; return v; }
std::vector<AgentDb::MessageRow> messages(const std::string& text) { return {{"user",text}}; }
struct Fake {
  int providers=0, tools=0;
  bool final=false, fail=false;
  agent_tool_provider_t provider() { return {this,[](void* p,const agent_tool_provider_request_t*,agent_tool_provider_response_t* out) {
    auto& f=*static_cast<Fake*>(p); ++f.providers; *out={};
    if(f.fail) return AGENT_ERR_INTERNAL;
    agent_string_set_copy(&out->assistant_content,"verified answer",15);
    if(!f.final) {
      out->tool_calls=static_cast<agent_tool_call_t*>(agent_malloc(sizeof(agent_tool_call_t)));
      check(out->tool_calls!=nullptr,"allocation"); *out->tool_calls={}; out->tool_call_count=1;
      agent_string_set_copy(&out->tool_calls[0].id,"call-1",6);
      agent_string_set_copy(&out->tool_calls[0].name,"proc_exec",9);
      agent_string_set_copy(&out->tool_calls[0].arguments_json,"{}",2);
    }
    return AGENT_OK;
  }}; }
  agent_tool_executor_t executor() { return {this,[](void* p,const char*,const char*,agent_string_t* out) {
    ++static_cast<Fake*>(p)->tools; return agent_string_set_copy(out,R"({"ok":true})",11);
  }}; }
};
agent_status_t generate(agent_tool_provider_t provider,size_t step) {
  agent_tool_provider_request_t req{}; req.model="stub"; req.step=step;
  agent_tool_provider_response_t out{}; const auto status=provider.generate(provider.ctx,&req,&out);
  agent_tool_provider_response_free(&out); return status;
}
agent_status_t execute(agent_tool_executor_t executor) {
  agent_string_t out{}; const auto status=executor.execute(executor.ctx,"proc_exec","{}",&out);
  agent_string_free(&out); return status;
}
void configure(agentd::WorkflowModelEffect& guard) {
  ToolLoopOptions options; options.max_steps=4; options.max_tool_calls_total=2;
  Json::Value scope,identity; scope["cwd"]="/synthetic"; identity["model"]="stub";
  guard.bind_scope(scope); guard.configure_transcript(&options,identity);
}
}
int main() {
  using namespace agentd;
  const auto root=std::filesystem::temp_directory_path()/("model-session-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path=(root/"state.sqlite").string();
  AgentDb db,peer; std::string error; check(db.open(path,&error),error); check(peer.open(path,&error),error);
  auto sql=[&](const std::string& command) {
    sqlite3* raw=nullptr; check(sqlite3_open(path.c_str(),&raw)==SQLITE_OK,"fixture open");
    check(sqlite3_exec(raw,command.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK,sqlite3_errmsg(raw)); sqlite3_close(raw);
  };
  auto snapshot=[&](const std::string& id) {
    std::vector<AgentDb::MessageRow> rows; int64_t revision=-1;
    check(db.load_session_messages(id,&rows,&error,&revision),error);
    return std::make_pair(rows,AgentDb::SessionFence{id,revision});
  };
  auto seed=[&](const std::string& id) {
    check(db.replace_session_messages(id,messages("original"),1,&error),error);
    return snapshot(id).second;
  };
  auto same=[&](const std::string& id,const std::string& text) {
    const auto rows=snapshot(id).first;
    check(rows.size()==1 && rows[0].content==text,"conversation changed unexpectedly: "+id);
  };
  // Upgrade a real pre-generation database with existing conversation rows.
  check(db.replace_session_messages("migrated",messages("pre-v39 history"),1,&error),error);
  peer.close();db.close();
  sql("DROP TABLE session_revisions; UPDATE meta SET value='38' WHERE key='schema_version';");
  check(db.open(path,&error),error);check(peer.open(path,&error),error);
  check(snapshot("migrated").second.revision==1,"existing session migration generation");
  same("migrated","pre-v39 history");
  check(snapshot("absent").second.revision==0,"missing session has a generation");
  const auto original=seed("generation");
  check(peer.replace_session_messages("generation",messages("original"),2,&error),error);
  check(!db.session_matches(original,&error),"identical rewrite did not advance generation");
  check(db.delete_session("generation",&error),error);
  check(db.replace_session_messages("generation",messages("original"),3,&error),error);
  check(!db.replace_session_messages("generation",messages("stale"),4,&error,&original),"delete/recreate accepted old snapshot");
  same("generation","original");

  const auto race=seed("race"); std::atomic<int> winners{0};
  std::thread a([&]{std::string e;if(db.replace_session_messages("race",messages("a"),5,&e,&race))++winners;});
  std::thread b([&]{std::string e;if(peer.replace_session_messages("race",messages("b"),6,&e,&race))++winners;});
  a.join();b.join();check(winners==1,"two session CAS writers committed");
  const auto race_after=snapshot("race");
  check(race_after.second.revision==race.revision+1 && race_after.first.size()==1,"CAS generation mismatch");

  WorkflowEffectContext context{&db,"wf",""};
  AgentDb::WorkflowRow wf; wf.workflow_id="wf";wf.status="queued";wf.spec_json="{}";wf.created_unix_ms=wf.updated_unix_ms=1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for(const char* id:{"resume","rollback","drift","recreate","admission","second_admission","frontier","final","stop","pure","oversized"}) {
    AgentDb::WorkflowTaskRow task;task.workflow_id="wf";task.task_id=id;task.status="queued";
    task.request_json="{}";task.depends_on_json="[]";task.created_unix_ms=task.updated_unix_ms=1;tasks.push_back(task);
  }
  check(db.create_workflow(wf,tasks,&error),error);
  const std::string request=R"({"prompt":"work","tools":"host","no_session":false})";
  auto ctx=[&](const char* id){auto c=context;c.task_id=id;return c;};
  auto start=[&](WorkflowModelEffect& guard,const AgentDb::SessionFence& fence) {
    check(guard.preflight().isNull(),"preflight");
    check(guard.bind_session(fence.session_id,fence.revision).isNull(),"bind session");
    configure(guard);
  };
  auto prefix=[&](WorkflowModelEffect& guard,Fake& fake) {
    check(generate(guard.wrap_provider(fake.provider()),0)==AGENT_OK,"provider prefix");
    check(execute(guard.wrap(fake.executor()))==AGENT_OK,"tool prefix");
  };
  auto effect=[&](const std::string& id) {
    std::vector<AgentDb::WorkflowEffectRow> rows;check(db.list_workflow_effects("wf",&rows,&error),error);
    for(const auto& row:rows)if(row.task_id==id){AgentDb::WorkflowEffectRow full;bool found=false;check(db.get_workflow_effect(row.effect_id,&full,&found,&error)&&found,error);return full;}
    return AgentDb::WorkflowEffectRow{};
  };

  // A normal transient failure leaves the conversation untouched. The next
  // attempt reuses exact receipts, then commits messages and result together.
  {
    const auto fence=seed("resume");Fake fake;
    WorkflowModelEffect first(ctx("resume"),request);start(first,fence);prefix(first,fake);
    fake.fail=true;check(generate(first.wrap_provider(fake.provider()),1)!=AGENT_OK,"failure missing");
    check(first.finish(result(false))["retryable"]==true,"frontier not retryable");same("resume","original");
    check(snapshot("resume").second.revision==fence.revision,"failed attempt wrote session");
    fake.fail=false;WorkflowModelEffect resumed(ctx("resume"),request);start(resumed,fence);prefix(resumed,fake);
    fake.final=true;check(generate(resumed.wrap_provider(fake.provider()),1)==AGENT_OK,"frontier");
    resumed.stage_session(messages("completed"));const auto out=resumed.finish(result(true));
    check(out["ok"]==true&&out["effect"]["session_committed"]==true&&fake.tools==1&&fake.providers==3,out.toStyledString());
    same("resume","completed");check(snapshot("resume").second.revision==fence.revision+1,"duplicate session commit");
    check(resumed.finish(out)==out,"double finish changed result");
    check(peer.replace_session_messages("resume",messages("newer"),9,&error),error);
    WorkflowModelEffect cached(ctx("resume"),request);const auto replay=cached.preflight();
    check(replay["ok"]==true&&replay["effect"]["replayed"]==true&&replay["effect"]["session_committed"]==true,"cached session receipt");
    same("resume","newer");check(fake.tools==1,"cached completion repeated tool");
  }

  // A failure while replacing messages rolls back the already-updated effect.
  // The final provider result remains a checkpoint, not an unpaired completion.
  {
    const auto fence=seed("rollback");Fake fake;WorkflowModelEffect first(ctx("rollback"),request);
    start(first,fence);prefix(first,fake);fake.final=true;check(generate(first.wrap_provider(fake.provider()),1)==AGENT_OK,"final response");
    first.stage_session(messages("completed"));
    sql("CREATE TRIGGER reject_session BEFORE INSERT ON messages WHEN NEW.session_id='rollback' BEGIN SELECT RAISE(FAIL,'fixture session write failure'); END");
    check(first.finish(result(true))["needs_input"]==true,"failed session write reported success");
    same("rollback","original");check(effect("rollback").state=="dispatching","receipt committed without session");
    sql("DROP TRIGGER reject_session");
    WorkflowModelEffect restored(ctx("rollback"),request);start(restored,fence);prefix(restored,fake);
    check(generate(restored.wrap_provider(fake.provider()),1)==AGENT_OK,"final response replay");
    restored.stage_session(messages("completed"));check(restored.finish(result(true))["ok"]==true,"retry atomic commit");
    same("rollback","completed");check(fake.tools==1&&fake.providers==2,"atomic retry repeated external work");
  }
  for(const char* id:{"drift","recreate"}) {
    const auto fence=seed(id);Fake fake;WorkflowModelEffect first(ctx(id),request);start(first,fence);prefix(first,fake);
    if(std::string(id)=="recreate")check(peer.delete_session(id,&error),error);
    check(peer.replace_session_messages(id,messages("newer"),10,&error),error);
    WorkflowModelEffect resumed(ctx(id),request);check(resumed.preflight().isNull(),"drift preflight");
    check(resumed.bind_session(id,snapshot(id).second.revision)["blocker"]["kind"]=="model_session_changed","changed conversation resumed");
    const auto out=resumed.finish(result(false));check(out["blocker"]["kind"]=="model_session_changed",out.toStyledString());
    same(id,"newer");const auto row=effect(id);
    check(row.state=="unknown"&&row.receipt_json.find("\"records\"")!=std::string::npos,"conflict erased original receipts");
    check(fake.tools==1&&fake.providers==1,"changed conversation repeated work");
  }
  {
    const auto fence=seed("admission");Fake fake;WorkflowModelEffect guard(ctx("admission"),request);start(guard,fence);
    check(generate(guard.wrap_provider(fake.provider()),0)==AGENT_OK,"admission provider");
    check(peer.replace_session_messages("admission",messages("newer"),11,&error),error);
    check(execute(guard.wrap(fake.executor()))!=AGENT_OK&&fake.tools==0,"stale session admitted host tool");
    check(guard.finish(result(false))["blocker"]["kind"]=="model_session_changed","admission conflict absent");same("admission","newer");
  }
  {
    const auto fence=seed("second_admission");Fake fake;WorkflowModelEffect guard(ctx("second_admission"),request);
    start(guard,fence);prefix(guard,fake);
    check(generate(guard.wrap_provider(fake.provider()),1)==AGENT_OK,"second admission provider");
    check(peer.replace_session_messages("second_admission",messages("newer"),11,&error),error);
    check(execute(guard.wrap(fake.executor()))!=AGENT_OK&&fake.tools==1,"stale session admitted second host tool");
    const auto out=guard.finish(result(false));
    check(out["blocker"]["kind"]=="model_session_changed","second admission conflict absent");
    same("second_admission","newer");const auto row=effect("second_admission");
    check(row.state=="unknown"&&row.receipt_json.find("\"pending\":null")!=std::string::npos,"rejected admission falsely marked pending");
  }
  {
    const auto fence=seed("frontier");Fake fake;WorkflowModelEffect guard(ctx("frontier"),request);start(guard,fence);prefix(guard,fake);
    check(peer.replace_session_messages("frontier",messages("newer"),12,&error),error);
    check(generate(guard.wrap_provider(fake.provider()),1)!=AGENT_OK&&fake.providers==1,"stale session sent fresh provider request");
    check(guard.finish(result(false))["blocker"]["kind"]=="model_session_changed","frontier conflict absent");same("frontier","newer");
  }
  {
    const auto fence=seed("final");Fake fake;WorkflowModelEffect guard(ctx("final"),request);start(guard,fence);prefix(guard,fake);
    fake.final=true;check(generate(guard.wrap_provider(fake.provider()),1)==AGENT_OK,"final provider");
    guard.stage_session(messages("stale completion"));check(peer.replace_session_messages("final",messages("newer"),13,&error),error);
    const auto out=guard.finish(result(true));check(out["ok"]==false&&out["blocker"]["kind"]=="model_session_changed",out.toStyledString());
    same("final","newer");check(effect("final").state=="unknown","final conflict not durably stopped");
  }
  {
    const auto fence=seed("stop");Fake fake;WorkflowModelEffect guard(ctx("stop"),request);start(guard,fence);prefix(guard,fake);
    fake.final=true;check(generate(guard.wrap_provider(fake.provider()),1)==AGENT_OK,"stop final provider");guard.stage_session(messages("unwanted"));
    sql("UPDATE workflows SET cancel_requested=1 WHERE workflow_id='wf'");
    check(guard.finish(result(true))["ok"]==false,"owner stop was revived at session publication");same("stop","original");
    check(effect("stop").state=="dispatching","stopped session falsely completed");
    sql("UPDATE workflows SET cancel_requested=0 WHERE workflow_id='wf'");
  }
  {
    const auto fence=seed("pure");WorkflowModelEffect guard(ctx("pure"),request);start(guard,fence);guard.stage_session(messages("pure answer"));
    check(guard.finish(result(true))["ok"]==true,"pure session publication failed");same("pure","pure answer");
    check(effect("pure").effect_id.empty(),"pure model run created host effect");
  }
  {
    const auto fence=seed("oversized");WorkflowModelEffect guard(ctx("oversized"),request);start(guard,fence);
    guard.stage_session(messages(std::string(8*1024*1024+1,'x')));
    check(guard.finish(result(true))["ok"]==false,"oversized session publication accepted");same("oversized","original");
  }
  peer.close();db.close();std::filesystem::remove_all(root);
}
