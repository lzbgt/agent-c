# Arch-hosted Agent-C

The current deployment target is **https://agent.hubstack.cn**. Execution, UI serving and PostgreSQL run on the owner's Arch host. The VPS supplies the existing Traefik HTTPS ingress and the existing shared Hubstack identity service. It runs no Agent-C application container, daemon or database. Changes and removal are scoped to Agent-C; Panda, payments, Codexw, Local Work Bridge and other deployments remain separate.

This rolling project has no backward compatibility requirement. The gateway bootstraps one canonical schema and checks its fingerprint on restart. A changed schema requires a fresh deployment/database; there is no automatic migration or compatibility branch. Deployment never silently erases an existing database. Keep any required artifacts/backups before intentionally replacing state.

## Runtime and authority

| Component | Location | Boundary |
| --- | --- | --- |
| `agent-c-daemon.service` | Arch user service, `127.0.0.1:18123` | Native C/C++ executor, SQLite state and host tools; private service token |
| `agent-c-gateway.service` | Arch user service, `10.147.16.2:18784` | Built WebUI, shared OAuth, server-side sessions and authenticated execution proxy |
| `agent-c-postgres` | Dedicated Arch Docker container, `127.0.0.1:15439` | Gateway login/session metadata; project-owned persistent volume |
| `/etc/traefik/agent-c.yml` | Existing VPS ingress | Exact `agent.hubstack.cn` route over the existing encrypted ZeroTier network to Arch |
| Shared identity application `agent-c` | Existing `payments.hubstack.cn` | Separate application credential and exact return URIs; existing Google/GitHub/Apple registrations |

The system administrator is the verified owner of `lzbgt@icloud.com`. Deployment pins immutable provider/namespace/subject proofs obtained from the existing Panda administrator's central account bindings. The shared service intentionally assigns different product account IDs in Panda and Agent-C; a Panda product ID must never be copied into Agent-C's administrator allowlist. Optional account-ID pins refer only to verified **Agent-C** memberships. The gateway stores the authenticated provider proof with each session and compares the configured pins on every request, allowing the verified owner to enter immediately on first sign-in. An email string or a successful unrelated sign-in never grants administration. Other signed-in accounts can inspect their access state and sign out, but cannot execute this administrator workspace. Per-member isolated execution is not implemented by this gateway.

The browser holds only an opaque Secure, HttpOnly, host-only session cookie. Runtime tokens and the application credential stay in private server files. The gateway checks the central application's account/instance, active status and revision on authenticated requests; central outage or revocation fails closed. An independent private operator bearer credential supports owner recovery and service qualification. It is never written into UI configuration or browser storage.

OAuth state and PKCE verifier are browser/provider bound, bounded and consumed once. The shared exchange grant is released after creating the local session. Unsafe browser requests require the exact product Origin. Proxying strips browser cookies and caller-controlled backend authentication, inserts the private upstream token and strips backend Set-Cookie/CORS headers. SSE responses stream without proxy buffering. Authentication does not add permission prompts to authorized agent actions.

Shared provider return URIs:

```text
https://agent.hubstack.cn/auth/google/callback
https://agent.hubstack.cn/auth/github/callback
https://agent.hubstack.cn/auth/apple/callback
```

## Reproduce and deploy

Build and verify the current committed source:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAGENT_BUILD_HOST=ON -DAGENTD_ENABLE_HTTP=ON
cmake --build build --target agentd -j 4
(cd broker && go test -race ./internal/gateway ./cmd/agentd-gateway)
(cd broker && go build -trimpath -ldflags='-s -w' -o ../build/agentd-gateway ./cmd/agentd-gateway)
npm ci --prefix ui
npm run build --prefix ui
```

The shared SDK is pinned in `broker/third_party/hubstack-identity/PROVENANCE.json` and has no third-party dependencies. On the identity host, `tools/deploy/export_hubstack_admin.py` exports private immutable proofs from the owner's independently verified Panda administrator membership. It reads central bindings without creating or merging accounts. Pass that private file to `tools/deploy/provision_hubstack_identity.py --admin-proof-file ...`; application registration uses the existing platform administrator API with its private service config. Its credential/proof output transfers privately to `~/.config/agent-c/identity.json`. No central provider credentials are copied to Agent-C.

With a registered private identity file and a clean committed source tree:

```sh
python3 tools/deploy/install_arch.py \
  --identity-file ~/.config/agent-c/identity.json --publish-ingress
```

The installer creates/reuses private random PostgreSQL, daemon and operator credentials; starts only the project-owned PostgreSQL Compose service; installs the two project user services; and publishes only Agent-C's Traefik file. It requires the parent infrastructure (Docker, systemd user session/lingering, the existing ZeroTier route, DNS and Traefik certificate resolver). It does not change another project's limits, routes or services.

Artifacts install under `~/.local/share/agent-c/releases/<source>-<artifact-hash>/`, with a manifest of the source revision and every binary/UI SHA-256. `current` switches atomically. Persistent daemon state and its working directory are separate under `~/.local/share/agent-c/{state,workspace}`. Credentials remain under mode-0700 `~/.config/agent-c`, with mode-0600 files. The existing owner `~/.env` supplies provider keys through the daemon's normal server-side discovery. The selected DeepSeek model is `deepseek-v4-pro`; the authenticated provider model catalog returned this ID on 2026-11-10.

The gateway schema is fingerprinted separately from daemon SQLite. The older general Docker/Keycloak development stack is not the production deployment for this host. Broker integration is an optional gateway upstream; the initial administrator workspace uses the direct local daemon.

## Operations and evidence

```sh
systemctl --user status agent-c-daemon.service agent-c-gateway.service
AGENTC_PRIVATE_DIR="$HOME/.config/agent-c" docker compose -p agent-c -f deploy/arch/compose.yml ps
curl -fsS https://agent.hubstack.cn/healthz
curl -fsS https://agent.hubstack.cn/auth/providers
```

`/healthz` reports the deployed source revision and verifies gateway storage availability. It does not establish provider/task success. Anonymous execution API calls must return 401. `/auth/session` reports only the authenticated account/access state. Browser sign-in redirects use the shared service; provider availability/redirect qualification is distinct from a completed real-user callback.

`broker/internal/gateway` tests cover browser binding, one-use callback exchange, grant cleanup, central revision/instance mismatch, outage refusal, local logout, non-admin denial, CSRF, proxy credential isolation and administrator entry when product-specific account IDs differ. Set a **disposable** `AGENTC_TEST_DATABASE_URL` to exercise actual PostgreSQL persistence/restart, persisted provider proofs and schema mismatch refusal; the test requires a database name ending in `_test` and drops only its dedicated `agentc_gateway` schema. Browser fixtures cover unauthenticated/member/admin surfaces and mobile layout. Live qualification also executed the four-action inspection SOP and a bounded `deepseek-v4-pro` request through public authenticated ingress.

VPS removal must identify Agent-C ownership from unit/container configuration or source paths before stopping/removing anything. Never use a global prune or remove generic `agent`, `broker`, `postgres` or `webui` workloads by name. The inspected VPS inventory contained no proven Agent-C runtime, container, unit or source deployment; no unrelated workload was removed.
