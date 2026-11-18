"""Real conditional action effects, skip receipts and pinned SOPs. Isolated, provider-free."""
import copy
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

binary = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def wait_for(fn, message):
    end = time.monotonic() + 12
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(.025)
    raise AssertionError(message)

def when(value=True, pointer="/payload/accepted", source="source"):
    return {"task_id": source, "pointer": pointer, "equals": value}

def seed(value=True):
    return {"task_id": "source", "kind": "delay", "delay_ms": 0, "result": {"payload": {"accepted": value}}}

class Daemon:
    def __init__(self, allowed=True):
        self.temp = tempfile.TemporaryDirectory(prefix="workflow-condition-")
        self.root = Path(self.temp.name)
        self.db = self.root / "state.db"
        self.log = (self.root / "daemon.log").open("w+")
        self.allowed = allowed
        self.process = None
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.start()

    def start(self):
        env = {k:v for k,v in os.environ.items() if k.lower() not in ("http_proxy","https_proxy","all_proxy")}
        env["AGENTD_DOTENV_PATH"] = "/dev/null"
        self.process = subprocess.Popen([binary, "--host", "127.0.0.1", "--port", str(self.port),
            "--tools", "host", "--yolo" if self.allowed else "--no-yolo", "--auth-token", "condition-fixture",
            "--state-dir", str(self.root/"state"), "--db-path", str(self.db),
            "--workflow-poll-ms", "10", "--workflow-concurrency", "1"],
            env=env, cwd=self.root, stdout=self.log, stderr=self.log)
        wait_for(lambda: self.process.poll() is None and self.call("/api/v1/health").get("ok"), "daemon startup")

    def stop(self, crash=False):
        if self.process:
            self.process.kill() if crash else self.process.terminate()
            self.process.wait(timeout=10)
            self.process = None

    def close(self):
        self.stop()
        self.log.close()
        self.temp.cleanup()

    def call(self, path, body=None, expected=200):
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}"+path,
            data=None if body is None else json.dumps(body).encode(),
            headers={"Authorization":"Bearer condition-fixture","Content-Type":"application/json"})
        try:
            response = opener.open(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            value = json.load(response)
            assert response.code == expected, (path, response.code, expected, value)
            return value

    def submit(self, tasks, **extra):
        return self.call("/api/v1/workflow/submit", {"tasks":tasks, **extra})["workflow_id"]

    def detail(self, wid, results=True):
        return self.call("/api/v1/workflow?workflow_id="+wid+"&include_tasks=1"+("&include_results=1" if results else ""))

    def terminal(self, wid, status="done"):
        result = wait_for(lambda: (d if d["workflow"]["status"] in ("done","error","cancelled") else None)
            if (d:=self.detail(wid)) else None, "workflow completion")
        assert result["workflow"]["status"] == status, result
        return result

    def count(self, table, wid, task=None):
        with sqlite3.connect(self.db) as conn:
            sql = "SELECT count(*) FROM "+table+" WHERE workflow_id=?"
            args = [wid]
            if task is not None:
                sql += " AND task_id=?"
                args.append(task)
            return conn.execute(sql,args).fetchone()[0]

    def host(self, task, expected=True, pointer="/payload/accepted"):
        marker = str(self.root/"effects.txt")
        return {"task_id":task, "kind":"host_tool", "max_attempts":4, "when":when(expected,pointer),
            "host_tool":{"name":"proc_exec","timeout_ms":3000,
            "arguments":{"argv":[sys.executable,"-c",
                "from pathlib import Path; p=Path("+repr(marker)+"); p.open('a').write("+repr(task+"\n")+")"]}}}

    def effects(self):
        p = self.root/"effects.txt"
        return p.read_text().splitlines() if p.exists() else []

def authored(source):
    action = next(a for s in source["sop"]["stages"] for a in s["actions"] if "when" in a)
    assert action["when"] == when(False) and action["max_attempts"] == 3
    daemon = Daemon(False)
    try:
        saved = daemon.call("/api/v1/workflow/sops", {"sop":source["sop"]}, expected=201)["definition"]
        with sqlite3.connect(daemon.db) as conn:
            assert conn.execute("SELECT count(*) FROM workflows").fetchone()[0] == 0
        wid = daemon.call("/api/v1/workflow/submit",
            {"sop_ref":{k:saved[k] for k in ("id","version","definition_hash")}})["workflow_id"]
        result = daemon.terminal(wid)
        out = result["result"]["results_by_task"][action["task_id"]]
        assert out["skipped"] and out["condition"]["matched"] is False
        assert out["tool_calls_total"] == 0 and daemon.count("workflow_effects",wid) == 0
        meta = next(t for t in daemon.detail(wid,False)["tasks"] if t["task_id"] == action["task_id"])
        assert meta["skipped"] and "actual" not in meta["condition"]
    finally:
        daemon.close()
    print("PASS exact browser-authored conditional SOP pinned and skipped without tool approval")

def full():
    daemon = Daemon()
    try:
        for invalid in [when(1), when([], "/payload"), when(True,""), when(True,"/bad~2"),
                        {**when(),"actor":"owner"}, when(True,source="missing"), when(True,source="guarded")]:
            daemon.call("/api/v1/workflow/submit",
                {"tasks":[seed(), {"task_id":"guarded","kind":"delay","when":invalid}]},expected=400)
        daemon.call("/api/v1/workflow/submit", {"defaults":{"when":when()}, "tasks":[seed()]}, expected=400)
        daemon.call("/api/v1/workflow/submit", {"tasks":[{"task_id":"bad","kind":{},"when":when()}]}, expected=400)
        daemon.call("/api/v1/workflow/submit", {"tasks":[seed(),{"task_id":"guarded","kind":"llm",
            "request":{"prompt":"never","tools":"none","when":when()}}]}, expected=400)
        for kind in ("edge_parallel","agentd_parallel","delegate_parallel"):
            daemon.call("/api/v1/workflow/submit", {"tasks":[{"task_id":"guarded","kind":kind,"when":when()}]}, expected=400)
        daemon.call("/api/v1/workflow/submit", {"tasks":[{"task_id":"a","kind":"delay","when":when(source="b")},
            {"task_id":"b","kind":"delay","when":when(source="a")}]}, expected=400)
        denied = daemon.host("guarded"); denied["allow_error"] = True
        daemon.call("/api/v1/workflow/submit", {"tasks":[seed(),denied]}, expected=400)

        tasks = [seed(), daemon.host("chosen"), daemon.host("opposite",False),
            {"task_id":"model","kind":"llm","when":when(False),
             "request":{"prompt":"${task.missing.json:/not-expanded}","tools":"none"}},
            {"task_id":"wait","kind":"delay","delay_ms":600000,"when":when(False),
             "expect":{"json_pointer_exists":["/not-present-on-skip"]}},
            {"task_id":"signal","kind":"signal","signal":{"name":"never"},"when":when(False)},
            {"task_id":"decision","kind":"decision","when":when(False),
             "decision":{"prompt":"Never ask","choices":[{"id":"yes","label":"Yes","outcome":"continue"}]}},
            {"task_id":"join","kind":"delay","depends_on":["chosen","opposite","model","wait","signal","decision"]}]
        wid = daemon.submit(tasks, workflow_limits={"max_tool_calls_total":1,"max_steps_total":1,
            "max_elapsed_ms_total":5000,"max_total_tokens":1})
        result = daemon.terminal(wid)
        assert daemon.effects() == ["chosen"], daemon.effects()
        for name in ("opposite","model","wait","signal","decision"):
            out = result["result"]["results_by_task"][name]
            assert out["skipped"] and out["condition"]["actual"] is True and not out["condition"]["matched"], out
            for cost in ("tool_calls_total","steps_executed","elapsed_ms","total_tokens"):
                assert out[cost] == 0, (name,cost,out)
            assert daemon.count("workflow_budget_reservations",wid,name) == 0
        assert daemon.count("workflow_effects",wid) == 1
        assert daemon.count("workflow_signals",wid) == 0
        assert daemon.count("workflow_decisions",wid) == 0
        assert daemon.count("workflow_delay_timers",wid,"wait") == 0
        metadata = daemon.detail(wid,False)
        assert all("result" not in t for t in metadata["tasks"])
        assert next(t for t in metadata["tasks"] if t["task_id"]=="opposite")["skipped"] is True
        daemon.stop(True); daemon.start()
        assert daemon.terminal(wid)["result"] == result["result"] and daemon.effects() == ["chosen"]

        for value,pointer in [(True,"/missing"),("true","/payload/accepted"),(42,"/payload/accepted")]:
            broken = daemon.host("must-not-run",True,pointer)
            wid = daemon.submit([seed(value),broken,{"task_id":"independent","kind":"delay"}])
            result = daemon.terminal(wid,"error")
            failed = next(t for t in result["tasks"] if t["task_id"]=="must-not-run")
            assert failed["attempt"] == 1 and daemon.count("workflow_effects",wid) == 0
            assert next(t for t in result["tasks"] if t["task_id"]=="independent")["status"] == "done"
        assert daemon.effects() == ["chosen"]

        # A false condition does not provide synthetic result fields to later conditions.
        skipped = {"task_id":"optional","kind":"delay","when":when(False)}
        downstream = daemon.host("after-skipped"); downstream["when"] = when(True,"/ok","optional")
        daemon.terminal(daemon.submit([seed(),skipped,downstream]),"error")
        assert daemon.effects() == ["chosen"]

        # Activated signal and its downstream conditions survive a process kill.
        signal = {"task_id":"source","kind":"signal","signal":{"name":"inspection.finished"}}
        wid = daemon.submit([signal,daemon.host("yes"),daemon.host("no",False)])
        wait_for(lambda: daemon.detail(wid)["signals"],"signal activation")
        daemon.stop(True); daemon.start()
        row = wait_for(lambda: next((s for s in daemon.detail(wid)["signals"] if s["available"]),None),"restored signal")
        daemon.call("/api/v1/workflow/signal",{"workflow_id":wid,"task_id":"source","signal_id":row["signal_id"],
            "idempotency_key":"inspection-result","payload":{"accepted":False}})
        daemon.terminal(wid)
        assert daemon.effects() == ["chosen","no"]
        daemon.stop(True); daemon.start()
        daemon.terminal(wid)
        assert daemon.effects() == ["chosen","no"]

        sop = {"schema":"agentd.sop.v1","id":"conditional-repeat","version":"1","stages":[
            {"id":"check","repeat_count":2,"actions":[seed(), {"task_id":"optional","kind":"delay","when":when()},
                {"task_id":"excluded","kind":"signal","when":when(False),"signal":{"name":"never"}}]},
            {"id":"last","actions":[{"task_id":"last","kind":"delay","when":when()}]}]}
        saved = daemon.call("/api/v1/workflow/sops",{"sop":sop},expected=201)["definition"]
        wid = daemon.call("/api/v1/workflow/submit",{"sop_ref":{k:saved[k] for k in ("id","version","definition_hash")}})["workflow_id"]
        result = daemon.terminal(wid)
        for iteration in (1,2):
            out = result["result"]["results_by_task"][f"optional:repeat:{iteration}"]
            assert out["condition"]["test"]["task_id"] == f"source:repeat:{iteration}" and out["condition"]["matched"]
            assert result["result"]["results_by_task"][f"excluded:repeat:{iteration}"]["skipped"]
        assert result["result"]["results_by_task"]["last"]["condition"]["test"]["task_id"] == "source:repeat:2"

        wid = daemon.submit([signal,daemon.host("cancelled")])
        wait_for(lambda: daemon.detail(wid)["signals"],"cancel signal activation")
        daemon.call("/api/v1/workflow/cancel",{"workflow_id":wid})
        daemon.terminal(wid,"cancelled")
        daemon.stop(True); daemon.start()
        assert daemon.count("workflow_effects",wid) == 0 and daemon.effects() == ["chosen","no"]
    except Exception:
        daemon.log.flush()
        print((daemon.root/"daemon.log").read_text()[-4000:],file=sys.stderr)
        raise
    finally:
        daemon.close()
    denied = Daemon(False)
    try:
        wid = denied.submit([seed(),denied.host("not-approved")])
        denied.terminal(wid,"error")
        assert denied.count("workflow_effects",wid) == 0 and denied.effects() == []
        wid = denied.submit([seed(),denied.host("not-selected",False)])
        assert denied.terminal(wid)["result"]["results_by_task"]["not-selected"]["skipped"]
    finally:
        denied.close()
    print("PASS conditional effects, zero-cost skips, restart/stop, strict source errors, pinned repeated SOP and unchanged host policy")

if "--authored" in sys.argv:
    authored(json.load(sys.stdin))
else:
    full()
