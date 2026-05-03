# Production Deployment Guide (agentd + broker + WebUI)

This guide focuses on **production-grade** deployment patterns for:
- `agentd` (daemon backend)
- `agentd-broker` + `agentd-connector` (secure relay + NAT traversal)
- WebUI (static site)

## Recommended topology

**Best practice**: expose **broker** publicly, keep `agentd` private.
- Clients authenticate to broker via OIDC/JWT.
- Broker proxies requests to agentd over mTLS (connector).
- Agentd can stay on loopback or private networks only.

Direct `agentd` exposure is supported, but you must harden it (see below).

---

## agentd (daemon)

### Hardening checklist
- **Auth**: use `--auth-token` (required for non-loopback binds).
- **CORS**: allowlist UI origins with `--cors-origin <https://ui.example>`.
- **State**: set `--state-dir` and `--db-path` on a persistent disk.
- **Secrets**: keep provider keys out of the UI; load via `.not_in_repo` or env. For service contexts,
  `AGENTD_DOTENV_PATH=/path/to/.env` can point the daemon at a specific dotenv file.
- **HTTP tasks**: keep `--workflow-enable-http-tasks` **off** unless needed; if enabled, set:
  - `--workflow-http-allow-host ...` and/or `--workflow-http-allow-cidr ...`
  - `--workflow-http-deny-private` (recommended)
  - `--workflow-http-dns-pin` (recommended)

### Example (systemd)
```
sudo install -d -o agentd -g agentd /var/lib/agentd /etc/agentd
sudo install -m 0644 packaging/systemd/agentd.service /etc/systemd/system/agentd.service
sudo install -m 0600 packaging/systemd/agentd.env.example /etc/agentd/agentd.env
sudo systemctl daemon-reload
sudo systemctl enable --now agentd.service
```

### Example (macOS launchd)
```
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.agentd.daemon</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>AGENTD_DOTENV_PATH</key><string>/Users/you/Library/Application Support/agentd/agentd.env</string>
  </dict>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/agentd</string>
    <string>--host</string><string>127.0.0.1</string>
    <string>--port</string><string>8123</string>
    <string>--auth-token</string><string>REPLACE_WITH_RANDOM_TOKEN</string>
    <string>--state-dir</string><string>/Users/you/Library/Application Support/agentd</string>
    <string>--db-path</string><string>/Users/you/Library/Application Support/agentd/agentd.db</string>
  </array>
  <key>WorkingDirectory</key><string>/Users/you/Library/Application Support/agentd</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/Users/you/Library/Logs/agentd.out.log</string>
  <key>StandardErrorPath</key><string>/Users/you/Library/Logs/agentd.err.log</string>
</dict>
</plist>
```

### Helper scripts (macOS)

For local bring-up on macOS, you can install/remove launchd services with:
```
AGENTD_AUTH_TOKEN="REPLACE_WITH_RANDOM_TOKEN" tools/install_agentd_launchd.sh
tools/uninstall_agentd_launchd.sh
```
You can also point the daemon at a specific dotenv file when installing the service:
```
AGENTD_DOTENV_PATH="/path/to/.env" tools/install_agentd_launchd.sh
```
For autonomous orchestration services on macOS, use the helper scripts:
```
BROKER_BASE="https://broker.example.com" \
  BROKER_OIDC_TOKEN="REPLACE_WITH_SERVICE_TOKEN" \
  tools/install_orchestrator_launchd.sh
tools/uninstall_orchestrator_launchd.sh

BROKER_BASE="https://broker.example.com" \
  BROKER_OIDC_TOKEN="REPLACE_WITH_SERVICE_TOKEN" \
  SPAWN_ALLOCATOR=1 \
  tools/install_spawn_adapter_launchd.sh
tools/uninstall_spawn_adapter_launchd.sh
```

Environment overrides:
- `AGENTD_BIN` (default `./build/agentd`)
- `AGENTD_HOST`, `AGENTD_PORT`
- `AGENTD_STATE_DIR`, `AGENTD_DB_PATH`
- `AGENTD_DOTENV_PATH` (optional override for provider key discovery)
- `AGENTD_AUTH_TOKEN`, `AGENTD_AUTH_COOKIE`
- `AGENTD_TOOLS`, `AGENTD_YOLO`
- `AGENTD_HOST_SCOPE`, `AGENTD_TOOLS_ROOT` (legacy optional args; unset by default)
- `AGENTD_CORS_ORIGINS` (comma-separated)
- `AGENTD_CORS_ALLOW_HEADERS`, `AGENTD_CORS_ALLOW_METHODS`
- `AGENTD_CORS_ALLOW_CREDENTIALS`, `AGENTD_CORS_MAX_AGE_SECONDS`
- `AGENTD_CORS_ROUTES` (JSON array of `{path_prefix, origins}`)
- `AGENTD_UPLOAD_MAX_BYTES` (per-file session upload cap)
- `AGENTD_EXTRA_ARGS` (space-delimited)

