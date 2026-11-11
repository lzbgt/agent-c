#include "agent_db.h"
#include "base64.h"
#include "agent/hmac_sha256.h"
#include "agent/json_c14n.h"
#include "edge_interop_endpoints.h"
#include "edge_util.h"
#include "workflow_edge_invoke.h"
#include "workflow_endpoints.h"
#include "workflow_engine.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace agentd;
namespace {
void check(bool good, const std::string &reason) {
  if (!good) {
    std::cerr << reason << '\n';
    std::exit(1);
  }
}
std::string encode(const Json::Value &v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}
int64_t count(const std::string &path, const char *sql) {
  sqlite3 *db = nullptr;
  check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open fixture");
  sqlite3_busy_timeout(db, 2000);
  sqlite3_stmt *s = nullptr;
  check(sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK,
        "query fixture");
  check(sqlite3_step(s) == SQLITE_ROW, "read fixture");
  auto n = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  sqlite3_close(db);
  return n;
}
void sql(const std::string &path, const char *command) {
  sqlite3 *db = nullptr;
  check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open trigger fixture");
  check(sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK,
        "fixture SQL failed");
  sqlite3_close(db);
}
struct Assignment {
  AgentDb::EdgeTaskRow task;
  AgentDb::EdgeOutboxMessageRow message;
  AgentDb::EdgeTaskEventRow event;
  Assignment(std::string id, std::string node = "node") {
    task.task_id = id;
    task.step_id = "action";
    task.node_id = node;
    task.idempotency_key = "key:" + id;
    task.mode = "invoke";
    task.tool_name = "actuator";
    task.payload_json = R"({"args":{"value":1},"tool":"actuator"})";
    task.state = "QUEUED";
    task.deadline_utc_ms = edge_unix_ms_now() + 60000;
    message.node_id = node;
    message.envelope_json = R"({"type":"TASK_ASSIGN"})";
    event.task_id = id;
    event.step_id = task.step_id;
    event.state = "QUEUED";
    event.data_json = "{}";
  }
  bool admit(AgentDb &db, int rate = 0, int cooldown = 0, int *code = nullptr,
             bool *duplicate = nullptr) {
    int status = 0;
    bool dedup = false;
    int64_t outbox = 0;
    std::string error;
    bool ok = db.admit_edge_task_assignment(
        task, message, event, rate, cooldown, &outbox, &dedup, &status, &error);
    if (code)
      *code = status;
    if (duplicate)
      *duplicate = dedup;
    if (ok)
      check(status == 200 && (dedup ? outbox == 0 : outbox > 0),
            "admission result identity");
    return ok;
  }
};
void concurrent(AgentDb &a, AgentDb &b, const std::string &prefix, int rate,
                bool locked, int expected) {
  std::atomic<bool> go{false};
  std::atomic<int> accepted{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; i++)
    workers.emplace_back([&, i] {
      while (!go.load())
        std::this_thread::yield();
      Assignment r(prefix + std::to_string(i), prefix);
      if (locked)
        r.task.resource_lock = "actuator-lock";
      int status = 0;
      if (r.admit(i % 2 ? a : b, rate, 0, &status))
        accepted++;
      else
        check(status == 429, "capacity failure was not explicit");
    });
  go.store(true);
  for (auto &worker : workers)
    worker.join();
  check(accepted == expected, "parallel capacity exceeded");
}

HttpResponse deliver_receipt(AgentDb& db, const DaemonConfig& cfg,
                             const AgentDb::EdgeTaskRow& assignment,
                             const std::string& source, const std::string& key,
                             Json::Value result, bool sign = true) {
  static int sequence = 0;
  Json::Value envelope;
  envelope["msg_id"] = "receipt-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence);
  envelope["type"] = "TASK_DONE";
  envelope["from"] = "node:" + source;
  envelope["to"] = "platform";
  envelope["ts_utc_ms"] = Json::Int64(edge_unix_ms_now());
  envelope["body"]["task_id"] = assignment.task_id;
  envelope["body"]["step_id"] = assignment.step_id;
  envelope["body"]["idempotency_key"] = key;
  envelope["body"]["result"] = result;
  // Attempt to smuggle a platform attestation decision. It must be discarded.
  envelope["body"]["_attest_sig_ok"] = true;
  if (sign) {
    envelope["auth"]["alg"] = "hmac-sha256";
    envelope["auth"]["kid"] = source;
    const auto bytes = encode(envelope);
    char* canonical = nullptr;
    size_t size = 0;
    char message[256]{};
    check(agent_json_c14n_canonicalize(bytes.data(),bytes.size(),&canonical,&size,message,sizeof(message)) == AGENT_OK,
          "cannot sign fixture");
    uint8_t mac[32];
    const auto secret = cfg.edge_auth_hmac_keys.at(source);
    agent_hmac_sha256(secret.data(),secret.size(),canonical,size,mac);
    agent_free(canonical);
    envelope["auth"]["sig"] = base64_encode(mac,sizeof(mac));
  }
  HttpRequest req;
  req.method = "POST";
  req.body = encode(envelope);
  HttpResponse resp;
  handle_edge_message_endpoint(cfg,CorsConfig{},&db,req,&resp);
  return resp;
}

void node(AgentDb &db, const std::string &id, int64_t seen) {
  AgentDb::EdgeNodeRow n;
  n.node_id = id;
  n.last_hello_utc_ms = seen;
  n.tools_json = R"(["actuator"])";
  n.tags_json = "[]";
  n.manifest_json =
      R"({"tools":[{"name":"actuator","side_effect_level":"low","parameters_schema":{"type":"object"}}]})";
  std::string error;
  check(db.upsert_edge_node(n, &error), error);
}
} // namespace
int main(int argc, char **argv) {
#if !defined(_WIN32)
  if (argc == 3 && std::string(argv[1]) == "--receipt-writer") {
    AgentDb db;
    std::string error;
    check(db.open(argv[2],&error),error);
    AgentDb::EdgeTaskRow assignment;
    check(db.get_edge_task("independent","action",&assignment,&error),error);
    DaemonConfig config;
    config.edge_auth_kid_policy = "match_node";
    config.edge_auth_hmac_keys["first"] = "isolated-test-first-key";
    Json::Value result;
    result["readback"] = 1;
    check(deliver_receipt(db,config,assignment,"first",assignment.idempotency_key,result).status==200,
          "child receipt ingestion failed");
    // Abrupt process death after receipt commit, before workflow recovery and
    // without a clean SQLite close, must not lose the evidence.
    _exit(0);
  }
#endif
  if (argc == 3 && std::string(argv[1]) == "--crash-writer") {
    AgentDb db;
    std::string error;
    if (!db.open(argv[2], &error))
      return 2;
    for (int i = 0; i < 20000; i++) {
      Assignment request("crash" + std::to_string(i));
      if (!request.admit(db, 20000))
        return 3;
    }
    return 0;
  }
  const auto root =
      std::filesystem::temp_directory_path() /
      ("agent-device-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path = (root / "state.sqlite").string();
  AgentDb db, second;
  std::string error;
  check(db.open(path, &error), error);
  check(second.open(path, &error), error);
  Assignment request("once");
  check(request.admit(db), "first admission");
  bool duplicate = false;
  check(request.admit(second, 0, 0, nullptr, &duplicate) && duplicate,
        "duplicate not observed");
  check(count(path, "SELECT COUNT(*) FROM edge_outbox_messages") == 1,
        "duplicate republished command");
  for (int field = 0; field < 4; field++) {
    auto changed = request;
    if (field == 0) {
      changed.task.node_id = "other";
      changed.message.node_id = "other";
    }
    if (field == 1)
      changed.task.idempotency_key = "different";
    if (field == 2)
      changed.task.payload_json = "{}";
    if (field == 3)
      changed.task.mode = "agent";
    int status = 0;
    check(!changed.admit(db, 0, 0, &status) && status == 409,
          "changed operation accepted");
  }
  auto changed = request.task;
  changed.node_id = "other";
  check(!db.upsert_edge_task(changed, &error),
        "upsert rewrote device identity");
  auto result = request.task;
  result.state = "SUCCEEDED";
  result.result_json = R"({"ok":true})";
  check(db.upsert_edge_task(result, &error), error);
  request.task.deadline_utc_ms = 1;
  check(request.admit(db, 0, 0, nullptr, &duplicate) && duplicate,
        "settled operation not reused");
  check(count(path, "SELECT COUNT(*) FROM edge_outbox_messages") == 1,
        "terminal replay published command");
  Assignment expired("expired");
  expired.task.deadline_utc_ms = 1;
  int status = 0;
  check(!expired.admit(db, 0, 0, &status) && status == 409,
        "expired command published");
  // A failure at the task/outbox boundary rolls back every resource and record.
  sql(path, "CREATE TRIGGER fail_outbox BEFORE INSERT ON edge_outbox_messages "
            "WHEN NEW.node_id='rollback' BEGIN SELECT RAISE(ABORT,'injected "
            "outbox failure'); END;");
  Assignment rollback("rollback", "rollback");
  check(!rollback.admit(db, 1), "injected failure ignored");
  check(count(path,
              "SELECT COUNT(*) FROM edge_tasks WHERE node_id='rollback'") == 0,
        "orphan task survived rollback");
  check(count(path, "SELECT COUNT(*) FROM edge_tool_rate_state WHERE "
                    "node_id='rollback'") == 0,
        "failed dispatch consumed rate capacity");
  sql(path, "DROP TRIGGER fail_outbox;");
  check(rollback.admit(db, 1), "rollback could not recover");
  concurrent(db, second, "rate", 2, false, 2);
  concurrent(db, second, "locked", 0, true, 1);
  check(count(path, "SELECT window_count FROM edge_tool_rate_state WHERE "
                    "node_id='rate'") == 2,
        "rate settlement mismatch");
  Assignment cooldown("cool1", "cool");
  check(cooldown.admit(db, 0, 10000), "cooldown first admission");
  cooldown.task.task_id = "cool2";
  cooldown.task.idempotency_key = "key:cool2";
  cooldown.event.task_id = "cool2";
  check(!cooldown.admit(db, 0, 10000, &status) && status == 429,
        "cooldown ignored");
  // A stop before admission cannot publish a device command.
  AgentDb::WorkflowRow wf;
  wf.workflow_id = "stopped";
  wf.status = "queued";
  wf.spec_json = "{}";
  wf.cancel_requested = true;
  AgentDb::WorkflowTaskRow task;
  task.workflow_id = wf.workflow_id;
  task.task_id = "action";
  task.status = "queued";
  task.request_json = "{}";
  task.depends_on_json = "[]";
  check(db.create_workflow(wf, {task}, &error), error);
  Assignment stopped(wf.workflow_id);
  check(!stopped.admit(db, 0, 0, &status) && status == 409,
        "stopped workflow dispatched");
  // Recovery pins node selection even when no scheduler result was persisted.
  wf.workflow_id = "workflow";
  wf.cancel_requested = false;
  task.workflow_id = wf.workflow_id;
  check(db.create_workflow(wf, {task}, &error), error);
  node(db, "first", 200);
  node(db, "second", 100);
  Json::Value rr;
  rr["edge"]["match_any"]["requires_tools"].append("actuator");
  rr["edge"]["tool"] = "actuator";
  rr["edge"]["args"]["value"] = 1;
  auto out = workflow_edge_invoke_to_json(&db, wf, task, rr);
  check(out["edge"]["node_id"] == "first" && out["retryable"] == true,
        "initial device selection failed");
  check(out["tool_calls_total"] == 1 && out["steps_executed"] == 1,
        "new device dispatch was not metered");
  AgentDb::EdgeTaskRow original;
  check(db.get_edge_task(wf.workflow_id, task.task_id, &original, &error),
        error);
  original.state = "SUCCEEDED";
  original.result_json = R"({"readback":1})";
  check(db.upsert_edge_task(original, &error), error);
  node(db, "second", 300);
  out = workflow_edge_invoke_to_json(&db, wf, task, rr);
  check(out["ok"] == true && out["edge"]["node_id"] == "first" &&
            out["edge"]["deduped"] == true,
        "recovery switched devices");
  check(out["tool_calls_total"] == 0 && out["steps_executed"] == 0,
        "device observation charged a second dispatch");
  original.state = "TIMED_OUT";
  check(db.upsert_edge_task(original, &error), error);
  out = workflow_edge_invoke_to_json(&db, wf, task, rr);
  check(out["needs_input"] == true &&
            out["blocker"]["kind"] == "device_outcome_unknown" &&
            out["retryable"] != true,
        "timeout retried uncertain device action");
  // The direct API also observes the pinned device after selection changes.
  HttpRequest direct;
  direct.method = "POST";
  Json::Value direct_args;
  direct_args["task_id"] = wf.workflow_id;
  direct_args["step_id"] = task.task_id;
  direct_args["idempotency_key"] = original.idempotency_key;
  direct_args["mode"] = "invoke";
  direct_args["deadline_utc_ms"] = Json::Int64(edge_unix_ms_now() + 60000);
  direct_args["match_any"] = rr["edge"]["match_any"];
  direct_args["payload"]["tool"] = "actuator";
  direct_args["payload"]["args"] = rr["edge"]["args"];
  direct.body = encode(direct_args);
  HttpResponse direct_response;
  handle_edge_task_assign_endpoint(DaemonConfig{}, CorsConfig{}, &db, direct,
                                   &direct_response);
  Json::Value direct_result;
  Json::CharReaderBuilder reader;
  std::istringstream direct_text(direct_response.body);
  check(Json::parseFromStream(reader, direct_text, &direct_result, &error) &&
            direct_response.status == 200 &&
            direct_result["node_id"] == "first" &&
            direct_result["deduped"] == true,
        "direct API reselected the device");
  db.close();
  second.close();
  // One uncertain device action must not prevent independent work with one
  // worker.
  {
    AgentDb engine_db;
    check(engine_db.open((root / "engine.sqlite").string(), &error), error);
    wf.workflow_id = "independent";
    wf.status = "queued";
    wf.cancel_requested = false;
    task.workflow_id = wf.workflow_id;
    task.task_id = "action";
    rr["kind"] = "edge_invoke";
    task.request_json = encode(rr);
    task.max_attempts = 10;
    auto independent = task;
    independent.task_id = "independent";
    independent.request_json = R"({"kind":"delay","delay_ms":0})";
    auto dependent = independent;
    dependent.task_id = "dependent";
    dependent.depends_on_json = R"(["action"])";
    check(engine_db.create_workflow(wf, {task, independent, dependent}, &error),
          error);
    node(engine_db, "first", 200);
    out = workflow_edge_invoke_to_json(&engine_db, wf, task, rr);
    check(engine_db.get_edge_task(wf.workflow_id, task.task_id, &original,
                                  &error),
          error);
    original.state = "TIMED_OUT";
    check(engine_db.upsert_edge_task(original, &error), error);
    DaemonConfig config;
    config.tools = "none";
    config.state_dir = root.string();
    WorkflowEngine::Options options;
    options.max_concurrency = 1;
    options.poll_ms = 10;
    WorkflowEngine engine(
        &engine_db, [&] { return config; },
        [](const DaemonConfig &) { return OpenAIClientConfig{}; }, nullptr,
        root.string(), options);
    check(engine.start(&error), error);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    AgentDb::WorkflowRow current;
    do {
      check(engine_db.get_workflow(wf.workflow_id, &current, &error), error);
      if (current.status == "error")
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    engine.stop();
    check(current.status == "error", "uncertain workflow never settled");
    std::vector<AgentDb::WorkflowTaskRow> settled;
    check(engine_db.list_workflow_tasks(wf.workflow_id, &settled, &error),
          error);
    for (const auto &row : settled) {
      if (row.task_id == "independent")
        check(row.status == "done", "device blocker stalled independent work");
      if (row.task_id == "dependent")
        check(row.status != "done", "dependent ran after unresolved effect");
    }
    HttpRequest request;
    request.method = "GET";
    request.query = "workflow_id=independent&include_tasks=1";
    HttpResponse response;
    handle_workflow_get_endpoint(config, CorsConfig{}, &engine_db, request,
                                 &response);
    Json::Value body;
    std::istringstream text(response.body);
    check(Json::parseFromStream(reader, text, &body, &error) &&
              response.status == 200,
          "workflow API failed");
    bool shown = false;
    for (const auto &row : body["tasks"])
      if (row["task_id"] == "action") {
        check(!row.isMember("result"), "task body leaked into metadata");
        shown = row["needs_input"] == true &&
                row["blocker"]["required_action"].isString();
      }
    check(shown, "required human action hidden without include_results");
    check(count((root / "engine.sqlite").string(),
                "SELECT COUNT(*) FROM edge_outbox_messages") == 1,
          "workflow recovery republished device command");

    // Recovery is readback-only: without a signed matching receipt, no task changes.
    HttpRequest reconcile;
    reconcile.method = "POST";
    reconcile.body = R"({"workflow_id":"independent","task_id":"action"})";
    const auto recover = [&]() {
      HttpResponse response;
      handle_workflow_reconcile_endpoint(config,CorsConfig{},&engine_db,reconcile,&response);
      return response;
    };
    check(recover().status == 409, "missing receipt allowed workflow recovery");
    config.edge_auth_kid_policy = "match_node";
    config.edge_auth_hmac_keys["first"] = "isolated-test-first-key";
    config.edge_auth_hmac_keys["second"] = "isolated-test-second-key";
    Json::Value result;
    result["readback"] = 1;
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,result,false).status == 200,
          "optional-auth legacy ingress changed");
    check(count((root/"engine.sqlite").string(),"SELECT COUNT(*) FROM edge_task_receipts") == 0,
          "unsigned late claim became a recovery receipt");
    check(deliver_receipt(engine_db,config,original,"second",original.idempotency_key,result).status == 200,
          "wrong-node observation rejected unexpectedly");
    check(deliver_receipt(engine_db,config,original,"first","wrong-operation",result).status == 200,
          "wrong-key observation rejected unexpectedly");
    AgentDb::EdgeNodeRow schema_node;
    check(engine_db.get_edge_node("first",&schema_node,&error),error);
    const auto original_manifest = schema_node.manifest_json;
    schema_node.manifest_json = R"({"tools":[{"name":"actuator","side_effect_level":"low","parameters_schema":{"type":"object"},"result_schema":{"type":"object","required":["readback"]}}]})";
    check(engine_db.upsert_edge_node(schema_node,&error),error);
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,result).status==200,
          "invalid schema observation failed");
    check(count((root/"engine.sqlite").string(),"SELECT COUNT(*) FROM edge_task_receipts")==0,
          "schema-invalid late result became a receipt");
    schema_node.manifest_json = original_manifest;
    check(engine_db.upsert_edge_node(schema_node,&error),error);
    config.edge_attest_require_sig = true;
    auto invalid = result;
    invalid["attest"]["kid"] = "first";
    invalid["attest"]["alg"] = "hmac-sha256";
    invalid["attest"]["sig"] = "invalid";
    invalid["attest"]["ts_utc_ms"] = Json::Int64(edge_unix_ms_now());
    invalid["attest"]["result_sha256"] = "sha256:" + std::string(64,'0');
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,invalid).status == 200,
          "invalid attestation observation failed");
    check(count((root/"engine.sqlite").string(),"SELECT COUNT(*) FROM edge_task_receipts") == 0,
          "wrong identity or forged attestation became a receipt");
    config.edge_attest_require_sig = false;
    auto incomplete_attest = result;
    incomplete_attest["attest"]["sig"] = "unverifiable";
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,incomplete_attest).status==200,
          "incomplete optional attestation observation failed");
    check(count((root/"engine.sqlite").string(),"SELECT COUNT(*) FROM edge_task_receipts")==0,
          "incomplete optional attestation became a trusted receipt");
