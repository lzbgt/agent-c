"""Actual loopback provider requests enforce fresh token grants (no external providers)."""
import json,os,socket,sqlite3,subprocess,sys,tempfile,threading,time,urllib.request
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve());seen={}
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  req=json.loads(self.rfile.read(int(self.headers["Content-Length"])))
  case=self.path.split("/")[1]; seen.setdefault(case,[]).append(req)
  n=len(seen[case]);final=n==2 and not case.startswith("exhaust")
  message={"role":"assistant","content":"done"} if final else {"role":"assistant","content":"","tool_calls":[{"id":"call-"+str(n),"type":"function","function":{"name":"proc_exec","arguments":json.dumps({"argv":["/usr/bin/printf","synthetic"]})}}]}
  if case.startswith("failed"):
   body=json.dumps({"error":{"message":"synthetic unavailable"}}).encode();self.send_response(503);self.send_header("Content-Length",str(len(body)));self.end_headers();self.wfile.write(body);return
  usage={"prompt_tokens":3,"completion_tokens":1,"total_tokens":4}
  if case.startswith("exhaust"):usage={"prompt_tokens":8,"completion_tokens":2,"total_tokens":10}
  if case.startswith("partial"):usage={"completion_tokens":1}
  if req.get("stream"):
   delta=message.copy();delta.pop("role",None)
   for i,t in enumerate(delta.get("tool_calls",[])):t["index"]=i
   chunks=[{"choices":[{"index":0,"delta":delta,"finish_reason":None}]},{"choices":[{"index":0,"delta":{},"finish_reason":"stop" if final else "tool_calls"}]}]
   if not case.startswith("missing") and not (case.startswith("late_missing") and n==2):chunks.append({"choices":[],"usage":usage})
   body=("".join("data: "+json.dumps(c)+"\n\n" for c in chunks)+"data: [DONE]\n\n").encode()
   self.send_response(200);self.send_header("Content-Type","text/event-stream")
  else:
   payload={"choices":[{"message":message,"finish_reason":"stop" if final else "tool_calls"}]}
   if not case.startswith("missing") and not (case.startswith("late_missing") and n==2):payload["usage"]=usage
   body=json.dumps(payload).encode();self.send_response(200);self.send_header("Content-Type","application/json")
  self.send_header("Content-Length",str(len(body)));self.end_headers();self.wfile.write(body)
provider=ThreadingHTTPServer(("127.0.0.1",0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
with tempfile.TemporaryDirectory(prefix="token-grant-") as root:
 with socket.socket() as sock:sock.bind(("127.0.0.1",0));port=sock.getsockname()[1]
 db=Path(root)/"state.db"
 def call(path,data=None):
  req=urllib.request.Request(f"http://127.0.0.1:{port}"+path,data=None if data is None else json.dumps(data).encode(),headers={"Content-Type":"application/json"})
  with opener.open(req,timeout=5) as r:return json.load(r)
 with open(Path(root)/"daemon.log","w") as log:
  env={k:v for k,v in os.environ.items() if k.lower() not in ("http_proxy","https_proxy","all_proxy")};env["AGENTD_DOTENV_PATH"]="/dev/null"
  p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--tools","host","--yolo","--state-dir",root,"--db-path",str(db),"--workflow-poll-ms","10"],env=env,stdout=log,stderr=log)
  try:
   for _ in range(200):
    try:
     if call("/api/v1/health")["ok"]:break
    except OSError:pass
    time.sleep(.025)
   for stream in (False,True):
    for mode in ("known","exhaust","missing","partial","failed","late_missing"):
     case=mode+str(int(stream))
     request={"prompt":case,"model":"synthetic","base_url":f"http://127.0.0.1:{provider.server_port}/{case}/v1","api_key":"synthetic","tools":"host","no_session":True,"stream_assistant":stream,"max_steps":4,"max_tool_calls_total":3,"max_retries":0}
     wid=call("/api/v1/workflow/submit",{"workflow_id":case,"allow_inline_api_keys":True,"workflow_limits":{"max_total_tokens":10},"tasks":[{"task_id":"model","request":request,"max_attempts":1}]})["workflow_id"]
     for _ in range(500):
      result=call("/api/v1/workflow?workflow_id="+wid+"&include_results=1")
      if result["workflow"]["status"] in ("done","error","cancelled"):break
      time.sleep(.025)
     out=result["result"]["results_by_task"]["model"]
     assert all(r.get("stream",False)==stream for r in seen[case]),(case,seen[case])
     caps=[r.get("max_completion_tokens",r.get("max_tokens")) for r in seen[case]]
     if mode=="known":
      assert caps==[10,6],(case,caps,out)
      assert out["ok"] and out["total_tokens"]==8 and out["token_budget_charge"]==8,(case,out)
     elif mode=="late_missing":
      assert caps==[10,6] and out["ok"] and out["total_tokens"]==4 and out["token_budget_charge"]==10 and out["token_usage_incomplete"],(case,caps,out)
     else:
      assert caps==[10],(case,caps,out)
      assert not out["ok"] and out["retryable"] is False and out["token_budget_charge"]==10,(case,out)
      if mode in ("missing","partial","failed"):assert out["token_usage_incomplete"] is True,(case,out)
     with sqlite3.connect(db) as conn:
      charged=conn.execute("SELECT total_tokens_cum FROM workflow_tasks WHERE workflow_id=? AND task_id='model'",(wid,)).fetchone()[0]
      assert charged==(8 if mode=="known" else 10),(case,charged,out)
     print("PASS",case,caps,charged)
  finally:p.terminate();p.wait(timeout=10);provider.shutdown();provider.server_close()