Autonomous launchd overrides:
- `ORCHESTRATOR_BIN`, `ORCHESTRATOR_PLIST_PATH`, `ORCHESTRATOR_LOG_DIR`
- `ORCHESTRATOR_POLL_INTERVAL`, `ORCHESTRATOR_STATUS`, `ORCHESTRATOR_LIMIT`
- `ORCHESTRATOR_INCLUDE_PLANNED`, `ORCHESTRATOR_ID`, `ORCHESTRATOR_EXTRA_ARGS`
- `SPAWN_ADAPTER_BIN`, `SPAWN_ADAPTER_PLIST_PATH`, `SPAWN_ADAPTER_LOG_DIR`
- `SPAWN_ADAPTER_POLL_INTERVAL`, `SPAWN_ADAPTER_STATUS`, `SPAWN_ADAPTER_LIMIT`
- `SPAWN_ADAPTER_COMMAND_TIMEOUT`, `SPAWN_ADAPTER_EXTRA_ARGS`
- `SPAWN_COMMAND`, `SPAWN_ALLOCATOR`, `SPAWN_ADAPTER_ID`

### OTA updates (agentd)

OTA is **disabled by default**. Enable it explicitly and provide an update command:

- `--ota-enable` (or env `AGENTD_OTA_ENABLE=1`)
- `--ota-command /path/to/tools/agentd_ota_apply.sh` (or env `AGENTD_OTA_COMMAND`)

The reference script reads a plan file from `AGENTD_OTA_PLAN_PATH` and uses optional env hints:

- `AGENTD_OTA_TARGET_BIN` — target binary path to replace (recommended)
- `AGENTD_OTA_RESTART` — `systemd` | `launchd` | `signal` (default: `signal`)
- `AGENTD_OTA_SERVICE` — service name/label for restart (systemd/launchd)

Example (systemd):
```
Environment=AGENTD_OTA_ENABLE=1
Environment=AGENTD_OTA_COMMAND=/opt/agentd/tools/agentd_ota_apply.sh
Environment=AGENTD_OTA_TARGET_BIN=/usr/local/bin/agentd
Environment=AGENTD_OTA_RESTART=systemd
Environment=AGENTD_OTA_SERVICE=agentd
```

Trigger via API:

- `POST /api/v1/ota/update` with `{ url, sha256?, version?, reason?, drain_timeout_ms? }`
- `GET /api/v1/ota/status` for current state

OTA enters **drain mode** while the update runs:
- new run/workflow submissions return `HTTP 503` + `drain_*` hints
- job/workflow schedulers pause claiming new tasks
- `GET /api/v1/ota/status` surfaces `drain_active`, `drain_until_unix_ms`, `drain_reason`
- Status also reports best-effort queue pressure (`jobs_running`, `jobs_queued`, `workflow_tasks_running`,
  `workflow_tasks_queued`, `workflows_running`) when the DB is available.
- agentd waits (best-effort) for **running jobs/workflow tasks** to finish until `drain_timeout_ms` elapses; any remaining
  work is resumed after restart.

WebUI + broker fanout:
- In broker mode, the WebUI lists connected deployments and can send OTA requests to **all selected deployments** using broker bulk OTA endpoints:
  - `POST /v1/agents/{agent_id}/ota/update` (body may include `deployment_ids`).
  - `GET  /v1/agents/{agent_id}/ota/status` (query `deployment_ids=...` optional).
- Status checks also fan out per deployment, so operators can verify drain/rollout progress without logging into each host.

Local verification:
- `tools/verify_ota_continuity.sh` runs a delay workflow, triggers an OTA update, restarts agentd, and confirms the workflow resumes.

### Native codexw broker connector as a service

The native codexw connector is intentionally a foreground protocol adapter, not
a lifetime manager. In production, run `agentd` and the connector as separate
OS-managed services:

- `agentd.service` owns the local daemon API and durable state.
- `agentd-codexw-connector.service` owns the outbound broker websocket and
  exits on unrecoverable broker/local failures.
- `systemd`, `launchd`, Docker Compose, Kubernetes, or another process manager
  owns restart/backoff, boot startup, log collection, and health supervision.