#if !defined(_WIN32)
    const auto receipt_child = fork();
    check(receipt_child>=0,"fork receipt writer");
    if (receipt_child==0) {
      execl(argv[0],argv[0],"--receipt-writer",(root/"engine.sqlite").c_str(),static_cast<char*>(nullptr));
      _exit(4);
    }
    int receipt_status = 0;
    check(waitpid(receipt_child,&receipt_status,0)==receipt_child &&
          WIFEXITED(receipt_status) && WEXITSTATUS(receipt_status)==0,
          "abrupt receipt writer failed");
#else
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,result).status == 200,
          "signed late receipt was not accepted");
#endif
    check(count((root/"engine.sqlite").string(),"SELECT COUNT(*) FROM edge_task_receipts") == 1,
          "signed receipt not durable");
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,result).status == 200,
          "identical receipt not idempotent");
    result["readback"] = 2;
    check(deliver_receipt(engine_db,config,original,"first",original.idempotency_key,result).status == 409,
          "conflicting receipt replaced committed evidence");
    AgentDb::EdgeTaskRow receipt;
    check(engine_db.get_edge_task(wf.workflow_id,task.task_id,&original,&error) &&
          original.state == "TIMED_OUT" && original.result_json.empty(),
          "late receipt erased platform timeout or mutated platform result");
    check(engine_db.get_edge_task_receipt(original,&receipt,&error) &&
          receipt.result_json == R"({"readback":1})",
          "receipt result changed after conflict");
    engine_db.close();
    check(engine_db.open((root/"engine.sqlite").string(),&error),error);
    check(engine_db.get_edge_task_receipt(original,&receipt,&error),"receipt lost on restart");
    check(engine_db.get_workflow(wf.workflow_id,&current,&error),error);
    current.cancel_requested = true;
    check(engine_db.upsert_workflow(current,&error),error);
    check(recover().status == 409,"receipt recovery cleared owner cancellation");
    // Cancellation is sticky in production. Reset only this isolated fixture
    // to test the independent deadline and successful-recovery cases.
    sql((root/"engine.sqlite").string(),"UPDATE workflows SET cancel_requested=0 WHERE workflow_id=\'independent\';");
    current.cancel_requested = false;
    current.deadline_unix_ms = edge_unix_ms_now()-1;
    check(engine_db.upsert_workflow(current,&error),error);
    check(recover().status == 409,"receipt recovery bypassed workflow deadline");
    current.deadline_unix_ms = edge_unix_ms_now()+60000;
    check(engine_db.upsert_workflow(current,&error),error);
    reconcile.body = R"({"workflow_id":"independent","task_id":"action","result":{"ok":true}})";
    check(recover().status == 400,"caller supplied outcome accepted");
    reconcile.body = R"({"workflow_id":"independent","task_id":"action","reconcile_url":"https://example.test"})";
    check(recover().status == 400,"arbitrary device resolver URL accepted");
    reconcile.body = R"({"workflow_id":"independent","task_id":"action"})";
    // Roll back the workflow change if task update or evidence checks fail.
    sql((root/"engine.sqlite").string(),
        "CREATE TRIGGER refuse_recovery BEFORE UPDATE ON workflow_tasks WHEN NEW.status='queued' BEGIN SELECT RAISE(ABORT,'test'); END;");
    check(recover().status == 409,"failed transaction claimed recovery");
    check(engine_db.get_workflow(wf.workflow_id,&current,&error) && current.status=="error",
          "failed recovery partially requeued workflow");
    sql((root/"engine.sqlite").string(),"DROP TRIGGER refuse_recovery;");
    const auto resumed = recover();
    check(resumed.status == 200,"verified receipt did not requeue workflow: " + resumed.body);
    check(recover().status == 409,"duplicate active recovery accepted");
    check(engine.start(&error),error);
    const auto recovered_by = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do {
      check(engine_db.get_workflow(wf.workflow_id,&current,&error),error);
      if (current.status=="done") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now()<recovered_by);
    engine.stop();
    check(current.status=="done","receipt recovery did not release dependent work");
    check(engine_db.list_workflow_tasks(wf.workflow_id,&settled,&error),error);
    for (const auto& row : settled) {
      check(row.status=="done","recovered workflow retained failed/pending task");
      if (row.task_id=="independent") check(row.attempt==1,"completed independent work replayed");
      if (row.task_id=="action") {
        Json::Value value;
        std::istringstream input(row.result_json);
        check(Json::parseFromStream(reader,input,&value,&error) &&
              value["edge_receipt_reconciled"]==true &&
              value["edge_platform_state"]=="TIMED_OUT" &&
              value["edge"]["node_id"]=="first" && value["tool_calls_total"]==0,
              "recovered outcome lost provenance or charged another command");
      }
    }
    check(count((root/"engine.sqlite").string(),"SELECT COUNT(*) FROM edge_outbox_messages")==1,
          "receipt recovery republished a device command");

  }
