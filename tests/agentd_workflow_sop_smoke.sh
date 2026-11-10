#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"
AGENTD_BIN="${1:?missing agentd binary}"
PORT_DAEMON="$(agentd_smoke_pick_port)"
LOG_DIR="$(agentd_smoke_log_dir)"
trap agentd_smoke_stop EXIT
agentd_smoke_start "${AGENTD_BIN}" 127.0.0.1 "${PORT_DAEMON}" agentd_workflow_sop_smoke --tools none
agentd_smoke_wait_health "${DAEMON_URL}"
python3 - "${DAEMON_URL}" <<'PY'
import json, sys, time, urllib.request, urllib.error
base = sys.argv[1]
def call(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(base + path, data=data, headers={"Content-Type":"application/json"})
    with urllib.request.urlopen(req, timeout=10) as res:
        return json.load(res)
sop = {"schema":"agentd.sop.v1", "id":"inspection", "version":"1", "stages":[
    {"id":"intake", "actions":[{"task_id":"a", "kind":"delay", "delay_ms":0}]},
    {"id":"checks", "mode":"parallel", "actions":[{"task_id":"b", "kind":"delay", "delay_ms":0}, {"task_id":"c", "kind":"delay", "delay_ms":0}]},
    {"id":"release", "actions":[{"task_id":"d", "kind":"delay", "delay_ms":0}]},
]}
payload = {"sop":sop, "idempotency_key":"sop-smoke"}
res = call('/api/v1/workflow/submit', payload)
assert res['ok'], res
wid = res['workflow_id']
assert call('/api/v1/workflow/submit', payload).get('deduped') is True
for _ in range(100):
    detail = call('/api/v1/workflow?workflow_id='+wid+'&include_tasks=1&include_results=1&include_spec=1')
    if detail['workflow']['status'] in ('done','error','cancelled'): break
    time.sleep(.1)
assert detail['workflow']['status'] == 'done', detail
assert len(detail['tasks']) == 4
assert detail['spec']['sop'] == sop
assert detail['spec']['sop_compiled']['definition_hash'].startswith('sha256:')
tasks = {t['task_id']:t for t in detail['spec']['tasks']}
assert tasks['b']['depends_on'] == ['a'] and tasks['d']['depends_on'] == ['b','c']
for invalid in ({'sop':sop,'tasks':[]}, {'sop':dict(sop, approval=True)}):
    try: call('/api/v1/workflow/submit',invalid)
    except urllib.error.HTTPError as e: assert e.code == 400
    else: raise AssertionError('invalid SOP accepted')
print('SOP submit, stage barriers, persistence and dedupe passed')
PY
