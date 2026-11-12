# Arch-hosted Agent-C

The current deployment target is **https://agent.hubstack.cn**. Execution, UI serving and PostgreSQL run on the owner's Arch host. The VPS supplies the existing Traefik HTTPS ingress and the existing shared Hubstack identity service. It runs no Agent-C application container, daemon or database. Changes and removal are scoped to Agent-C; Panda, payments, Codexw, Local Work Bridge and other deployments remain separate.

This rolling project has no backward compatibility requirement. The gateway bootstraps one canonical schema and checks its fingerprint on restart. A changed schema requires a fresh deployment/database; there is no automatic migration or compatibility branch. Deployment never silently erases an existing database. Keep any required artifacts/backups before intentionally replacing state.

## Owner stop (2026-11-12)

The owner stopped Agent-C project development and requested shutdown of related
services to prevent cloud-model API fees. Do not resume implementation, deployment
or service operation without a new owner instruction. The last integrated source
is `ab69b1788c644d993578c828d0f0581f332a6ee0`; this shutdown record does not deploy it.
The prior installed runtime, source history, local configuration, artifacts and
persistent databases remain preserved. The Panda publication hold remains in force.

On Arch, shutdown used `systemctl --user disable --now agent-c-gateway.service
agent-c-daemon.service`, then `docker update --restart=no agent-c-postgres` and
`docker stop --timeout 20 agent-c-postgres`. Disabling the user units and container
restart policy prevents their ordinary automatic startup. The shared VPS ingress,
identity, Docker daemon and unrelated services were not stopped. The Agent-C site
is intentionally unavailable while its dedicated gateway is stopped.

Verify the hold without starting workloads:

```sh
systemctl --user show agent-c-daemon.service agent-c-gateway.service \
  -p ActiveState -p MainPID -p UnitFileState
docker inspect agent-c-postgres \
  --format '{{.State.Status}} restart={{.HostConfig.RestartPolicy.Name}}'
ss -ltnp | grep -E ':(18123|18784|15439)\b'
```

Expected results are `inactive`, `MainPID=0`, `disabled`, container `exited`
with restart `no`, and no listeners on the three project ports. This establishes
local shutdown, not retrospective provider billing or revocation of shared keys.
Do not run the deployment or live-model probes below while this owner hold applies.

## Owner-directed local 4B CLI use (2026-11-12)

The local model task explicitly uses the existing Agent C core and CLI with
the 4B Qwen3.5 fine-tune on Arch. This narrow integration adds a bounded native
`web_search` tool and explicit response-token limits; it uses the authenticated
loopback model at `127.0.0.1:18088`. Its launcher and measured 126K profile are
maintained in `/home/bruce/work/ai_model`:

```sh
python3 /home/bruce/work/ai_model/scripts/deepseek_qwen35.py agent chat
```

This is local, on-demand CLI use. The production service stop above and the
Panda publication hold remain effective; general Agent C development and
cloud-model service operation are not resumed. The dedicated daemon/gateway
remain inactive and disabled, and the database container remains exited with
restart disabled. Native `agent` and `agentd` were built for verification;
only the CLI and local llama.cpp model were executed for this integration.
See [CLI](CLI.md#local-models-and-response-budgets) and
[Tools](TOOLS.md#native-public-web-search) for the native contracts and search
coverage limits.

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

Artifacts install under `~/.local/share/agent-c/releases/<source>-<artifact-hash>/`, with a manifest of the source revision and every binary/UI SHA-256. `current` switches atomically. UI-only updates restart the gateway and leave an
unchanged daemon running; the installer compares its binary, environment and unit
before requesting a daemon restart. Persistent daemon state and its working directory are separate under `~/.local/share/agent-c/{state,workspace}`. Credentials remain under mode-0700 `~/.config/agent-c`, with mode-0600 files. The existing owner `~/.env` supplies provider keys through the daemon's normal server-side discovery. The selected DeepSeek model is `deepseek-v4-pro`; the authenticated provider model catalog returned this ID on 2026-11-10.

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

On 2026-11-10, the owner's real Apple callback completed through the shared service
and the authenticated browser reached the connected execution workspace. The
initial signed-in access page was caused by copying Panda's product-specific
account ID; immutable central provider proofs now establish the owner's Agent-C
administration independently of product IDs. This observed browser acceptance is
separate from `tools/deploy/verify_arch.py`, which checks anonymous rejection,
provider redirects, source revision and an authenticated SOP. The script's
`--live-model` option also makes a bounded provider call; it never claims to have
performed an interactive owner callback.

An opt-in live UI probe uses the private operator credential in an isolated
Chromium context. It checks the connected workspace, workflow sections and
settings at desktop/320/390/768px widths, refuses execution submissions and always
disables header/cookie traces. Screenshots remain in ignored local test output.
It qualifies the deployed UI separately from the mocked snapshot tests:

```sh
cd ui
AGENTC_LIVE_CONFIG="$HOME/.config/agent-c/gateway.json" \
AGENT_E2E_UI_BASE_URL=https://agent.hubstack.cn \
AGENT_E2E_RECORD=0 npx playwright test e2e/live_deployment.spec.ts --workers=1
```

VPS removal must identify Agent-C ownership from unit/container configuration or source paths before stopping/removing anything. Never use a global prune or remove generic `agent`, `broker`, `postgres` or `webui` workloads by name. The inspected VPS inventory contained no proven Agent-C runtime, container, unit or source deployment; no unrelated workload was removed.