#if !defined(_WIN32)
  // Kill a real writer during repeated transactions. WAL recovery must expose
  // all assignment records together; a scheduler retry cannot fill a crash gap.
  const auto crash = (root / "crash.sqlite").string();
  {
    AgentDb init;
    check(init.open(crash, &error), error);
  }
  const pid_t child = fork();
  check(child >= 0, "fork writer");
  if (child == 0) {
    execl(argv[0], argv[0], "--crash-writer", crash.c_str(),
          static_cast<char *>(nullptr));
    _exit(4);
  }
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  bool wrote = false;
  while (std::chrono::steady_clock::now() < until) {
    if (count(crash, "SELECT COUNT(*) FROM edge_tasks") >= 20) {
      wrote = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  kill(child, SIGKILL);
  int child_status = 0;
  waitpid(child, &child_status, 0);
  check(wrote && WIFSIGNALED(child_status), "crash writer did not execute");
  {
    AgentDb recovered;
    check(recovered.open(crash, &error), error);
  }
  const auto rows = count(crash, "SELECT COUNT(*) FROM edge_tasks");
  check(rows == count(crash, "SELECT COUNT(*) FROM edge_outbox_messages") &&
            rows == count(crash, "SELECT COUNT(*) FROM edge_task_events") &&
            rows == count(crash,
                          "SELECT SUM(window_count) FROM edge_tool_rate_state"),
        "crash produced partial device admission");
#endif
  std::filesystem::remove_all(root);
  std::cout << "device assignment tests passed\n";
  return 0;
}
