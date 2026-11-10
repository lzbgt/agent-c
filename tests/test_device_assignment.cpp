#include "agent_db.h"
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
