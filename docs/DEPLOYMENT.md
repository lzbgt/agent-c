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

### If exposing `agentd` directly
- Put it behind a reverse proxy with TLS (nginx/Caddy/Envoy).
- **Do not** bind directly to `0.0.0.0` without `--auth-token`.
- Use strict CORS allowlists.

---

## Broker + Connector

### Broker requirements
- TLS enabled (`--tls-cert`, `--tls-key`)
- mTLS enabled for connectors (`--tls-client-ca`)
- OIDC enabled for users (`--oidc-issuer`, `--oidc-audience`)
- Durable DB (Postgres) with backups

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

## WebUI (static)

- Build once and host on a static server:
  - `cd ui && npm ci && npm run build`
- Serve `ui/dist/` via nginx/Caddy/S3.
- Configure UI to talk to:
  - Broker proxy (recommended), or
  - Direct agentd base URL (ensure CORS + auth token).

---

## Operational checklist

- **Backups**: SQLite (`agentd.db`) + Postgres (broker).
- **Logs**: rotate daemon + broker logs.
- **Monitoring**: scrape `/healthz` (broker) + `/api/v1/health` (agentd).
- **Resource limits**: set process limits and container quotas.
- **Upgrades**: keep `agentd` and WebUI in lockstep (OpenAPI + protocol changes).

---

## Local prod-like stack

Use `docker-compose.yml` + `tools/verify_compose_stack.sh` for a local, production-like integration test of:
Postgres + Keycloak + broker + connector + agentd + WebUI.