Do not enable connector `--reconnect` in service units. Keep reconnect for
manual foreground debugging only; service managers already have better,
observable restart policy.

Systemd service templates are provided under `packaging/systemd/`:

```
sudo install -d -o agentd -g agentd /var/lib/agentd /etc/agentd
sudo install -m 0644 packaging/systemd/agentd.service /etc/systemd/system/agentd.service
sudo install -m 0644 packaging/systemd/agentd-codexw-connector.service /etc/systemd/system/agentd-codexw-connector.service
sudo install -m 0644 packaging/systemd/agentd-codexw-connector-self-test.service /etc/systemd/system/agentd-codexw-connector-self-test.service
sudo install -m 0644 packaging/systemd/agentd-codexw-connector-self-test.timer /etc/systemd/system/agentd-codexw-connector-self-test.timer
sudo install -m 0600 packaging/systemd/agentd.env.example /etc/agentd/agentd.env
sudo install -m 0600 packaging/systemd/codexw-connector.env.example /etc/agentd/codexw-connector.env
sudo systemctl daemon-reload
sudo systemctl enable --now agentd.service agentd-codexw-connector.service
```

The connector environment file contains first-boot broker credentials only so
it can mint a deployment enrollment token and persist the broker-signed
deployment certificate under `AGENTD_CODEXW_IDENTITY_DIR`. After the certificate
exists, remove the bootstrap password from `/etc/agentd/codexw-connector.env`
and restart the connector.

The same connector binary has a read-only readiness mode:

```
sudo -u agentd sh -c '
  set -a
  . /etc/agentd/codexw-connector.env
  set +a
  python3 /opt/agentd/tools/agentd_codexw_native_broker_connector.py \
    --self-test --require-broker-visible
'
```

It verifies local identity files, local `agentd` health, the shared
session/event adapter surfaces, and, when broker credentials or
`AGENTD_CODEXW_BROKER_TOKEN` are available, that the broker currently reports
this runtime instance online. Enable
`agentd-codexw-connector-self-test.timer` when you want systemd to run that
check every five minutes after the connector service is installed. After
first-boot certificate enrollment, prefer a read-only broker token for the
self-test and remove the bootstrap username/password from the env file.
For hosts that intentionally enable `AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota`,
set `AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT=1` as well. That keeps the periodic
self-test read-only while requiring the broker to accept
`/api/v2/runtime-instances/{id}/actions/preflight` for `runtime.update`, return
`mutates_runtime:false`, and prepare an update payload that matches the current
`/status` candidate.
The macOS installer writes a companion LaunchAgent at
`${AGENTD_CODEXW_SELF_TEST_PLIST_PATH}` by default, using label
`${AGENTD_CODEXW_LABEL}.self-test` and `StartInterval=300`. It runs the same
`--self-test --require-broker-visible` command as the systemd timer and adds
`--require-update-preflight` when `AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT=1`.
Set `AGENTD_CODEXW_INSTALL_SELF_TEST=0` only when another scheduler owns this
readiness check.

After installation, generate a single readiness report from the installed
profile:

```
python3 /opt/agentd/tools/verify_agentd_codexw_connector_service.py --json
```

On macOS the verifier reads the launchd connector/self-test plists, calls
`launchctl print` unless `--skip-supervisor` is provided, tails the configured
connector logs, and runs the installed self-test command. On Linux it reads
`/etc/agentd/codexw-connector.env`, calls `systemctl show` and recent
`journalctl` self-test logs, then runs the same read-only self-test command.
The report redacts token/password/secret environment values and includes the
broker-visible and optional `runtime.update` preflight checks in one JSON
object.

Broker-driven runtime updates are disabled by default. To expose
`runtime.update` through the shared codexw broker, first enable the daemon OTA
path (`AGENTD_OTA_ENABLE=1`, `AGENTD_OTA_COMMAND`, `AGENTD_OTA_TARGET_BIN`,
and a restart policy such as `AGENTD_OTA_RESTART=systemd` with
`AGENTD_OTA_SERVICE=agentd`). Then set
`AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota` on the connector service. In
that mode the connector's readiness self-test also checks
`GET /api/v1/ota/status`, and broker `runtime.update` commands are forwarded
to `POST /api/v1/ota/update` with `{url, sha256?, version?, reason?,
drain_timeout_ms?}`. Leave the mode as `disabled` on hosts that cannot prove
this local OTA boundary. `runtime.restart` and compatibility `runtime.upgrade`
remain unadvertised by `agentd`.
The shared adapter also serves `GET /api/v1/runtime/status`; the codexw broker
uses that read-only route to show OTA state, drain state, and queue pressure in
iOS/macOS and WebUI before an operator runs `runtime.update`. Include a
`candidate`, `update_candidate`, `release`, or `artifact` object in
`/api/v1/ota/status` when a concrete OTA target is available; the bridge maps
`url`, `sha256`, `version`, and `drain_timeout_ms` into
`update.candidate.input`, and the broker uses that status-derived input when it
forwards the confirmed update action.
The durable connector self-test can continuously verify that same broker-side
preparation without mutating the daemon by setting
`AGENTD_CODEXW_REQUIRE_UPDATE_PREFLIGHT=1`.

