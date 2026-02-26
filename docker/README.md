# Docker Compose stack (prod-like local verification)

This folder contains a Docker Compose setup that spins up a **prod-like** stack for local verification.
For real production deployment guidance, see `docs/DEPLOYMENT.md`.

macOS: Docker Desktop or Colima is required. Prefer running `agentd` locally and broker/webui via compose, or use `tools/verify_mac_full_stack.sh`.
If Docker is unavailable, `tools/verify_mac_local_stack.sh` verifies agentd + WebUI without broker.

- Postgres (broker DB + Keycloak DB)
- Keycloak (OIDC provider)
- agentd-broker (HTTPS + mTLS for agent connectors; OIDC/JWT for users)
- agentd (daemon backend)
- agentd-connector (outbound mTLS websocket to broker; proxies to agentd)
- WebUI (static site served via nginx)

Use `tools/verify_compose_stack.sh` to bring everything up and run basic smoke checks.

To add the autonomous services (orchestrator + spawn adapter) to the stack:

```bash
docker compose -f docker-compose.yml -f docker/compose.autonomous.yml up -d
```

For the full "automouse" preset (autonomous services + WebUI automation defaults):

```bash
docker compose -f docker-compose.yml -f docker/compose.autonomous.yml -f docker/compose.automouse.yml up -d
```

Or run the wrapper (includes preflight + smoke checks):

```bash
tools/automouse_pack.sh
```

Notes:
- The autonomous compose overlay enables broker client auth fallback and uses a dev token
  (`AUTOMATION_CLIENT_TOKEN`, default `dev-orchestrator-token`) for the services.
- Override `AUTOMATION_CLIENT_TOKEN` and update `docker/broker/client_auth.autonomous.json`
  for real deployments.
- The overlay also starts an `oidc-refresh` sidecar that writes a broker token file
  (`/run/agentd/broker_oidc_token.txt`). Orchestrator/spawn adapter will prefer
  `BROKER_OIDC_TOKEN_FILE` when present.

If Docker build fails on macOS with `unpigz`/`runc` resource errors, restart Docker Desktop or increase
CPU/RAM. You can also control the build behavior via:
- `COMPOSE_BUILD_SERIAL=1` (default) to reduce concurrency
- `COMPOSE_BUILD_RETRIES=3` (default) to raise retry attempts
- `COMPOSE_BUILD=0` to skip image rebuilds when images are already built (requires images present; otherwise it skips)
