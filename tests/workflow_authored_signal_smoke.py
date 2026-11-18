"""Execute the exact browser-authored signal SOP on an isolated authenticated daemon."""
import json,os,socket,sqlite3,subprocess,sys,tempfile,time,urllib.request
from pathlib import Path
source=json.load(sys.stdin);binary=str(Path(sys.argv[1]).resolve())
action=next(a for s in source["sop"]["stages"] for a in s["actions"] if a["kind"]=="signal")
assert action["signal"]=={"name":"inspection.finished","title":"Inspection ready","timeout_ms":10000,"accept_before_activation":True}
assert action["max_attempts"]==2
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
with socket.socket() as s:s.bind(("127.0.0.1",0));port=s.getsockname()[1]
def call(path,data=None):
 req=urllib.request.Request(f"http://127.0.0.1:{port}"+path,data=None if data is None else json.dumps(data).encode(),headers={"Content-Type":"application/json","Authorization":"Bearer local-signal-test"})
 with opener.open(req,timeout=5) as r:return json.load(r)
with tempfile.TemporaryDirectory(prefix="authored-signal-") as root:
 db=Path(root)/"state.db"
 with open(Path(root)/"daemon.log","w") as log:
  env={k:v for k,v in os.environ.items() if k.lower() not in ("http_proxy","https_proxy","all_proxy")}
  env["AGENTD_DOTENV_PATH"]="/dev/null"
  p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--tools","none","--state-dir",root,"--db-path",str(db),"--auth-token","local-signal-test","--workflow-poll-ms","10","--workflow-concurrency","1"],env=env,stdout=log,stderr=log)
  try:
   for _ in range(200):
    try:
     if call("/api/v1/health")["ok"]:break
    except OSError:pass
    time.sleep(.025)
   definition=call("/api/v1/workflow/sops",{"sop":source["sop"]})["definition"]
   with sqlite3.connect(db) as conn:assert conn.execute("SELECT count(*) FROM workflow_signals").fetchone()[0]==0
   wid=call("/api/v1/workflow/submit",{"sop_ref":{k:definition[k] for k in ("id","version","definition_hash")}})["workflow_id"]
   for _ in range(300):
    detail=call("/api/v1/workflow?workflow_id="+wid+"&include_tasks=1&include_results=1")
    ready=next((s for s in detail["signals"] if s["available"]),None)
    if ready:break
    time.sleep(.025)
   assert ready and ready["state"]=="pending" and ready["name"]=="inspection.finished" and ready["title"]=="Inspection ready",detail
   assert next(t for t in detail["tasks"] if t["task_id"]==action["task_id"])["attempt"]==0
   payload={k:ready[k] for k in ("workflow_id","task_id","signal_id")}
   payload.update(idempotency_key="browser-authored-result",payload={"result":"accepted","measurements":[1,2,3]})
   receipt=call("/api/v1/workflow/signal",payload)
   assert receipt["replayed"] is False and receipt["signal"]["state"]=="buffered"
   for _ in range(300):
    detail=call("/api/v1/workflow?workflow_id="+wid+"&include_tasks=1&include_results=1")
    if detail["workflow"]["status"] in ("done","error","cancelled"):break
    time.sleep(.025)
   assert detail["workflow"]["status"]=="done",detail
   result=next(t for t in detail["tasks"] if t["task_id"]==action["task_id"])["result"]
   assert result["payload"]==payload["payload"],result
   assert call("/api/v1/workflow/signal",payload)["replayed"] is True
   with sqlite3.connect(db) as conn:assert conn.execute("SELECT count(*) FROM workflow_signals WHERE state='resolved'").fetchone()[0]==1
  finally:p.terminate();p.wait(timeout=10)
print("PASS exact browser-authored signal SOP saved, pinned, independently waiting and delivered once")
