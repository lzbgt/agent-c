"""Durable queued/running cancellation without in-memory job state."""
import json, os, socket, sqlite3, subprocess, sys, tempfile, time, urllib.request, urllib.error
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve())
with socket.socket() as s: s.bind(("127.0.0.1",0)); port=s.getsockname()[1]
base=f"http://127.0.0.1:{port}"
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def call(path, data=None, status=200, auth=True):
  req=urllib.request.Request(base+path,data=None if data is None else json.dumps(data).encode(),
    headers={"Content-Type":"application/json",**({"Authorization":"Bearer cancel-test"} if auth else {})})
  try:
    with opener.open(req,timeout=5) as r: code,body=r.status,json.load(r)
  except urllib.error.HTTPError as e: code,body=e.code,json.load(e)
  assert code==status,(code,body)
  return body
with tempfile.TemporaryDirectory(prefix="durable-cancel-") as root:
  db=str(Path(root)/"state.db"); log=open(Path(root)/"daemon.log","w")
  def start():
    p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--state-dir",root,"--db-path",db,
      "--auth-token","cancel-test","--tools","none","--no-yolo"],stdout=log,stderr=log,
      env={**os.environ,"AGENTD_DOTENV_PATH":"/dev/null","AGENTD_JOB_POLL_MS":"5000"})
    for _ in range(200):
      try:
        if call("/api/v1/health")["ok"]: return p
      except OSError: pass
      time.sleep(.025)
    raise AssertionError("daemon not ready")
  p=start()
  try:
    with sqlite3.connect(db) as c:
      for name,state in [("queued","queued"),("running","running"),("done","done"),("fault","queued")]:
        c.execute("INSERT INTO jobs(job_id,session_id,trace_id,request_json,created_unix_ms,updated_unix_ms,status,cancel_requested) VALUES(?,?,?,'{}',1,1,?,0)",
          (name,"test","trace-"+name,state))
    url="/api/v1/job/cancel?job_id="
    call(url+"queued",{},status=401,auth=False)
    call(url+"queued",{})
    row=call("/api/v1/job?job_id=queued")
    assert row["status"]=="cancelled" and row["cancel_requested"] is True
    assert row["stop_reason"]=="cancelled_before_dispatch"
    call(url+"running",{})
    row=call("/api/v1/job?job_id=running")
    assert row["status"]=="running" and row["cancel_requested"] is True
    call(url+"done",{},status=409);call(url+"missing",{},status=409)
    with sqlite3.connect(db) as c:
      c.execute("CREATE TRIGGER cancel_fault BEFORE UPDATE ON jobs WHEN NEW.job_id='fault' AND NEW.cancel_requested=1 BEGIN SELECT RAISE(FAIL,'test storage fault'); END")
    failed=call(url+"fault",{},status=503)
    assert failed["ok"] is False and failed["live_cancel_requested"] is False
    with sqlite3.connect(db) as c:
      assert c.execute("SELECT cancel_requested FROM jobs WHERE job_id='fault'").fetchone()[0]==0
      c.execute("DROP TRIGGER cancel_fault")
    call(url+"fault",{})
    # Let the queue engine scan: cancelled work must never become runnable again.
    time.sleep(5.2)
    assert call("/api/v1/job?job_id=queued")["status"]=="cancelled"
    assert call("/api/v1/job?job_id=fault")["status"]=="cancelled"
    p.kill();p.wait();p=start()
    assert call("/api/v1/job?job_id=queued")["status"]=="cancelled"
    running=call("/api/v1/job?job_id=running")
    assert running["status"]=="interrupted" and running["cancel_requested"] is True
    assert call("/api/v1/job?job_id=done")["status"]=="done"
    print("PASS DB-only queued cancellation, running flag, auth, terminal guards, explicit storage failure, queue scan and restart preservation")
  finally:
    p.terminate();p.wait(timeout=10);log.close()
