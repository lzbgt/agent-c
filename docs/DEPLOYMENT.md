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
- **Secrets**: keep provider keys out of the UI; load via `.not_in_repo` or env.
- **HTTP tasks**: keep `--workflow-enable-http-tasks` **off** unless needed; if enabled, set:
  - `--workflow-http-allow-host ...` and/or `--workflow-http-allow-cidr ...`
  - `--workflow-http-deny-private` (recommended)
  - `--workflow-http-dns-pin` (recommended)

### Example (systemd)
```
[Unit]
Description=agentd daemon
After=network.target

[Service]
User=agentd
WorkingDirectory=/var/lib/agentd
ExecStart=/usr/local/bin/agentd \
  --host 127.0.0.1 \
  --port 8123 \
  --auth-token ${AGENTD_AUTH_TOKEN} \
  --state-dir /var/lib/agentd \
  --db-path /var/lib/agentd/agentd.db
Restart=always
RestartSec=3
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

### Example (macOS launchd)
```
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.agentd.daemon</string>
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

Environment overrides:
- `AGENTD_BIN` (default `./build/agentd`)
- `AGENTD_HOST`, `AGENTD_PORT`
- `AGENTD_STATE_DIR`, `AGENTD_DB_PATH`
- `AGENTD_TOOLS`, `AGENTD_YOLO`
- `AGENTD_HOST_SCOPE`, `AGENTD_TOOLS_ROOT`
- `AGENTD_CORS_ORIGINS` (comma-separated)
- `AGENTD_UPLOAD_MAX_BYTES` (per-file session upload cap)
- `AGENTD_EXTRA_ARGS` (space-delimited)

### Example (Windows service)

#### Option A: `sc.exe` (built-in)
Run from an **elevated** Command Prompt:
```
sc.exe create agentd binPath= "\"C:\\Program Files\\agentd\\agentd.exe\" --host 127.0.0.1 --port 8123 --auth-token REPLACE_WITH_RANDOM_TOKEN --state-dir \"C:\\ProgramData\\agentd\" --db-path \"C:\\ProgramData\\agentd\\agentd.db\"" start= auto
sc.exe config agentd start= delayed-auto
sc.exe failure agentd reset= 86400 actions= restart/5000/restart/5000/restart/5000
```

#### Option B: PowerShell `New-Service`
Run from an **elevated** PowerShell prompt:
```
$exe = "C:\\Program Files\\agentd\\agentd.exe"
$args = "--host 127.0.0.1 --port 8123 --auth-token REPLACE_WITH_RANDOM_TOKEN --state-dir `\"C:\\ProgramData\\agentd`\" --db-path `\"C:\\ProgramData\\agentd\\agentd.db`\""
New-Service -Name "agentd" -BinaryPathName "`"$exe`" $args" -StartupType Automatic
```

### If exposing `agentd` directly
- Put it behind a reverse proxy with TLS (nginx/Caddy/Envoy).
- **Do not** bind directly to `0.0.0.0` without `--auth-token`.
- Use strict CORS allowlists.
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
  --oidc-audience agentd-broker
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

You can also send `SIGHUP` to reload the client auth file immediately.

### Example (connector flags)
```
agentd-connector \
  --broker wss://broker.example.com/v1/agent/connect \
  --local-agentd http://127.0.0.1:8123 \
  --tls-ca /etc/agentd/tls/ca.pem \
  --tls-cert /etc/agentd/tls/client.pem \
  --tls-key /etc/agentd/tls/client.key.pem \
  --agent-id agent-123
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
  - Edit that file to adjust broker URL, agent id, or pass-through daemon token for local dev.
- For a one-command macOS verification run, use: `tools/verify_mac_full_stack.sh`.
- If Docker is unavailable or resource-constrained, you can verify the local stack without compose:
  - `tools/verify_mac_local_stack.sh` (agentd + WebUI only; no broker)
  - Optional env: `MAC_LOCAL_SKIP_UI=1` to skip WebUI build/serve, `MAC_LOCAL_UI_INSTALL=0` to skip `npm ci` when deps already exist
- If Docker build is blocked but Docker itself runs, you can verify a host-mode full stack:
  - `tools/verify_mac_full_stack_host.sh` (runs Postgres + Keycloak via Docker, and runs agentd/broker/connector/WebUI on the host)
- If `tools/verify_mac_full_stack.sh` skips due to Docker build resource errors (e.g. `unpigz`/`runc`),
  restart Docker Desktop or increase CPU/RAM. The script already retries builds, can fall back to the
  legacy builder, and throttles pigz threads. You can also tweak:
  - `COMPOSE_BUILD_SERIAL=1` (default) to reduce concurrency
  - `COMPOSE_BUILD_RETRIES=3` (default) to raise retry attempts
  - `PIGZ=-p1 GZIP=-p1` to reduce decompression thread pressure
  - `COMPOSE_BUILD=0` to skip image rebuilds when you already have fresh images (requires images present; otherwise it skips)
  - `COMPOSE_PULL=1` to auto-pull missing images when `COMPOSE_BUILD=0`
- Prebuilt images (optional):
  - set `BROKER_IMAGE`, `AGENTD_IMAGE`, `CONNECTOR_IMAGE`, `WEBUI_IMAGE` to registry tags
  - run `COMPOSE_BUILD=0 COMPOSE_PULL=1 ./tools/verify_compose_stack.sh`

---

## WebUI (static)

- Build once and host on a static server:
  - `cd ui && npm ci && npm run build`
- Serve `ui/dist/` via nginx/Caddy/S3.
- Runtime defaults (no rebuild): edit `ui/dist/agentui-config.js` to set:
  - `connectionMode` (`direct` or `broker`)
  - `daemonBaseUrl`, `brokerBaseUrl`, `brokerAgentId`
  - `daemonAuthToken`, `brokerAuthToken` (if you accept putting tokens in a static file)
  - `model`, `baseUrl`, `proxyUrl`, `timeoutMs`
  - `tools`, `yolo`, `hostPolicy`, `verbose`
  - `allowClientRpcs`, `allowClientEffects`, `allowUnsafePageEval`
- Build-time overrides (optional): `VITE_AGENTD_BASE_URL`, `VITE_BROKER_BASE_URL`, and `VITE_AGENTUI_*` variants above.
- Configure UI to talk to:
  - Broker proxy (recommended), or
  - Direct agentd base URL (ensure CORS + auth token).

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
