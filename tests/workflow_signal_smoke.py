"""Real authenticated signal delivery, immutable receipts and worker-free waits. No providers."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

agentd = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
token = "signal-smoke-fixture"

def wait_for(fn, message, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, KeyError, urllib.error.URLError):
            pass
        time.sleep(.04)
    raise AssertionError(message)

def signal(timeout=0):
    return {"task_id":"input", "kind":"signal", "max_attempts":4,
            "signal":{"name":"inspection.finished", "title":"Inspection result", "timeout_ms":timeout}}

with tempfile.TemporaryDirectory(prefix="workflow-signal-") as temp:
    root=Path(temp); database=root/"state.sqlite"; log=(root/"daemon.log").open("w+")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1",0)); port=sock.getsockname()[1]
    base=f"http://127.0.0.1:{port}"; process=None

    def call(path, payload=None, expected=200, auth=True, with_status=False):
        headers={"Content-Type":"application/json"}
        if auth: headers["Authorization"]="Bearer "+token
        try:
            response=opener.open(urllib.request.Request(base+path, data=None if payload is None else json.dumps(payload).encode(),headers=headers),timeout=5)
        except urllib.error.HTTPError as exc: response=exc
        with response:
            body=json.load(response);allowed=expected if isinstance(expected,tuple) else (expected,)
            assert response.code in allowed,(path,response.code,expected,body)
            return (response.code,body) if with_status else body

    def start():
        global process
        process=subprocess.Popen([agentd,"--host","127.0.0.1","--port",str(port),"--auth-token",token,
            "--db-path",str(database),"--state-dir",str(root/"state"),"--workflow-concurrency","1"],
            cwd=root,stdout=log,stderr=log)
        wait_for(lambda: process.poll() is None and call("/api/v1/health").get("ok"),"daemon startup")

    def stop(crash=False):
        global process
        if process:
            process.kill() if crash else process.terminate();process.wait(timeout=10);process=None

    def detail(wid,suffix="&include_tasks=1&include_results=1"):
        return call("/api/v1/workflow?workflow_id="+wid+suffix)

    def submit(timeout=0,**options):
        return call("/api/v1/workflow/submit",{"tasks":[signal(timeout),
            {"task_id":"independent","kind":"delay","delay_ms":0},
            {"task_id":"after","kind":"delay","delay_ms":0,"depends_on":["input"]}],**options})["workflow_id"]

    def waiting(wid):
        return wait_for(lambda: next((s for s in detail(wid)["signals"] if s["available"]),None),"signal activation")

    def send(row,payload=None,key="delivery-1"):
        return {"workflow_id":row["workflow_id"],"task_id":row["task_id"],"signal_id":row["signal_id"],
                "idempotency_key":key,"payload":payload if payload is not None else {"accepted":True,"count":3}}

    def terminal(wid,status):
        return wait_for(lambda: (d if d["workflow"]["status"]==status else None) if (d:=detail(wid)) else None,"terminal "+status)

    try:
        start()
        call("/api/v1/workflow/signal",{},expected=401,auth=False)
        for body in ({},[],{"payload":"x"*33000}):
            call("/api/v1/workflow/signal",body,expected=400)
        for field,value in [("name",""),("name","bad/name"),("timeout_ms",-1),("timeout_ms",2592000001),("actor","owner")]:
            task=signal();task["signal"][field]=value
            call("/api/v1/workflow/submit",{"tasks":[task]},expected=400)
        task=signal();task["allow_error"]=True
        call("/api/v1/workflow/submit",{"tasks":[task]},expected=400)

        wid=submit(workflow_limits={"max_tool_calls_total":1,"max_steps_total":1,"max_elapsed_ms_total":2000,"max_total_tokens":1})
        row=waiting(wid)
        wait_for(lambda: next(t for t in detail(wid)["tasks"] if t["task_id"]=="independent")["status"]=="done","single worker blocked")
        other=call("/api/v1/workflow/submit",{"tasks":[{"task_id":"other","kind":"delay","delay_ms":0}]})["workflow_id"]
        terminal(other,"done")
        state=detail(wid);task=next(t for t in state["tasks"] if t["task_id"]=="input")
        assert task["status"]=="queued" and task["attempt"]==0
        assert detail(wid,"&include_tasks=0")["signals"][0]==row and "payload" not in row
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT count(*) FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='input'",(wid,)).fetchone()[0]==0
        stop(crash=True);start();assert waiting(wid)==row
        for body in ({**send(row),"actor":"owner"},{**send(row),"payload":[]},{**send(row),"payload":{"too_big":"x"*16384}}):
            call("/api/v1/workflow/signal",body,expected=400)
        call("/api/v1/workflow/signal",{**send(row),"signal_id":"wrong"},expected=409)
        call("/api/v1/workflow/signal",{**send(row),"workflow_id":"missing"},expected=404)
        receipt=call("/api/v1/workflow/signal",send(row))
        assert receipt["replayed"] is False and receipt["signal"]["payload_hash"] and "payload" not in receipt["signal"]
        done=terminal(wid,"done");result=next(t for t in done["tasks"] if t["task_id"]=="input")
        assert result["attempt"]==1 and result["result"]["payload"]=={"accepted":True,"count":3}
        assert result["result"]["elapsed_ms"]==0
        assert call("/api/v1/workflow/signal",send(row,{"count":3,"accepted":True}))["replayed"] is True
        call("/api/v1/workflow/signal",send(row,{},key="delivery-1"),expected=409)
        call("/api/v1/workflow/signal",send(row,key="new-delivery"),expected=409)
        stop(crash=True);start()
        assert call("/api/v1/workflow/signal",send(row))["replayed"] is True
        assert detail(wid)["workflow"]["status"]=="done"

        race=submit();race_row=waiting(race)
        def deliver(value):
            return call("/api/v1/workflow/signal",send(race_row,{"winner":value},key=value),expected=(200,409),with_status=True)
        with ThreadPoolExecutor(max_workers=2) as pool:receipts=list(pool.map(deliver,["a","b"]))
        assert sorted(code for code,_ in receipts)==[200,409]
        race_done=terminal(race,"done")
        winning_hash=next(body for code,body in receipts if code==200)["signal"]["payload_hash"]
        assert next(t for t in race_done["tasks"] if t["task_id"]=="input")["result"]["payload_hash"]==winning_hash

        timed_task=signal(900);timed_task["depends_on"]=["prepare"]
        timed=call("/api/v1/workflow/submit",{"tasks":[{"task_id":"prepare","kind":"delay","delay_ms":200},timed_task]})["workflow_id"]
        timed_row=waiting(timed)
        assert timed_row["expires_unix_ms"]-timed_row["created_unix_ms"]==900
        assert timed_row["created_unix_ms"]>=next(t for t in detail(timed)["tasks"] if t["task_id"]=="prepare")["finished_unix_ms"]
        assert terminal(timed,"error")["signals"][0]["state"]=="expired"
        call("/api/v1/workflow/signal",send(timed_row),expected=409)
        cancelled=submit();cancel_row=waiting(cancelled)
        call("/api/v1/workflow/cancel",{"workflow_id":cancelled})
        call("/api/v1/workflow/signal",send(cancel_row),expected=409)
        assert terminal(cancelled,"cancelled")["signals"][0]["state"]=="stopped"
        deadline=submit(deadline_unix_ms=int(time.time()*1000)+1200);deadline_row=waiting(deadline)
        terminal(deadline,"cancelled");call("/api/v1/workflow/signal",send(deadline_row),expected=409)
        stop(crash=True);start()
        assert detail(cancelled)["workflow"]["cancel_requested"] is True
        assert detail(cancelled)["signals"][0]["available"] is False

        sop={"schema":"agentd.sop.v1","id":"signal-process","version":"1","stages":[
            {"id":"receive","repeat_count":2,"actions":[signal()]},
            {"id":"finish","actions":[{"task_id":"finish","kind":"delay","delay_ms":0}]}]}
        saved=call("/api/v1/workflow/sops",{"sop":sop},expected=201)
        run=call("/api/v1/workflow/submit",{"sop_ref":{"id":"signal-process","version":"1"}})["workflow_id"]
        first=waiting(run);assert first["task_id"]=="input:repeat:1"
        call("/api/v1/workflow/signal",send(first))
        second=waiting(run);assert second["task_id"]=="input:repeat:2" and second["signal_id"]!=first["signal_id"]
        assert call("/api/v1/workflow/signal",send(first))["replayed"] is True
        assert next(t for t in detail(run)["tasks"] if t["task_id"]==second["task_id"])["status"]=="queued"
        call("/api/v1/workflow/signal",send(second,{"second":True}))
        terminal(run,"done")
        assert detail(run,"&include_spec=1")["spec"]["sop"]["version"]=="1"

        # Opt-in mailboxes exist in the same submit transaction, before dependency activation.
        gate=signal();gate["task_id"]="gate"
        early=signal(100);early["signal"]["accept_before_activation"]=True;early["depends_on"]=["gate"]
        body={"tasks":[gate,early,{"task_id":"free","kind":"delay","delay_ms":0}],
              "idempotency_key":"early-signal-submit"}
        mailbox=call("/api/v1/workflow/submit",body)["workflow_id"]
        assert call("/api/v1/workflow/submit",body)["workflow_id"]==mailbox
        pending=next(s for s in detail(mailbox)["signals"] if s["task_id"]=="input")
        assert pending["state"]=="pending" and pending["available"] and not pending.get("activated_unix_ms")
        assert not pending.get("expires_unix_ms") and "payload" not in pending
        gate_row=wait_for(lambda: next((s for s in detail(mailbox)["signals"] if s["task_id"]=="gate"),None),"gate")
        wait_for(lambda: next(t for t in detail(mailbox)["tasks"] if t["task_id"]=="free")["status"]=="done","early mailbox held worker")
        with sqlite3.connect(database) as db:
            before=db.execute("SELECT status,attempt,ready_unix_ms FROM workflow_tasks WHERE workflow_id=? AND task_id='input'",(mailbox,)).fetchone()
        early_receipt=call("/api/v1/workflow/signal",send(pending))
        assert early_receipt["signal"]["state"]=="buffered" and not early_receipt["replayed"]
        time.sleep(.2) # Waiting for dependencies does not start activation-time expiry.
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT status,attempt,ready_unix_ms FROM workflow_tasks WHERE workflow_id=? AND task_id='input'",(mailbox,)).fetchone()==before
            assert db.execute("SELECT count(*) FROM workflow_budget_reservations WHERE workflow_id=? AND task_id='input'",(mailbox,)).fetchone()[0]==0
            assert db.execute("SELECT count(*) FROM workflow_signals WHERE workflow_id=? AND task_id='input'",(mailbox,)).fetchone()[0]==1
        stop(crash=True);start()
        assert next(s for s in detail(mailbox)["signals"] if s["task_id"]=="input")["state"]=="buffered"
        assert call("/api/v1/workflow/signal",send(pending))["replayed"]
        call("/api/v1/workflow/signal",send(pending,key="replacement"),expected=409)
        call("/api/v1/workflow/signal",send(gate_row))
        completed=terminal(mailbox,"done")
        consumed=next(t for t in completed["tasks"] if t["task_id"]=="input")
        assert consumed["attempt"]==1 and consumed["result"]["payload"]=={"accepted":True,"count":3}
        final_signal=next(s for s in completed["signals"] if s["task_id"]=="input")
        assert final_signal["state"]=="resolved" and final_signal["activated_unix_ms"]>=pending["created_unix_ms"]

        def gated(early=True,timeout=0,condition=None,gate_timeout=0,**options):
            first=signal(gate_timeout);first["task_id"]="gate"
            last=signal(timeout);last["depends_on"]=["gate"]
            if early:last["signal"]["accept_before_activation"]=True
            if condition:last["when"]=condition
            wid=call("/api/v1/workflow/submit",{"tasks":[first,last],**options})["workflow_id"]
            gate_row=wait_for(lambda: next((s for s in detail(wid)["signals"] if s["task_id"]=="gate"),None),"gate activation")
            return wid,gate_row,next((s for s in detail(wid)["signals"] if s["task_id"]=="input"),None)

        # Default behavior stays activated-only; names or fabricated identities cannot route early input.
        default,default_gate,missing=gated(early=False)
        assert missing is None
        call("/api/v1/workflow/signal",{**send(default_gate),"task_id":"input"},expected=404)
        call("/api/v1/workflow/cancel",{"workflow_id":default});terminal(default,"cancelled")
        for value in ("true",1,None):
            invalid=signal();invalid["signal"]["accept_before_activation"]=value
            call("/api/v1/workflow/submit",{"tasks":[invalid]},expected=400)

        # Expiry starts only on activation, and survives restart without being extended.
        late,late_gate,late_row=gated(timeout=350)
        time.sleep(.4)
        assert next(s for s in detail(late)["signals"] if s["task_id"]=="input")["state"]=="pending"
        call("/api/v1/workflow/signal",send(late_gate))
        armed=wait_for(lambda: next((s for s in detail(late)["signals"] if s["task_id"]=="input" and s["state"]=="waiting"),None),"mailbox activation")
        assert armed["expires_unix_ms"]-armed["activated_unix_ms"]==350
        assert armed["activated_unix_ms"]>armed["created_unix_ms"]
        stop(crash=True);start()
        expired=terminal(late,"error")
        assert next(s for s in expired["signals"] if s["task_id"]=="input")["expires_unix_ms"]==armed["expires_unix_ms"]
        call("/api/v1/workflow/signal",send(late_row),expected=409)

        # One immutable early winner, with no readiness mutation or premature claim.
        raced,raced_gate,raced_row=gated()
        def early_deliver(value):
            return call("/api/v1/workflow/signal",send(raced_row,{"winner":value},key=value),expected=(200,409),with_status=True)
        with ThreadPoolExecutor(max_workers=2) as pool:early_receipts=list(pool.map(early_deliver,["a","b"]))
        assert sorted(code for code,_ in early_receipts)==[200,409]
        assert next(t for t in detail(raced)["tasks"] if t["task_id"]=="input")["attempt"]==0
        call("/api/v1/workflow/signal",send(raced_gate));terminal(raced,"done")

        # Receipt acceptance does not imply activation: conditions, failed dependencies,
        # deadline and owner cancellation can leave saved input permanently unused.
        skipped,skip_gate,skip_row=gated(condition={"task_id":"gate","pointer":"/payload/accepted","equals":True})
        call("/api/v1/workflow/signal",send(skip_row))
        call("/api/v1/workflow/signal",send(skip_gate,{"accepted":False}))
        skipped_done=terminal(skipped,"done")
        skip_task=next(t for t in skipped_done["tasks"] if t["task_id"]=="input")
        assert skip_task["result"]["skipped"] and "payload" not in skip_task["result"]
        unused=next(s for s in skipped_done["signals"] if s["task_id"]=="input")
        assert unused["state"]=="unused" and unused["payload_hash"] and not unused["available"]
        assert not unused.get("activated_unix_ms")
        assert call("/api/v1/workflow/signal",send(skip_row))["replayed"]
        for buffered in (False,True):
            stopped,stopped_gate,stopped_row=gated()
            if buffered:call("/api/v1/workflow/signal",send(stopped_row))
            call("/api/v1/workflow/cancel",{"workflow_id":stopped})
            stopped_done=terminal(stopped,"cancelled")
            if buffered:assert call("/api/v1/workflow/signal",send(stopped_row))["replayed"]
            else:call("/api/v1/workflow/signal",send(stopped_row),expected=409)
            assert next(s for s in stopped_done["signals"] if s["task_id"]=="input")["state"]=="unused"
            assert stopped_done["workflow"]["cancel_requested"]
            assert next(t for t in stopped_done["tasks"] if t["task_id"]=="input")["attempt"]==0
        for cause in ("dependency","deadline"):
            options={"gate_timeout":200} if cause=="dependency" else {"deadline_unix_ms":int(time.time()*1000)+300}
            failed,failed_gate,failed_row=gated(**options)
            call("/api/v1/workflow/signal",send(failed_row))
            failed_done=terminal(failed,"error" if cause=="dependency" else "cancelled")
            assert next(s for s in failed_done["signals"] if s["task_id"]=="input")["state"]=="unused"
            assert next(t for t in failed_done["tasks"] if t["task_id"]=="input")["attempt"]==0
            assert call("/api/v1/workflow/signal",send(failed_row))["replayed"]

        # Future repetitions have distinct submitted slots; delivering the second first
        # cannot release the first or advance the sequential SOP.
        sop["id"]="early-signal-process";sop["stages"][0]["actions"][0]["signal"]["accept_before_activation"]=True
        call("/api/v1/workflow/sops",{"sop":sop},expected=201)
        repeated=call("/api/v1/workflow/submit",{"sop_ref":{"id":sop["id"],"version":"1"}})["workflow_id"]
        rows=detail(repeated)["signals"];assert len(rows)==2
        one=next(r for r in rows if r["task_id"]=="input:repeat:1")
        two=next(r for r in rows if r["task_id"]=="input:repeat:2")
        assert one["signal_id"]!=two["signal_id"] and two["state"]=="pending"
        call("/api/v1/workflow/signal",send(two,{"iteration":2}))
        assert next(t for t in detail(repeated)["tasks"] if t["task_id"]==two["task_id"])["attempt"]==0
        call("/api/v1/workflow/signal",send(one,{"iteration":1}))
        repeated_done=terminal(repeated,"done")
        assert [t["result"]["payload"]["iteration"] for t in repeated_done["tasks"] if t["task_id"].startswith("input:")]==[1,2]
        assert call("/api/v1/workflow/signal",send(two,{"iteration":2}))["replayed"]

        corrupt=submit();corrupt_row=waiting(corrupt);stop()
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()[0]=="43"
            db.execute("UPDATE workflow_signals SET spec_json='{}' WHERE workflow_id=?",(corrupt,))
        start();call("/api/v1/workflow/signal",send(corrupt_row),expected=500)
        call("/api/v1/workflow?workflow_id="+corrupt,expected=500)
        print("Signals: single-worker progress, bounded JSON, exact race/replay, kill/restart, expiry/deadline/stop, pinned repeated SOP, auth and integrity passed")
    except Exception:
        log.flush();log.seek(0);print(log.read(),file=sys.stderr);raise
    finally:
        stop();log.close()
