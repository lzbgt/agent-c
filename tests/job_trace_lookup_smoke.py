"""Authenticated metadata-only trace correlation: never a dispatch/retry."""
import json, os, socket, sqlite3, subprocess, sys, tempfile, time, urllib.request, urllib.error
from pathlib import Path
binary = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with socket.socket() as s: s.bind(("127.0.0.1",0)); port=s.getsockname()[1]
base=f"http://127.0.0.1:{port}"
def call(path, data=None, status=200, auth=True):
    req=urllib.request.Request(base+path,data=None if data is None else json.dumps(data).encode(),
      headers={"Content-Type":"application/json", **({"Authorization":"Bearer lookup-test"} if auth else {})})
    try:
      with opener.open(req,timeout=5) as r: code,result=r.status,json.load(r)
    except urllib.error.HTTPError as e: code,result=e.code,json.load(e)
    assert code==status,(code,result)
    return result
with tempfile.TemporaryDirectory(prefix="job-trace-") as root:
    db=str(Path(root)/"state.db")
    log=open(Path(root)/"daemon.log","w")
    def start():
      p=subprocess.Popen([binary,"--host","127.0.0.1","--port",str(port),"--state-dir",root,
        "--db-path",db,"--auth-token","lookup-test","--tools","none","--no-yolo"],
        stdout=log,stderr=log,env={**os.environ,"AGENTD_DOTENV_PATH":"/dev/null"})
      for _ in range(200):
        try:
          if call("/api/v1/health")["ok"]: return p
        except OSError: pass
        time.sleep(.025)
      raise AssertionError("daemon not ready")
    p=start()
    try:
      url="/api/v1/job?trace_id=original-trace&session_id=original-session"
      call(url,status=401,auth=False)
      call(url,status=404)
      for suffix in ["trace_id=x","trace_id=&session_id=s","trace_id=bad%20trace&session_id=s","trace_id=x&session_id=s&job_id=j"]:
        call("/api/v1/job?"+suffix,status=400)
      # Real accepted request fails local argument validation: no provider/device action.
      accepted=call("/api/v1/run_async",{"prompt":"synthetic","session_id":"original-session",
        "trace_id":"original-trace","tools":"invalid-test-tools","base_url":"http://127.0.0.1:1","api_key":"synthetic","timeout_ms":100}, status=202)
      job=accepted["job_id"]
      for _ in range(200):
        found=call(url)
        if found["status"] in ("done","error","cancelled","interrupted"): break
        time.sleep(.025)
      assert found["job_id"]==job and found["durable_only"] is True
      assert "request_json" not in found and "result" not in found and "error" not in found
      call("/api/v1/job?trace_id=original-trace&session_id=other",status=404)
      with sqlite3.connect(db) as c:
        before=c.execute("SELECT * FROM jobs ORDER BY job_id").fetchall()
      for _ in range(10): assert call(url)["job_id"]==job
      with sqlite3.connect(db) as c:
        assert c.execute("SELECT * FROM jobs ORDER BY job_id").fetchall()==before
      p.kill();p.wait();p=start()
      assert call(url)["job_id"]==job
      with sqlite3.connect(db) as c:
        c.execute("INSERT INTO jobs(job_id,session_id,trace_id,created_unix_ms,updated_unix_ms,status,cancel_requested) VALUES('duplicate','original-session','original-trace',1,1,'done',0)")
      call(url,status=409)
      with sqlite3.connect(db) as c:
        c.execute("UPDATE jobs SET session_id='other' WHERE job_id='duplicate'")
      assert call(url)["job_id"]==job
      assert call("/api/v1/job?trace_id=original-trace&session_id=other")["job_id"]=="duplicate"
      call("/api/v1/job?job_id=absent",status=404)
      with sqlite3.connect(db) as c:
        c.execute("ALTER TABLE jobs RENAME TO jobs_fault_fixture")
      failure=call("/api/v1/job?job_id="+job,status=503)
      assert "outcome remains unknown" in failure["error"], failure
      call(url,status=500)
      with sqlite3.connect(db) as c:
        c.execute("ALTER TABLE jobs_fault_fixture RENAME TO jobs")
      assert call("/api/v1/job?job_id="+job)["job_id"]==job
      assert call(url)["job_id"]==job
      print("PASS durable lookup/auth/restart/isolation and storage-failure outcome preservation/recovery")
    finally:
      p.terminate();p.wait(timeout=10);log.close()
