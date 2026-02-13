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

If Docker build fails on macOS with `unpigz`/`runc` resource errors, restart Docker Desktop or increase
CPU/RAM. You can also control the build behavior via:
- `COMPOSE_BUILD_SERIAL=1` (default) to reduce concurrency
- `COMPOSE_BUILD_RETRIES=3` (default) to raise retry attempts
- `COMPOSE_BUILD=0` to skip image rebuilds when images are already built (requires images present)
