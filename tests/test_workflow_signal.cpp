#include "workflow_signal.h"
#include "workflow_budget.h"
#include "agent/json_c14n.h"
#include "agentd/api.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <thread>

namespace {
void check(bool ok, const std::string& message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
Json::Value spec() {
  Json::Value out; out["name"] = "inspection.finished"; out["title"] = "Inspection result"; return out;
}
std::string digest(const std::string& json) {
  char out[80] = {}, error[160] = {};
  check(agent_json_c14n_sha256_token(json.data(), json.size(), out, error, sizeof(error)) == AGENT_OK, error);
  return out;
}
void sql(const std::string& path, const std::string& statement) {
  sqlite3* db = nullptr; check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "fixture open");
  sqlite3_busy_timeout(db, 2000);
  check(sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL");
  sqlite3_close(db);
}
agentd::AgentDb::WorkflowTaskRow task(agentd::AgentDb& db, const std::string& id) {
  std::vector<agentd::AgentDb::WorkflowTaskRow> rows; std::string error;
  check(db.list_workflow_tasks("signals", &rows, &error), error);
  for (const auto& row : rows) if (row.task_id == id) return row;
  check(false, "task missing"); return {};
}
}
int main() {
  using namespace agentd;
  using Result = AgentDb::WorkflowSignalResolution;
  const auto root = std::filesystem::temp_directory_path() / ("agent-signal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.sqlite").string();
  AgentDb db; std::string error;
  check(db.open(path, &error), error);
  db.close(); sql(path, "DROP TABLE workflow_signals; UPDATE meta SET value='41' WHERE key='schema_version';");
  check(db.open(path, &error), error);
  AgentDb::WorkflowRow wf; wf.workflow_id = "signals"; wf.status = "running"; wf.spec_json = "{}";
  wf.created_unix_ms = wf.updated_unix_ms = 1;
  std::vector<AgentDb::WorkflowTaskRow> tasks;
  for (const auto* id : {"input", "timeout", "stop", "tamper", "payload-tamper", "race"}) {
    AgentDb::WorkflowTaskRow t; t.workflow_id = wf.workflow_id; t.task_id = id;
    t.status = "running"; t.attempt = 1; t.max_attempts = 3;
    t.request_json = "{}"; t.depends_on_json = "[]"; t.created_unix_ms = t.updated_unix_ms = 1;
    tasks.push_back(t);
  }
  check(db.create_workflow(wf, tasks, &error), error);
  check(workflow_signal_validate(spec(), &error), error);
  for (const auto* name : {"", "bad name", "bad/name"}) {
    auto bad = spec(); bad["name"] = name; check(!workflow_signal_validate(bad, &error), "invalid name admitted");
  }
  auto bad = spec(); bad["timeout_ms"] = Json::Int64(2592000001LL);
  check(!workflow_signal_validate(bad, &error), "oversized expiry admitted");
  bad = spec(); bad["accept_before_activation"]="true";
  check(!workflow_signal_validate(bad,&error),"nonboolean mailbox admitted");
  bad = spec(); bad["actor"] = "owner"; check(!workflow_signal_validate(bad, &error), "caller identity admitted");
  bad = spec(); bad["title"] = std::string("bad\0title", 9);
  check(!workflow_signal_validate(bad, &error), "NUL title admitted");
  bool yielded = false;
  auto output = workflow_signal_step(&db, task(db,"input"), spec(), &yielded);
  check(yielded && output.isNull() && task(db,"input").attempt == 0 && task(db,"input").status == "queued", "wait consumed attempt/worker");
  AgentDb::WorkflowSignalRow row,saved;
  check(db.get_workflow_signal("signals","input",&row,&error),error);
  const auto identity = row.signal_id;
  db.close(); check(db.open(path,&error),error);
  check(db.get_workflow_signal("signals","input",&saved,&error) && saved.signal_id == identity, "restart lost wait");
  const std::string payload = "{\"accepted\":true,\"count\":3}", hash = digest(payload);
  check(db.resolve_workflow_signal(row,payload,hash,"delivery-1",&saved,&error) == Result::Recorded,error);
  const auto ready = task(db,"input").ready_unix_ms;
  check(db.resolve_workflow_signal(row,payload,hash,"delivery-1",&saved,&error) == Result::Replayed,error);
  check(task(db,"input").ready_unix_ms == ready,"replay changed readiness");
  check(db.resolve_workflow_signal(row,payload,hash,"delivery-2",&saved,&error) == Result::Conflict,"new delivery replaced receipt");
  check(db.resolve_workflow_signal(row,"{}",digest("{}"),"delivery-1",&saved,&error) == Result::Conflict,"changed payload accepted");
  db.close(); check(db.open(path,&error),error);
  sql(path,"UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='input';");
  output = workflow_signal_step(&db,task(db,"input"),spec(),&yielded);
  check(!yielded && output["ok"] == true && output["payload"]["count"] == 3 && output["elapsed_ms"] == 0,"receipt did not resume exactly");

  // Upgrade a real v42 signal row without changing its identity, expiry or receipt.
  db.close(); sql(path, "ALTER TABLE workflow_signals DROP COLUMN activated_unix_ms; UPDATE meta SET value='42' WHERE key='schema_version';");
  check(db.open(path,&error),error);
  check(db.get_workflow_signal("signals","input",&saved,&error) && saved.signal_id==identity &&
    saved.payload_json==payload && saved.payload_hash==hash && saved.activated_unix_ms==saved.created_unix_ms,
    "v42 migration altered original receipt");
  check(db.resolve_workflow_signal(saved,payload,hash,"delivery-1",&row,&error)==Result::Replayed,error);

  // Submission and its opt-in slot are atomic. A failed insert leaves no run/task.
  AgentDb::WorkflowRow early_wf=wf;early_wf.workflow_id="mailbox-atomic";
  AgentDb::WorkflowTaskRow early_task;
  early_task.workflow_id=early_wf.workflow_id;early_task.task_id="input";early_task.status="queued";
  early_task.depends_on_json="[]";early_task.ready_unix_ms=9876543210000LL;
  auto early_spec=spec();early_spec["accept_before_activation"]=true;early_spec["timeout_ms"]=1000;
  Json::Value early_request;early_request["kind"]="signal";early_request["signal"]=early_spec;
  early_task.request_json=Json::writeString(Json::StreamWriterBuilder(),early_request);
  AgentDb::WorkflowSignalRow early_definition;
  check(workflow_signal_definition(early_wf.workflow_id,early_task.task_id,early_spec,&early_definition,&error),error);
  sql(path,"CREATE TRIGGER fail_signal_slot BEFORE INSERT ON workflow_signals WHEN NEW.workflow_id='mailbox-atomic' BEGIN SELECT RAISE(ABORT,'injected mailbox insert failure'); END;");
  check(!db.create_workflow(early_wf,{early_task},&error,{early_definition}),"partial mailbox submit accepted");
  AgentDb::WorkflowRow absent;
  check(!db.get_workflow(early_wf.workflow_id,&absent,&error),"failed mailbox submit persisted run");
  std::vector<AgentDb::WorkflowTaskRow> early_tasks;
  check(db.list_workflow_tasks(early_wf.workflow_id,&early_tasks,&error)&&early_tasks.empty(),"failed mailbox submit persisted task");
  sql(path,"DROP TRIGGER fail_signal_slot;");
  check(db.create_workflow(early_wf,{early_task},&error,{early_definition}),error);
  check(db.get_workflow_signal(early_wf.workflow_id,"input",&row,&error) &&
    row.activated_unix_ms==0 && row.expires_unix_ms==0,error);
  Json::Value projection;
  check(workflow_signals_projection(&db,early_wf,&projection,&error)&&projection.size()==1&&
    projection[0]["state"]=="pending"&&projection[0]["available"]==true,"missing early slot projection");
  // A receipt write fault cannot partially publish a payload or alter readiness.
  sql(path,"CREATE TRIGGER fail_signal_receipt BEFORE UPDATE OF payload_json ON workflow_signals WHEN NEW.workflow_id='mailbox-atomic' BEGIN SELECT RAISE(ABORT,'injected receipt failure'); END;");
  check(db.resolve_workflow_signal(row,payload,hash,"early-delivery",&saved,&error)==Result::StorageError,"receipt fault not surfaced");
  check(db.get_workflow_signal(early_wf.workflow_id,"input",&saved,&error)&&saved.state=="waiting"&&saved.payload_json.empty(),"partial early receipt persisted");
  sql(path,"DROP TRIGGER fail_signal_receipt;");
  check(db.resolve_workflow_signal(row,payload,hash,"early-delivery",&saved,&error)==Result::Recorded,error);
  check(db.list_workflow_tasks(early_wf.workflow_id,&early_tasks,&error)&&early_tasks.size()==1&&
    early_tasks[0].ready_unix_ms==9876543210000LL&&early_tasks[0].attempt==0&&early_tasks[0].status=="queued",
    "early delivery advanced task");
  check(workflow_signals_projection(&db,early_wf,&projection,&error)&&projection[0]["state"]=="buffered"&&
    projection[0]["available"]==false&&!projection[0].isMember("payload"),"buffered projection leaked payload or accepted another input");

  // A delivery racing the scheduler's first claim must see either the unactivated
  // running slot or the atomically yielded wait, never a lost input window.
  {
    AgentDb peer;check(peer.open(path,&error),error);
    for (int iteration=0;iteration<8;++iteration) {
      auto racing_wf=early_wf;racing_wf.workflow_id="activation-race-"+std::to_string(iteration);
      auto claimed=early_task;claimed.workflow_id=racing_wf.workflow_id;claimed.status="running";claimed.attempt=1;
      AgentDb::WorkflowSignalRow racing_definition;
      check(workflow_signal_definition(racing_wf.workflow_id,claimed.task_id,early_spec,&racing_definition,&error),error);
      check(db.create_workflow(racing_wf,{claimed},&error,{racing_definition}),error);
      AgentDb::WorkflowSignalRow pending,delivered;
      check(db.get_workflow_signal(racing_wf.workflow_id,"input",&pending,&error),error);
      Json::Value action;bool did_yield=false;Result delivery=Result::StorageError;std::string delivery_error;
      std::thread activation([&]{action=workflow_signal_step(&db,claimed,early_spec,&did_yield);});
      std::thread arrival([&]{delivery=peer.resolve_workflow_signal(pending,payload,hash,"activation-race",&delivered,&delivery_error);});
      activation.join();arrival.join();
      check(delivery==Result::Recorded,delivery_error);
      check(did_yield || (action["ok"]==true && action["payload"]["count"]==3),"activation race lost buffered result");
      check(db.get_workflow_signal(racing_wf.workflow_id,"input",&saved,&error)&&
        saved.state=="resolved"&&saved.activated_unix_ms>0&&saved.payload_json==payload,"activation race lost receipt");
      check(peer.resolve_workflow_signal(pending,payload,hash,"activation-race",&delivered,&delivery_error)==Result::Replayed,
        "activation race changed immutable identity");
    }
  }

  auto timed=spec(); timed["timeout_ms"]=1000;
  workflow_signal_step(&db,task(db,"timeout"),timed,&yielded); check(yielded,"expiry wait");
  sql(path,"UPDATE workflow_signals SET expires_unix_ms=1 WHERE task_id='timeout'; UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='timeout';");
  output=workflow_signal_step(&db,task(db,"timeout"),timed,&yielded);
  check(!yielded && output["ok"]==false && output["retryable"]==false && output["signal"]["state"]=="expired","expiry retried");

  workflow_signal_step(&db,task(db,"race"),spec(),&yielded);
  check(yielded && db.get_workflow_signal("signals","race",&row,&error),error);
  AgentDb other; check(other.open(path,&error),error);
  Result first=Result::StorageError,second=Result::StorageError;
  std::thread a([&]{AgentDb::WorkflowSignalRow r;std::string e;first=db.resolve_workflow_signal(row,payload,hash,"a",&r,&e);});
  std::thread b([&]{AgentDb::WorkflowSignalRow r;std::string e;second=other.resolve_workflow_signal(row,"{}",digest("{}"),"b",&r,&e);});
  a.join();b.join();other.close();
  check((first==Result::Recorded&&second==Result::Conflict)||(second==Result::Recorded&&first==Result::Conflict),"race did not choose one receipt");

  workflow_signal_step(&db,task(db,"tamper"),spec(),&yielded);check(yielded,"tamper wait");
  sql(path,"UPDATE workflow_signals SET spec_json='{}' WHERE task_id='tamper'; UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='tamper';");
  output=workflow_signal_step(&db,task(db,"tamper"),spec(),&yielded);
  check(!yielded&&output["ok"]==false,"definition tamper resumed");
  workflow_signal_step(&db,task(db,"payload-tamper"),spec(),&yielded);
  check(db.get_workflow_signal("signals","payload-tamper",&row,&error),error);
  check(db.resolve_workflow_signal(row,payload,hash,"original",&saved,&error)==Result::Recorded,error);
  sql(path,"UPDATE workflow_signals SET payload_json='{}' WHERE task_id='payload-tamper'; UPDATE workflow_tasks SET status='running',attempt=1 WHERE task_id='payload-tamper';");
  output=workflow_signal_step(&db,task(db,"payload-tamper"),spec(),&yielded);
  check(!yielded&&output["ok"]==false&&output["retryable"]==false,"payload tamper resumed");

  workflow_signal_step(&db,task(db,"stop"),spec(),&yielded);
  check(yielded&&db.get_workflow_signal("signals","stop",&row,&error),error);
  sql(path,"UPDATE workflows SET cancel_requested=1 WHERE workflow_id='signals';");
  check(db.resolve_workflow_signal(row,payload,hash,"stop",&saved,&error)==Result::Stopped,"owner stop bypassed");
  check(db.get_workflow_signal("signals","input",&row,&error),error);
  check(db.resolve_workflow_signal(row,payload,hash,"delivery-1",&saved,&error)==Result::Replayed,"historical receipt lost");
  AgentDb::WorkflowRow stopped;check(db.get_workflow("signals",&stopped,&error)&&stopped.cancel_requested,"replay cleared stop");
  Json::Value request;request["kind"]="signal";
  check(workflow_budget_demand(request,DaemonConfig(),{10,10,10000,100})==AgentDb::WorkflowBudgetAmounts{},"signal reserved budget");
  db.close();
  {
    AgentdApi::Options opts;opts.cfg.db_path=(root/"facade.sqlite").string();opts.cfg.state_dir=(root/"facade").string();opts.cfg.auth_token="signal-facade-fixture";
    AgentdApi api(opts);check(api.init(&error),error);
    HttpRequest req;req.method="POST";req.path="/api/v1/workflow/signal";req.body="{}";HttpResponse resp;
    check(api.handle(req,&resp)&&resp.status==401,"facade signal auth absent");
    req.headers["authorization"]="Bearer signal-facade-fixture";resp={};
    check(api.handle(req,&resp)&&resp.status==400,"facade signal route absent");
  }
  std::filesystem::remove_all(root);
  std::cout<<"Signal migration, identity, wait/wake, restart, race, expiry, tamper, stop, budget and facade checks passed\n";
}