Before enabling that mode on a durable production connector, verify the shared
broker update path with the non-mutating live proof:

```
tools/verify_codexw_live_agentd_ota_candidate.sh
```

The proof starts a temporary loopback fake `agentd`/OTA API, connects the native
connector to the live codexw broker with runtime update mode set to
`agentd_ota`, verifies
`/api/v2/runtime-instances/{id}/status` exposes `update.candidate`, verifies
the broker advertises an enabled `runtime.update` descriptor, calls
`/api/v2/runtime-instances/{id}/actions/preflight`, and checks that the
non-mutating preflight response contains the broker-prepared `{url, sha256,
version, reason, drain_timeout_ms}` body. The fake OTA update endpoint must not
receive a request in the default proof; set `VERIFY_AGENTD_OTA_DISPATCH=1` only
when you intentionally want to dispatch to the fake endpoint as a separate
compatibility check. It removes the temporary broker deployment by default and
does not replace any daemon binary.

The service units intentionally read `AGENTD_AUTH_TOKEN`,
`AGENTD_CODEXW_BROKER_PASSWORD`, and one-time enrollment secrets from the
environment file instead of passing them as command-line arguments. Keep that
property intact; process arguments are routinely visible in system process
lists. The same rule applies to the macOS launchd helper, which places runtime
secrets under the plist `EnvironmentVariables` block rather than
`ProgramArguments`.

macOS launchd helpers mirror the two-service layout:

```
AGENTD_AUTH_TOKEN="REPLACE_WITH_RANDOM_TOKEN" \
  tools/install_agentd_launchd.sh

AGENTD_BASE_URL="http://127.0.0.1:8123" \
AGENTD_AUTH_TOKEN="REPLACE_WITH_RANDOM_TOKEN" \
AGENTD_CODEXW_BROKER_URL="https://broker.example" \
AGENTD_CODEXW_DEPLOYMENT_ID="agentd-mac" \
AGENTD_CODEXW_BROKER_USER="admin" \
AGENTD_CODEXW_BROKER_PASSWORD="REPLACE_WITH_FIRST_BOOT_PASSWORD" \
  tools/install_agentd_codexw_connector_launchd.sh
```

Uninstall with:

```
tools/uninstall_agentd_codexw_connector_launchd.sh
tools/uninstall_agentd_launchd.sh
```

### Example (Windows service)

#### Option A: `sc.exe` (built-in)
Run from an **elevated** Command Prompt:
```
setx AGENTD_DOTENV_PATH "C:\\ProgramData\\agentd\\agentd.env" /M
sc.exe create agentd binPath= "\"C:\\Program Files\\agentd\\agentd.exe\" --host 127.0.0.1 --port 8123 --auth-token REPLACE_WITH_RANDOM_TOKEN --state-dir \"C:\\ProgramData\\agentd\" --db-path \"C:\\ProgramData\\agentd\\agentd.db\"" start= auto
sc.exe config agentd start= delayed-auto
sc.exe failure agentd reset= 86400 actions= restart/5000/restart/5000/restart/5000
```

#### Option B: PowerShell `New-Service`
Run from an **elevated** PowerShell prompt:
```
[Environment]::SetEnvironmentVariable("AGENTD_DOTENV_PATH", "C:\\ProgramData\\agentd\\agentd.env", "Machine")
$exe = "C:\\Program Files\\agentd\\agentd.exe"
$args = "--host 127.0.0.1 --port 8123 --auth-token REPLACE_WITH_RANDOM_TOKEN --state-dir `\"C:\\ProgramData\\agentd`\" --db-path `\"C:\\ProgramData\\agentd\\agentd.db`\""
New-Service -Name "agentd" -BinaryPathName "`"$exe`" $args" -StartupType Automatic
```

