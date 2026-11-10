"""Exercise real HTTP side effects at durable crash boundaries without providers."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

agentd = str(Path(sys.argv[1]).resolve())
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
lock = threading.Lock()
posts, applied, receipts, signals = {}, {}, {}, {}
release = threading.Event()


def wait_for(fn, explanation, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            result = fn()
            if result:
                return result
        except (OSError, KeyError, sqlite3.Error, urllib.error.URLError):
            pass
        time.sleep(.025)
    raise AssertionError(explanation)


class Destination(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def respond(self, payload):
        data = json.dumps(payload).encode()
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        case = request['case']
        effect = self.headers['Idempotency-Key']
        with lock:
            posts[case] = posts.get(case, 0) + 1
            first = effect not in receipts
            if first:
                applied[case] = applied.get(case, 0) + 1
                result = {'case': case, 'applied': applied[case]}
                receipts[effect] = {'schema': 'agentd.effect.receipt.v1', 'effect_id': effect, 'request_hash': self.headers['X-Agent-Effect-Hash'],
                                    'attempt': int(self.headers['X-Agent-Effect-Attempt']), 'state': 'committed',
                                    'final': True, 'result': {'status': 200, 'response_json': result}}
            result = receipts[effect]['result']['response_json']
            signals.setdefault(case, threading.Event()).set()
        if case == 'remote_commit' and first:
            release.wait(12)
        if case in ('lost_guarded', 'lost_idempotent', 'cancel_guarded') and first:
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        self.respond(result)

    def do_GET(self):
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        with lock:
            proof = dict(receipts[query['effect_id'][0]])
        assert query['request_hash'][0] == proof['request_hash']
        assert int(query['attempt'][0]) == proof['attempt']
        self.respond(proof)


server = ThreadingHTTPServer(('127.0.0.1', 0), Destination)
threading.Thread(target=server.serve_forever, daemon=True).start()
destination = f'http://127.0.0.1:{server.server_port}'
process = None
try:
    with tempfile.TemporaryDirectory(prefix='agent-effect-recovery-') as tmp:
        root = Path(tmp)
        database = root / 'agentd.sqlite'
        log = (root / 'daemon.log').open('w+')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        base = f'http://127.0.0.1:{port}'
        env = {k: v for k, v in os.environ.items() if k.lower() not in ('http_proxy', 'https_proxy', 'all_proxy')}

        def call(path, payload=None):
            data = None if payload is None else json.dumps(payload).encode()
            req = urllib.request.Request(base + path, data=data, headers={'Content-Type': 'application/json'})
            with opener.open(req, timeout=5) as response:
                return json.load(response)

        def start():
            global process
            process = subprocess.Popen([agentd, '--host', '127.0.0.1', '--port', str(port), '--tools', 'none',
                                        '--db-path', str(database), '--state-dir', str(root / 'state'),
                                        '--workflow-enable-http-tasks', '--workflow-concurrency', '1'], cwd=root, env=env, stdout=log, stderr=log)
            wait_for(lambda: process.poll() is None and call('/api/v1/health').get('ok'), 'daemon failed to start')

        def stop(kill=False):
            global process
            if process is not None:
                (process.kill if kill else process.terminate)()
                process.wait(timeout=10)
                process = None

        def sql(text, params=()):
            with sqlite3.connect(database, timeout=5) as db:
                return db.execute(text, params).fetchall()

        def submit(case, effect=None):
            hj = {'url': destination + '/orders', 'body': {'case': case}, 'timeout_ms': 5000}
            if effect is not None:
                hj['effect'] = effect
            payload = {'workflow_id': 'wf_' + case, 'tasks': [
                {'task_id': 'write', 'kind': 'http_json', 'priority': 100, 'max_attempts': 10, 'http_json': hj},
                {'task_id': 'independent', 'kind': 'delay', 'priority': -100, 'delay_ms': 0},
                {'task_id': 'after', 'kind': 'delay', 'delay_ms': 0, 'depends_on': ['write']},
            ]}
            result = call('/api/v1/workflow/submit', payload)
            assert result['ok'], result
            return result['workflow_id']

        def terminal(wid):
            def poll():
                result = call('/api/v1/workflow?workflow_id=' + wid + '&include_tasks=1&include_results=1')
                return result if result['workflow']['status'] in ('done', 'error', 'cancelled') else None
            return wait_for(poll, 'workflow did not settle: ' + wid)

        try:
            start()
            # Intent persisted, dispatch admission fails, then daemon is killed.
            sql("CREATE TRIGGER stop_dispatch BEFORE UPDATE OF state ON workflow_effects WHEN NEW.state='dispatching' BEGIN SELECT RAISE(FAIL,'injected before dispatch'); END")
            wid = submit('prepared')
            wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='prepared'", (wid,)), 'intent not persisted')
            assert posts.get('prepared', 0) == 0
            stop(kill=True)
            sql('DROP TRIGGER stop_dispatch')
            start()
            detail = terminal(wid)
            assert detail['workflow']['status'] == 'done' and posts['prepared'] == applied['prepared'] == 1, detail

            # Destination commits, but daemon dies before receiving the response.
            wid = submit('remote_commit', {'reconcile_url': destination + '/receipts'})
            wait_for(lambda: signals.get('remote_commit') and signals['remote_commit'].is_set(), 'remote write did not commit')
            stop(kill=True)
            release.set()
            start()
            detail = terminal(wid)
            assert detail['workflow']['status'] == 'done' and posts['remote_commit'] == applied['remote_commit'] == 1, detail
            assert detail['effects'][0]['state'] == 'completed'
            result = detail['result']['results_by_task']['write']
            assert result['effect']['reconciled'] is True and result['http']['response_json']['case'] == 'remote_commit', result

            # Receipt persisted, then task completion fails before process death.
            sql("CREATE TRIGGER stop_task_done BEFORE UPDATE OF status ON workflow_tasks WHEN NEW.workflow_id='wf_settled' AND NEW.task_id='write' AND NEW.status='done' BEGIN SELECT RAISE(FAIL,'injected before task completion'); END")
            wid = submit('settled')
            wait_for(lambda: sql("SELECT 1 FROM workflow_effects WHERE workflow_id=? AND state='completed'", (wid,)), 'receipt not persisted')
            stop(kill=True)
            sql('DROP TRIGGER stop_task_done')
            start()
            detail = terminal(wid)
            assert detail['workflow']['status'] == 'done' and posts['settled'] == applied['settled'] == 1, detail
            assert detail['result']['results_by_task']['write']['effect']['replayed'] is True

            # Ambiguous guarded write stops only its dependent branch.
            wid = submit('lost_guarded')
            detail = terminal(wid)
            by_id = {task['task_id']: task for task in detail['tasks']}
            result = detail['result']['results_by_task']['write']
            assert detail['workflow']['status'] == 'error' and result['needs_input'] is True, detail
            assert by_id['independent']['status'] == 'done' and by_id['write']['attempt'] == 1, detail
            assert posts['lost_guarded'] == applied['lost_guarded'] == 1
            try:
                call('/api/v1/workflow/reconcile', {'workflow_id': wid, 'task_id': 'write', 'reconcile_url': 'http://other.example/receipts'})
            except urllib.error.HTTPError as e:
                assert e.code == 400
            else:
                raise AssertionError('cross-origin receipt endpoint accepted')
            resumed = call('/api/v1/workflow/reconcile', {'workflow_id': wid, 'task_id': 'write', 'reconcile_url': destination + '/receipts'})
            assert resumed['ok'], resumed
            detail = terminal(wid)
            assert detail['workflow']['status'] == 'done' and all(t['status'] == 'done' for t in detail['tasks']), detail
            assert posts['lost_guarded'] == 1, 'human-action recovery repeated the original write'

            wid = submit('cancel_guarded')
            terminal(wid)
            call('/api/v1/workflow/cancel', {'workflow_id': wid})
            try:
                call('/api/v1/workflow/reconcile', {'workflow_id': wid, 'task_id': 'write', 'reconcile_url': destination + '/receipts'})
            except urllib.error.HTTPError as e:
                assert e.code == 409
            else:
                raise AssertionError('reconciliation bypassed an owner stop')
            assert posts['cancel_guarded'] == 1

            # Explicit idempotency contract permits a safe repeat of the same key.
            wid = submit('lost_idempotent', {'mode': 'idempotent'})
            detail = terminal(wid)
            assert detail['workflow']['status'] == 'done' and posts['lost_idempotent'] == 2 and applied['lost_idempotent'] == 1, detail
            assert detail['effects'][0]['attempt'] == 2
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read()[-7000:], file=sys.stderr)
            raise
        finally:
            stop()
            log.close()
finally:
    release.set()
    server.shutdown()
    server.server_close()
print('Crash-before-dispatch, lost response, saved receipt, scoped blocker and idempotent recovery passed')
