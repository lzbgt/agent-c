"""Execute exact browser-authored host SOP in isolated permitted and denied daemons."""
import json,os,socket,sqlite3,subprocess,sys,tempfile,time,urllib.request
from pathlib import Path
source=json.load(sys.stdin);binary=str(Path(sys.argv[1]).resolve())
action=next(a for s in source["sop"]["stages"] for a in s["actions"] if a["kind"]=="host_tool")
spec=action["host_tool"]
assert spec["name"]=="proc_exec" and spec["timeout_ms"]==4500
assert spec["arguments"]["argv"]==["/usr/bin/printf","[%s]","space ; $(literal)",""]
assert action["max_attempts"]==1
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
for allowed in (False,True):
 with socket.socket() as s:s.bind(("127.0.0.1",0));port=s.getsockname()[1]
 def call(path,data=None):
  req=urllib.request.Request(f"http://127.0.0.1:{port}"+path,data=None if data is None else json.dumps(data).encode(),headers={"Content-Type":"application/json","Authorization":"Bearer local-test"})
  with opener.open(req,timeout=5) as r:return json.load(r)
 with tempfile.TemporaryDirectory(prefix="authored-host-") as root:
  db=Path(root)/"state.db"
  with open(Path(root)/"daemon.log","w") as log:
   env={k:v for k,v in os.environ.items() if k.lower() not in ("http_proxy","https_proxy","all_proxy")}
   env["AGENTD_DOTENV_PATH"]="/dev/null"
   p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--tools","host","--yolo" if allowed else "--no-yolo","--state-dir",root,"--db-path",str(db),"--auth-token","local-test","--workflow-poll-ms","10"],env=env,stdout=log,stderr=log)
   try:
    for _ in range(200):
     try:
      if call("/api/v1/health")["ok"]:break
     except OSError:pass
     time.sleep(.025)
    definition=call("/api/v1/workflow/sops",{"sop":source["sop"]})["definition"]
    with sqlite3.connect(db) as conn:assert conn.execute("SELECT count(*) FROM workflow_effects").fetchone()[0]==0
    wid=call("/api/v1/workflow/submit",{"sop_ref":{k:definition[k] for k in ("id","version","definition_hash")}})["workflow_id"]
    for _ in range(400):
     result=call("/api/v1/workflow?workflow_id="+wid+"&include_results=1")
     if result["workflow"]["status"] in ("done","error","cancelled"):break
     time.sleep(.025)
    out=result["result"]["results_by_task"][action["task_id"]]
    if allowed:
     assert result["workflow"]["status"]=="done",result
     assert out["assistant_text"]=="[space ; $(literal)][]",out
     with sqlite3.connect(db) as conn:assert conn.execute("SELECT count(*) FROM workflow_effects WHERE state='completed'").fetchone()[0]==1
    else:
     assert result["workflow"]["status"]=="error",result
     with sqlite3.connect(db) as conn:assert conn.execute("SELECT count(*) FROM workflow_effects").fetchone()[0]==0
   finally:p.terminate();p.wait(timeout=10)
print("PASS exact browser-authored host SOP pinned, literal argv preserved, denied policy admits no effect")
