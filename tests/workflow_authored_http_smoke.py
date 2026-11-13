"""Execute exact browser-authored integration SOP against synthetic loopback JSON."""
import json,os,socket,subprocess,sys,tempfile,time,threading,urllib.request
from urllib.parse import urlparse
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
source=json.load(sys.stdin); binary=str(Path(sys.argv[1]).resolve())
action=next(a for s in source["sop"]["stages"] for a in s["actions"] if a["kind"]=="http_json")
spec=action["http_json"];url=urlparse(spec["url"])
assert url.hostname=="127.0.0.1" and url.path=="/status" and spec["method"]=="GET"
assert spec["timeout_ms"]==4500 and spec["max_bytes"]==4096 and spec["bearer_env"]=="SOP_TEST_TOKEN"
seen=[]
class Handler(BaseHTTPRequestHandler):
 def do_GET(self):
  seen.append((self.path,self.headers.get("Authorization"),self.headers.get("X-Test")))
  body=json.dumps({"source":"synthetic","ready":True}).encode()
  self.send_response(200);self.send_header("Content-Type","application/json");self.end_headers();self.wfile.write(body)
 def log_message(self,*args):pass
server=ThreadingHTTPServer(("127.0.0.1",url.port),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
with socket.socket() as s:s.bind(("127.0.0.1",0));port=s.getsockname()[1]
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def call(path,data=None):
 req=urllib.request.Request(f"http://127.0.0.1:{port}"+path,data=None if data is None else json.dumps(data).encode(),headers={"Content-Type":"application/json","Authorization":"Bearer local-test"})
 with opener.open(req,timeout=5) as r:return json.load(r)
with tempfile.TemporaryDirectory(prefix="authored-http-") as root:
 with open(Path(root)/"daemon.log","w") as log:
  env={k:v for k,v in os.environ.items() if k.lower() not in ("http_proxy","https_proxy","all_proxy")}
  env.update(AGENTD_DOTENV_PATH="/dev/null",SOP_TEST_TOKEN="synthetic-only")
  p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--tools","none","--no-yolo","--workflow-enable-http-tasks","--state-dir",root,"--db-path",str(Path(root)/"state.db"),"--auth-token","local-test","--workflow-poll-ms","10"],env=env,stdout=log,stderr=log)
  try:
   for _ in range(200):
    try:
     if call("/api/v1/health")["ok"]:break
    except OSError:pass
    time.sleep(.025)
   definition=call("/api/v1/workflow/sops",{"sop":source["sop"]})["definition"]
   assert not seen,"saving must not execute"
   wid=call("/api/v1/workflow/submit",{"sop_ref":{k:definition[k] for k in ("id","version","definition_hash")}})["workflow_id"]
   for _ in range(400):
    result=call("/api/v1/workflow?workflow_id="+wid+"&include_results=1")
    if result["workflow"]["status"] in ("done","error","cancelled"):break
    time.sleep(.025)
   assert result["workflow"]["status"]=="done",result
   out=result["result"]["results_by_task"][action["task_id"]]
   assert out["http"]["response_json"]=={"source":"synthetic","ready":True},out
   assert seen==[("/status","Bearer synthetic-only","preserved")],seen
   print("PASS exact browser-authored GET SOP saved without execution, pinned and executed with server credential reference and preserved headers")
  finally:p.terminate();p.wait(timeout=10);server.shutdown();server.server_close()