### If exposing `agentd` directly
- Put it behind a reverse proxy with TLS (nginx/Caddy/Envoy).
- **Do not** bind directly to `0.0.0.0` without `--auth-token`.
- Use strict CORS allowlists.
- If using cookie auth, enable CORS credentials (`--cors-allow-credentials`).
- Set HTTP safety limits via env:
  - `AGENTD_HTTP_MAX_BODY_BYTES` (request body cap)
  - `AGENTD_HTTP_MAX_HEADER_BYTES` (request header cap)
  - `AGENTD_HTTP_READ_TIMEOUT_MS` (read timeout for slow clients)

---

## Broker + Connector

### Broker requirements
- TLS enabled (`--tls-cert`, `--tls-key`)
- mTLS enabled for connectors (`--tls-client-ca`)
- OIDC enabled for users (`--oidc-issuer`, `--oidc-audience`)
- Durable DB (Postgres) with backups
- HTTP tunables:
  - `--max-body-bytes`, `--max-header-bytes`
  - `--read-timeout`, `--write-timeout`, `--idle-timeout`, `--read-header-timeout`
  - Env overrides: `AGENTD_BROKER_MAX_BODY_BYTES`, `AGENTD_BROKER_MAX_HEADER_BYTES`,
    `AGENTD_BROKER_READ_TIMEOUT_MS`, `AGENTD_BROKER_WRITE_TIMEOUT_MS`,
    `AGENTD_BROKER_IDLE_TIMEOUT_MS`, `AGENTD_BROKER_READ_HEADER_TIMEOUT_MS`
- Browser clients:
  - CORS allowlist with `--cors-origin` / `--cors-origins`
  - Cookie auth (optional): `--auth-cookie <name>` + `--cors-allow-credentials`

### Connector requirements
- Unique `--agent-id` per agentd instance
- mTLS client certs issued by broker CA
- Runs close to the agentd instance (same host or VPC)

### Example (broker flags)
```
agentd-broker \
  --listen :8443 \
  --tls-cert /etc/agentd/tls/server.pem \
  --tls-key /etc/agentd/tls/server.key.pem \
  --tls-client-ca /etc/agentd/tls/ca.pem \
  --db-dsn postgres://... \
  --oidc-issuer https://id.example.com/realms/agentd \
  --oidc-audience agentd-broker \
  --cors-origin https://ui.example.com
```

Optional (non-UI service clients): static client token file:
```
agentd-broker \
  --client-auth-file /etc/agentd/client_auth.json \
  --client-auth-fallback \
  --client-auth-reload-ms 60000 \
  --client-auth-strict \
  --client-auth-max-age-ms 120000
```

Env equivalents:
- `AGENTD_BROKER_CLIENT_AUTH_FILE=/etc/agentd/client_auth.json`
- `AGENTD_BROKER_CLIENT_AUTH_FALLBACK=1`
- `AGENTD_BROKER_CLIENT_AUTH_RELOAD_MS=60000`
- `AGENTD_BROKER_CLIENT_AUTH_STRICT=1`
- `AGENTD_BROKER_CLIENT_AUTH_MAX_AGE_MS=120000`
- `AGENTD_BROKER_CLIENT_AUTH_ALLOW_AUTOMATION=1` (allow admin client tokens for orchestration endpoints)

You can also send `SIGHUP` to reload the client auth file immediately.

### Autonomous services (orchestrator + spawn adapter)

For fully automatic operation without a UI, run the broker with client auth enabled and
allow admin client tokens to access orchestration endpoints.

Local automouse preset (compose + autonomous services + WebUI automation defaults):

```
tools/automouse_pack.sh
```

Or via compose directly:

```
COMPOSE_AUTOMOUSE=1 tools/verify_compose_stack.sh
```

Example (systemd):
```
[Unit]
Description=agentd orchestrator loop
After=network.target

[Service]
User=agentd
Environment=BROKER_BASE=https://broker.example.com
Environment=BROKER_OIDC_TOKEN=REPLACE_WITH_SERVICE_TOKEN
Environment=BROKER_INSECURE_TLS=0
ExecStart=/usr/local/bin/agentd-orchestrator --poll-interval 5s
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```
[Unit]
Description=agentd spawn adapter
After=network.target

[Service]
User=agentd
Environment=BROKER_BASE=https://broker.example.com
Environment=BROKER_OIDC_TOKEN=REPLACE_WITH_SERVICE_TOKEN
Environment=SPAWN_ALLOCATOR=1
Environment=BROKER_INSECURE_TLS=0
ExecStart=/usr/local/bin/agentd-spawn-adapter --poll-interval 3s --allocator
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Notes:
- `BROKER_OIDC_TOKEN` can be a static client token when the broker is configured with
  `--client-auth-file` + `--client-auth-fallback` + `--client-auth-allow-automation`.
