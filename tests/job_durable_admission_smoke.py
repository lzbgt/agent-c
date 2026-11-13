"""Real local-provider request, DB admission failure and crash outcome fence."""
import json, os, socket, sqlite3, subprocess, sys, tempfile, time, threading, urllib.request, urllib.error
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve())
calls=[]; release=threading.Event()
class Provider(BaseHTTPRequestHandler):
  def do_POST(self):
    calls.append(json.loads(self.rfile.read(int(self.headers.get("Content-Length",0)))))
    release.wait(10)
    body=json.dumps({"id":"synthetic","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}).encode()
    try:
      self.send_response(200);self.send_header("Content-Type","application/json");self.end_headers();self.wfile.write(body)
    except (BrokenPipeError,ConnectionResetError): pass
  def log_message(self,*args): pass
provider=ThreadingHTTPServer(("127.0.0.1",0),Provider)
threading.Thread(target=provider.serve_forever,daemon=True).start()
with socket.socket() as sock: sock.bind(("127.0.0.1",0));port=sock.getsockname()[1]
base=f"http://127.0.0.1:{port}"
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def call(path,data=None,status=200):
  req=urllib.request.Request(base+path,data=None if data is None else json.dumps(data).encode(),
    headers={"Authorization":"Bearer admission-test","Content-Type":"application/json"})
  try:
    with opener.open(req,timeout=5) as r: code,result=r.status,json.load(r)
  except urllib.error.HTTPError as e: code,result=e.code,json.load(e)
  assert code==status,(code,result)
  return result
with tempfile.TemporaryDirectory(prefix="job-admission-") as root:
  db=str(Path(root)/"state.db"); log=open(Path(root)/"daemon.log","w")
  def start():
    p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--state-dir",root,"--db-path",db,
      "--auth-token","admission-test","--tools","none","--no-yolo"],stdout=log,stderr=log,
      env={**os.environ,"AGENTD_DOTENV_PATH":"/dev/null"})
    for _ in range(200):
      try:
        if call("/api/v1/health")["ok"]: return p
      except OSError: pass
      time.sleep(.025)
    raise AssertionError("daemon not ready")
  p=start()
  request={"job_id":"original","trace_id":"trace-original","session_id":"admission","prompt":"synthetic",
    "tools":"none","model":"fake","api_key":"synthetic","base_url":f"http://127.0.0.1:{provider.server_port}/v1","timeout_ms":15000}
  try:
    with sqlite3.connect(db) as c:
      c.execute("CREATE TRIGGER reject_admission BEFORE INSERT ON jobs BEGIN SELECT RAISE(FAIL,'synthetic disk failure'); END")
    call("/api/v1/run_async",request,status=503)
    time.sleep(.15);assert not calls
    with sqlite3.connect(db) as c:
      assert c.execute("SELECT count(*) FROM jobs").fetchone()[0]==0
      c.execute("DROP TRIGGER reject_admission")
    assert call("/api/v1/run_async",request,status=202)["job_id"]=="original"
    for _ in range(200):
      if calls: break
      time.sleep(.025)
    assert len(calls)==1
    with sqlite3.connect(db) as c:
      row=c.execute("SELECT status,request_json FROM jobs WHERE job_id='original'").fetchone()
      assert row[0]=="running" and "synthetic" not in json.loads(row[1]).get("api_key","")
    call("/api/v1/run_async",request,status=409)
    p.kill();p.wait();release.set()
    p=start()
    outcome=call("/api/v1/job?job_id=original")
    assert outcome["status"]=="interrupted" and outcome["stop_reason"]=="restart_outcome_unknown",outcome
    call("/api/v1/run_async",{**request,"prompt":"different"},status=409)
    time.sleep(.5);assert len(calls)==1
    with sqlite3.connect(db) as c:
      assert c.execute("SELECT count(*) FROM jobs").fetchone()[0]==1
      assert c.execute("SELECT status FROM jobs WHERE job_id='original'").fetchone()[0]=="interrupted"
    # A genuinely never-claimed queued job remains runnable.
    queued={**request,"job_id":"never-started","trace_id":"trace-never-started","tools":"invalid-test-tools"}
    with sqlite3.connect(db) as c:
      c.execute("INSERT INTO jobs(job_id,session_id,trace_id,request_json,created_unix_ms,updated_unix_ms,status,cancel_requested) VALUES(?,?,?,?,1,1,'queued',0)",
        ("never-started","admission","trace-never-started",json.dumps(queued)))
    for _ in range(200):
      with sqlite3.connect(db) as c:
        state=c.execute("SELECT status FROM jobs WHERE job_id='never-started'").fetchone()[0]
      if state in ("done","error"): break
      time.sleep(.025)
    assert state in ("done","error"),state
    assert len(calls)==1
    release.clear()
    live={**request,"job_id":"live-cancel","trace_id":"trace-live-cancel"}
    call("/api/v1/run_async",live,status=202)
    for _ in range(200):
      if len(calls)==2: break
      time.sleep(.025)
    assert len(calls)==2
    call("/api/v1/job/cancel?job_id=live-cancel",{})
    assert call("/api/v1/job?job_id=live-cancel")["cancel_requested"] is True
    with sqlite3.connect(db) as c:
      assert c.execute("SELECT cancel_requested FROM jobs WHERE job_id='live-cancel'").fetchone()[0]==1
    # Keep provider blocked so normal completion does not race the stop callback.
    for _ in range(200):
      result=call("/api/v1/job?job_id=live-cancel")
      if result["status"] in ("cancelled","error","done"): break
      time.sleep(.025)
    release.set()
    assert result["status"]=="cancelled",result
    print("PASS durable admission failure/crash/identity fences, never-claimed queue progress and live local-provider cancellation")
  finally:
    p.terminate();p.wait(timeout=10);release.set();provider.shutdown();log.close()
