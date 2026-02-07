# Docker Compose stack (prod-like local verification)

This folder contains a Docker Compose setup that spins up a **prod-like** stack for local verification.
For real production deployment guidance, see `docs/DEPLOYMENT.md`.

macOS: Docker Desktop or Colima is required. Prefer running `agentd` locally and broker/webui via compose.

- Postgres (broker DB + Keycloak DB)
- Keycloak (OIDC provider)
- agentd-broker (HTTPS + mTLS for agent connectors; OIDC/JWT for users)
- agentd (daemon backend)
- agentd-connector (outbound mTLS websocket to broker; proxies to agentd)
- WebUI (static site served via nginx)

Use `tools/verify_compose_stack.sh` to bring everything up and run basic smoke checks.