- `BROKER_OIDC_TOKEN_FILE` can be used instead; the orchestrator/spawn adapter will
  read the token file on each request (best-effort).
- For OIDC-only deployments, use a token refresh sidecar to keep the token file current.

Example refresh helper (Keycloak/password grant):
```
tools/oidc_token_refresh.sh \
  --issuer https://id.example.com/realms/agentd \
  --client-id agentd-broker \
  --user orchestration \
  --password 'REDACTED' \
  --output /etc/agentd/broker_oidc_token.txt
```

Container-friendly refresh (broker image):
```
agentd-oidc-refresh \
  --issuer https://id.example.com/realms/agentd \
  --client-id agentd-broker \
  --user orchestration \
  --password 'REDACTED' \
  --output /etc/agentd/broker_oidc_token.txt
```

On macOS, the launchd helper scripts mirror these services:
```
BROKER_BASE="https://broker.example.com" \
  BROKER_OIDC_TOKEN="REPLACE_WITH_SERVICE_TOKEN" \
  tools/install_orchestrator_launchd.sh

BROKER_BASE="https://broker.example.com" \
  BROKER_OIDC_TOKEN="REPLACE_WITH_SERVICE_TOKEN" \
  SPAWN_ALLOCATOR=1 \
  tools/install_spawn_adapter_launchd.sh

BROKER_BASE="https://broker.example.com" \
  BROKER_OIDC_TOKEN_FILE="/etc/agentd/broker_oidc_token.txt" \
  tools/install_orchestrator_launchd.sh
```

### Example (connector flags)
```
agentd-connector \
  --broker wss://broker.example.com/v1/agent/connect \
  --local-agentd http://127.0.0.1:8123 \
  --local-agentd-token "$AGENTD_AUTH_TOKEN" \
  --tls-ca /etc/agentd/tls/ca.pem \
  --tls-cert /etc/agentd/tls/client.pem \
  --tls-key /etc/agentd/tls/client.key.pem \
  --agent-id agent-123
```

### Connector status helper (optional)
Use `agentd-connector-status` to publish connector health into the broker registry:

```
agentd-connector-status \
  --broker-base https://broker.example.com \
  --auth-token "$BROKER_ADMIN_TOKEN" \
  --connector-id slack \
  --status ready \
  --interval 30s
```

---

## macOS (MacBook M2) — broker + WebUI

Recommended on macOS: run **broker + connector + WebUI** via Docker Compose, and keep `agentd` as a local launchd service.

### Prereqs
- Docker Desktop or Colima
- `docker compose` available

### Local prod-like stack (broker + connector + WebUI)
```
cp project.local.md.example project.local.md
export WEBUI_PUBLISHED_PORT=8100
export AGENTD_PUBLISHED_PORT=8123
export BROKER_PUBLISHED_PORT=8443
export KEYCLOAK_PUBLISHED_PORT=8081
docker compose up -d postgres keycloak broker connector webui
```

Then run `agentd` locally (launchd or foreground):
```
./build/agentd --host 127.0.0.1 --port 8123 --auth-token "REPLACE_WITH_RANDOM_TOKEN" --state-dir "$HOME/Library/Application Support/agentd" --db-path "$HOME/Library/Application Support/agentd/agentd.db"
```

### Manual verification (macOS)
1) Broker health:
```
curl -k https://127.0.0.1:8443/healthz
```
2) Agentd health:
```
curl http://127.0.0.1:8123/api/v1/health
```
3) WebUI bring-up:
- Open `http://127.0.0.1:8100`
- Set **Base URL** to `http://127.0.0.1:8123`
- Set **Daemon Auth Token** to the `--auth-token` value above
4) Functional check:
- Run a short prompt that emits an audio artifact (or scene audio).
- If autoplay is blocked by the browser, click once in the UI to unlock media playback.

### Notes
- Broker TLS in compose uses test certificates under `tools/_compose_mtls` (local only).
- WebUI expects to talk to broker proxy or direct agentd; align CORS + auth tokens accordingly.
- Compose mounts `tools/agentui-config.compose.js` into the WebUI container to default to broker mode.
  - Broker mode exposes a broker console panel for agent selection + membership management + audit.
- `docker-compose.yml` caps Keycloak heap (`JAVA_OPTS_KC_HEAP`) for low-memory hosts; override if you need more RAM.

### WebUI Playwright smoke (optional)

From `ui/`:
- `npm run e2e:agentd` (agentd host UI smoke, including workflow schedules)
- `npm run e2e:broker` (broker console UI smoke)

From repo root (headless capture + traces/videos/screenshots):
- `tools/run_ui_playwright_smoke.sh`

Playwright auto-starts an isolated local Vite server for these UI runs unless
`AGENT_E2E_UI_BASE_URL` is set. The default auto-start port is `4173`; override
it with `AGENT_E2E_UI_PORT=<port>` if you need a different local port.

### Evidence bundles (optional)

Capture a lightweight snapshot for debugging or attachable evidence:
- `tools/capture_agent_evidence_bundle.sh --agentd-base http://127.0.0.1:8123`

Validate a captured bundle:
- `python3 tools/check_agent_evidence_bundle.py --dir docs/artifacts/evidence/<bundle_dir>`

### Scenario runner (optional)

Run a JSON scenario that captures logs + evidence:
- `python3 tools/scenario_runner.py --file tools/scenarios/agentd_smoke.json`

Run all scenarios and validate evidence bundles:
- `python3 tools/scenario_pack.py --dir tools/scenarios --validate`

### One-command devstack (optional)

Bring up agentd + broker + connector + WebUI on the host (Postgres + Keycloak via Docker), then run smoke checks + capture evidence:
- `tools/devstack_agent.sh`
  - Re-running it now reuses the canonical live stack from `out/devstack_state.json` instead of starting a second broker/WebUI.
  - Use `tools/devstack_agent.sh --restart` when you intentionally want to replace the current canonical stack.

Stop the stack:
- `tools/devstack_agent_down.sh` (use `--wipe-volumes` to reset Keycloak and invalidate existing tokens)
  - Edit that file to adjust broker URL, agent id, or pass-through daemon token for local dev.
- Inspect the live/stale state quickly with `tools/devstack_status.sh`.
- WebUI serving in `tools/devstack_agent.sh` and `tools/verify_mac_full_stack_host.sh` uses `python -m http.server`.
  If no Python is available, the scripts skip WebUI serve and continue.
- For a one-command macOS verification run, use: `tools/verify_mac_full_stack.sh`.
- If Docker is unavailable or resource-constrained, you can verify the local stack without compose:
- `tools/verify_compose_stack.sh` warns when Docker memory is under 4 GiB (Keycloak may OOM).
  - `tools/verify_mac_local_stack.sh` (agentd + WebUI only; no broker)
  - Optional env:
    - `MAC_LOCAL_SKIP_UI=1` to skip WebUI build/serve
    - `MAC_LOCAL_UI_INSTALL=0` to skip `npm ci` when deps already exist
    - `MAC_LOCAL_PROVIDER_TEST=1` to run diagnostics provider tests if keys are available
    - `MAC_LOCAL_PROVIDER_TEST_TIMEOUT_MS=30000` to override provider test timeout
    - `AGENTD_TOOLS=basic|host|none` to override the agentd tools mode (provider tests switch to `basic` by default)
- If Docker build is blocked but Docker itself runs, you can verify a host-mode full stack:
  - `tools/verify_mac_full_stack_host.sh` (runs Postgres + Keycloak via Docker, and runs agentd/broker/connector/WebUI on the host)
  - Optional env: `HOST_STACK_SKIP_UI=1` to skip WebUI build/serve, `HOST_STACK_UI_INSTALL=0` to skip `npm ci` when deps already exist
- If Docker/Keycloak are unavailable but local Postgres is running, you can verify a full stack with client auth:
  - `tools/verify_mac_full_stack_local_postgres.sh` (local Postgres + broker client auth; seeds broker DB directly)
  - Optional env: `HOST_STACK_SKIP_UI=1`, `HOST_STACK_UI_INSTALL=0`, `HOST_STACK_PG_DSN=postgres://user@127.0.0.1:5432/agentd_broker?sslmode=disable`
- If `tools/verify_mac_full_stack.sh` skips due to Docker build resource errors (e.g. `unpigz`/`runc`),
  restart Docker Desktop or increase CPU/RAM (Docker Desktop → Settings → Resources; also ensure disk image size).
  The script already retries builds, can fall back to the
  legacy builder, and throttles pigz threads. You can also tweak:
  - `COMPOSE_BUILD_SERIAL=1` (default) to reduce concurrency
  - `COMPOSE_BUILD_RETRIES=3` (default) to raise retry attempts
  - `PIGZ=-p1 GZIP=-p1` to reduce decompression thread pressure
  - `COMPOSE_BUILD=0` to skip image rebuilds when you already have fresh images (requires images present; otherwise it skips)
  - `COMPOSE_PULL=1` to auto-pull missing images when `COMPOSE_BUILD=0`
- If `docker info` hangs (daemon not responding), set `AGENT_DOCKER_INFO_TIMEOUT_SEC` (default `5`) to shorten the
  readiness check or confirm Docker Desktop/Colima is running.
- On macOS, the verify scripts will attempt to launch Docker Desktop automatically when the daemon is down.
  Control this behavior with:
  - `AGENT_DOCKER_AUTOSTART=1` (default) to `open -a Docker` and wait for the daemon
  - `AGENT_DOCKER_AUTOSTART=0` to disable auto-start
  - `AGENT_DOCKER_STARTUP_TIMEOUT_SEC=120` to adjust the wait time
- Prebuilt images (optional):
  - set `BROKER_IMAGE`, `AGENTD_IMAGE`, `CONNECTOR_IMAGE`, `WEBUI_IMAGE` to registry tags
  - run `COMPOSE_BUILD=0 COMPOSE_PULL=1 ./tools/verify_compose_stack.sh`
- Port selection note: `tools/verify_compose_stack.sh` prefers the defaults
  (8443/5433/8123/8100/8081) but falls back to random high ports if those are
  already in use. Pin ports explicitly with `BROKER_PUBLISHED_PORT`,
  `KEYCLOAK_PUBLISHED_PORT`, `POSTGRES_PUBLISHED_PORT`, `AGENTD_PUBLISHED_PORT`,
  `WEBUI_PUBLISHED_PORT`.

---

## Manual verification (macOS)

1) Broker health (if using broker/connector):
```
curl -k https://127.0.0.1:8443/healthz
```

2) Agentd health:
```
curl http://127.0.0.1:8123/api/v1/health
curl http://127.0.0.1:8123/api/v1/ready
curl http://127.0.0.1:8123/metrics
```

Optional diagnostics (requires auth if enabled):
```
curl http://127.0.0.1:8123/api/v1/diagnostics
curl http://127.0.0.1:8123/api/v1/diagnostics/providers
```

See `docs/DIAGNOSTICS.md` for provider_test usage. The providers endpoint includes `base_url_source`
(`config` / `env` / `default`) to explain how a provider base URL was selected.

3) UI functional check:
- Run a short prompt that emits audio (artifact or scene).
- If autoplay is blocked, click once in the UI to unlock media playback.

## WebUI

- Build once and host on a static server:
  - `cd ui && npm ci && npm run build`
- Serve `ui/dist/` via nginx/Caddy/S3.
- Runtime defaults (no rebuild): edit `ui/dist/agentui-config.js` (supports `serverPrefsMode` = `off|auto|on`).
- Build-time overrides (optional): `VITE_AGENTD_BASE_URL`, `VITE_BROKER_BASE_URL`, and `VITE_AGENTUI_*`.
- Broker proxy is recommended for production; direct agentd exposure requires CORS + auth hardening.
- Full WebUI setup (dev, runtime config, diagnostics, reliability notes) lives in `docs/WEBUI.md`.
- Session selection, history UI state, job resume, and scene cache are scoped per profile (and base URL) to avoid
  collisions when switching between deployments.

---

## Operational checklist

- **Backups**: SQLite (`agentd.db`) + Postgres (broker).
- **Logs**: rotate daemon + broker logs.
- **Monitoring**: scrape `/healthz`, `/readyz`, `/metrics` (broker) + `/api/v1/health`, `/api/v1/ready`, `/metrics` (agentd).
- **Access logs**: set `AGENTD_ACCESS_LOG=1` (text) or `AGENTD_ACCESS_LOG=json` for machine-parsable access logs.
- **Resource limits**: set process limits and container quotas.
- **Upgrades**: keep `agentd` and WebUI in lockstep (OpenAPI + protocol changes).

---

## Docker stack hardening checklist

- **Secrets**: keep provider keys out of images; inject via env or Docker/Swarm secrets.
- **TLS**: terminate TLS at broker; use valid certs in production (replace test CA).
- **mTLS**: ensure connector certs are per-agent and rotated.
- **Least exposure**: only publish broker/WebUI ports; keep agentd private.
- **Backups**: snapshot Postgres + agentd SQLite volume.
- **Observability**: log rotation + metrics scraping.
- **Image pinning**: pin broker/agentd build images by digest for deterministic rollouts.

---

## Local prod-like stack

Use `docker-compose.yml` + `tools/verify_compose_stack.sh` for a local, production-like integration test of:
Postgres + Keycloak + broker + connector + agentd + WebUI.
