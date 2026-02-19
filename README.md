# agent (prototype)

This repo is an early scaffold for a **portable agent core** (env-free, persistence-agnostic) plus a **desktop CLI host adapter** (env/config + persistence + HTTP).

Docs quickstart:
- Architecture + roadmap: `DESIGN.md`
- Durable workflows: `docs/WORKFLOWS.md`
- Broker design: `docs/BROKER.md`
- WebUI protocol: `docs/PROTOCOL.md`

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For a single command that runs configure/build/tests (and writes timestamped logs under `build/`), use:

```bash
tools/verify.sh
```

To run the same verification pass but first source `${HOME}/.env` (so provider keys like `DEEPSEEK_API_KEY` are available
to smoke tests), use:

```bash
tools/verify_prod.sh
```

Host builds (`agent` / `agentd`) require `libcurl` and `jsoncpp` (via `pkg-config`).

**macOS (Homebrew)**:

```bash
brew install cmake pkg-config curl jsoncpp sqlite
```

macOS production packaging (signed/notarized `.pkg`) is documented in `docs/MACOS_PACKAGING.md`.

**Windows (native build)**: install dependencies via vcpkg, then configure with its toolchain file:

```powershell
vcpkg install curl jsoncpp sqlite3
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build -j
```

Or run the helper:

```powershell
tools\verify_windows_build.ps1 -Config Release
```

To auto-install vcpkg + deps:

```powershell
tools\verify_windows_build.ps1 -InstallDeps -VcpkgRoot C:\vcpkg -Config Release
```

CI helpers (optional):
- `tools/trigger_ci_windows_build.sh [ref]` (dispatches Windows CI; requires token or `gh auth login`)
- `tools/check_ci_windows_build.sh` (prints latest Windows CI status; requires token for private repos)

Platform-specific notes (Windows limitations, plugin support, AVM endpoints) live in `docs/PLATFORM_SUPPORT.md`.

Smoke tests:
- `ctest` includes many bash-based `agentd_*_smoke.sh` tests that start/stop the daemon; shared helpers live in `tests/lib/agentd_smoke_lib.sh`.

### Core-only build (portable; no CURL required)

If you only want the portable core library + core unit tests (e.g. embedded/toolchain bring-up), disable host builds:

```bash
cmake -S . -B build-core -DAGENT_BUILD_HOST=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

Or:

```bash
tools/verify.sh --core-only
```

This builds `agent_core` and `agent_core_tests`, but skips `agent_host`, `agent`, `agentd`, and host/network smokes.

### Persistence port (hosts/embedded)

Core defines an optional persistence interface (`core/include/agent/persist.h`) so hosts can swap persistence implementations
without changing the core call sites (filesystem `.sess`, SQLite, NVS/flash, etc.).

### Durable memory (daemon state)

`agentd` maintains a durable memory directory under `state_dir/memory/` and exposes host tools (`memory_write/get/search/put`)
to let models persist and retrieve long-lived facts/preferences/tasks. See `docs/MEMORY.md`.

### Daemon longevity (job GC)

`agentd` keeps async job state in memory for UI progress streaming. Finished jobs are garbage-collected:
- `--job-ttl-ms <n>`: remove done/error jobs older than `n` ms (default: 1800000)
- `--max-jobs <n>`: keep at most `n` jobs in memory (default: 256)

### Daemon auth (optional)

If you bind `agentd` to non-loopback (e.g. `--host 0.0.0.0`), you must set an auth token (agentd refuses to start otherwise):

```bash
./build/agentd --auth-token "your_token"
```

Clients must send `Authorization: Bearer your_token` to all endpoints (except `/api/v1/health`, `/api/v1/ready`, and `/metrics`).
You can also accept the token via a cookie name (useful for browser clients):

```bash
./build/agentd --auth-token "your_token" --auth-cookie "agentd_auth"
```

### Daemon CORS (browser clients)

The Web UI typically runs on a different origin (e.g. `http://localhost:5173`) than the daemon (`http://127.0.0.1:8123`),
so `agentd` emits CORS headers for browser fetches:

- When binding to loopback (`127.0.0.1` / `localhost`): CORS defaults to `Access-Control-Allow-Origin: *` for local dev ergonomics.
- When binding to a non-loopback host: CORS is **disabled by default**. Enable it explicitly with one or more `--cors-origin` values.

Example (remote UI origin allowlist):

```bash
./build/agentd --host 0.0.0.0 --auth-token "your_token" --cors-origin "https://your-ui.example"
```

If you use cookie auth, allow credentials so the browser can send cookies:

```bash
./build/agentd --host 0.0.0.0 --auth-token "your_token" --auth-cookie "agentd_auth" \
  --cors-origin "https://your-ui.example" --cors-allow-credentials
```

Per-route origin policies (longest path prefix wins) are supported:

```bash
./build/agentd --cors-route '{"path_prefix":"/api/v1/openrouter","origins":["https://ui.example"]}'
```

By default, `agentd` allows common headers needed by the UI, including `Authorization` (daemon auth), `X-OpenRouter-Key`,
and request correlation headers `X-Request-Id` / `X-Trace-Id` (also exposed for browser access).
(provider key for the OpenRouter model catalog endpoint).

`ctest` includes two network smoke tests (OpenRouter + DeepSeek). They will run if keys are present
either via environment variables or `project.local.md` (gitignored). Disable them with:

```bash
export AGENT_DISABLE_NETWORK_TESTS=1
```

If you want to keep DeepSeek/Moonshot network tests but skip OpenRouter-only tests:

```bash
export AGENT_TEST_SKIP_OPENROUTER=1
```

To set keys via a local file, copy `project.local.md.example` to `project.local.md` and fill in real values.

Network tests also assume an HTTP proxy may be required; the scripts default to `http://localhost:8120`
via `https_proxy` / `http_proxy`.

### Local secrets file: `.not_in_repo` (preferred)

For local development, you can store provider keys in a gitignored file named `.not_in_repo` at the repo root.
This keeps keys out of the UI/browser, and lets `agentd` load them automatically.

Accepted formats (either style; one per line):

```text
DEEPSEEK_API_KEY=sk-...
OPENROUTER_API_KEY=sk-...
KIMI_API_KEY_CN=sk-...
MOONSHOT_API_KEY=sk-...
```

or:

```text
- deepseek: sk-...
- openrouter: sk-...
```

Notes:
- `.not_in_repo` is gitignored; do not commit keys.
- `agentd` prefers `.not_in_repo` over `project.local.md` when auto-loading provider keys.
- You can point `agentd` at a specific dotenv file with `AGENTD_DOTENV_PATH=/path/to/.env` (useful for services
  running under a different user).
- `~/.env` is a last-resort key source only; base URLs still come from flags/env/config. For Moonshot, set
  `MOONSHOT_API_BASE` or pass `--base-url https://api.moonshot.cn/v1`.

### Tool extensions: plugins + tool servers (out-of-process)

`agentd` supports adding tools without rebuilding:
- **Tool plugins** (in-process, dlopen): `--tool-plugin /path/to/plugin.so`
- **Tool servers** (out-of-process, stdio JSON-lines): `--tool-server-cmd "<cmd>"`

Tool servers are the preferred “fast bring-up” path for big integrations (Playwright, device bridges, AVM policy runners)
because failures are isolated across a process boundary. See `docs/TOOLS.md`.

## Real end-to-end (agentd + browser) test

This repo includes a “real” E2E harness that drives the Web UI in a real browser (headless) using Playwright and makes real
provider calls via `agentd`.

Prereqs:
- `.not_in_repo` populated with your provider key(s) (or env vars set)
- `./build/agentd` built (`tools/verify.sh`)
- UI deps installed (`cd ui && npm install`)

Run:

```bash
tools/e2e_real.sh
```

Logs are written under `build/e2e/`.

Notes:
- The harness expects provider keys to be available via env or `.not_in_repo` (preferred).
- `agentd` now also supports **daemon-side** key loading per-run based on the request `base_url`, so the Web UI can omit `api_key`.
- The Web UI sends a `client` identity with each run (e.g. `client.kind="webui"`). `agentd` can use this to inject a
  client-specific system prompt “profile” for default presentation/DoD semantics. See `docs/CLIENT.md`.

### Daemon config snapshot (debug)

For debugging client/daemon mismatches (CORS, sandbox defaults, job GC), `agentd` exposes:
- `GET /api/v1/config`

This endpoint requires auth when `--auth-token` is set. It intentionally does not include secrets (auth token, provider API keys).
The Web UI surfaces this snapshot in the Settings panel as “Daemon config”, including the effective `state_dir`, `sessions_root_dir`,
and `db_path` (SQLite; canonical daemon state store).

### Update daemon defaults at runtime (model/base_url + server-side keys)

`agentd` supports updating its defaults **at runtime** (persisted in `agentd`’s SQLite DB at `db_path`) so the browser UI does not need
to store provider keys.

Endpoint:
- `POST /api/v1/config/update`

Example (set default provider + model):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"base_url":"https://api.deepseek.com","model":"deepseek-chat"}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Example (store a provider key server-side; do **not** do this over an unauthenticated network):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"provider":"deepseek","api_key":"sk-REDACTED"}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Notes:
- The response intentionally never includes secrets. Use `GET /api/v1/config` to see booleans like `provider_keys_set`.
- The Web UI also exposes buttons in Settings to “Save defaults to daemon” and “Save API key to daemon”.

### Daemon state dir / multi-agent safety

`agentd` stores its canonical state in SQLite (`--db-path`, default: `./agentd.db` relative to the daemon working directory).
If you run multiple `agentd` instances and want to avoid accidental session collisions, use a distinct DB path per daemon:

```bash
./build/agentd --db-path /tmp/agentd_state_1.db
./build/agentd --db-path /tmp/agentd_state_2.db
```

Legacy (non-canonical) session-store directories are still configurable for backwards compatibility and best-effort cleanup:

```bash
./build/agentd --sessions-root /tmp/agentd_sessions
```

Environment variables:
- `AGENTD_STATE_DIR`
- `AGENTD_SESSIONS_ROOT`
- `AGENTD_DB_PATH`
- `AGENTD_ACCESS_LOG` (`1` for text logs, `json` for JSON logs)
- `AGENTD_HTTP_MAX_BODY_BYTES` (default `67108864`, `0` to disable limit)
- `AGENTD_HTTP_MAX_HEADER_BYTES` (default `1048576`, `0` to disable limit)
- `AGENTD_HTTP_READ_TIMEOUT_MS` (default `15000`, `0` to disable read timeouts)
- `AGENTD_UPLOAD_MAX_BYTES` (per-file upload cap, default `33554432`, `0` to disable cap)
- `AGENTD_MAX_TOOL_CALL_ARGS_CHARS_DEFAULT` (default `0`, disables tool-arg length guard)

## Run in production (agentd + WebUI)

This repo’s “production” shape is:
- `agentd` runs as the backend daemon (tools + persistence + HTTP API)
- the Web UI runs as a separate static site (Vite build) that talks to `agentd` via HTTP
  - it can also run in **broker mode** (OIDC) and exposes a broker console for agent selection + membership management

For a full production checklist (TLS, broker/connector, auth hardening, backups), see `docs/DEPLOYMENT.md`.

### 1) Start agentd (prod defaults: YOLO + host tools)

Recommended: pick a stable working directory (so relative artifact paths like `out/foo.wav` are always resolvable) and keep
all generated files under it.

Example (local machine, loopback only; no auth required):

```bash
cd /path/to/agent
./build/agentd \
  --host 127.0.0.1 \
  --port 8123 \
  --tools host \
  --yolo \
  --host-scope "$(pwd)" \
  --tools-root "@host" \
  --db-path "$(pwd)/agentd.db"
```

Example (LAN / production; requires auth + explicit CORS allowlist):

```bash
cd /path/to/agent

# Optional: load real provider keys for the daemon process (do NOT commit secrets)
set -a
source "${HOME}/.env"
set +a

./build/agentd \
  --host 0.0.0.0 \
  --port 8123 \
  --auth-token "REPLACE_WITH_RANDOM_TOKEN" \
  --cors-origin "http://127.0.0.1:5173" \
  --cors-origin "http://127.0.0.1:8100" \
  --tools host \
  --yolo \
  --host-scope "$(pwd)" \
  --tools-root "@host" \
  --db-path "$(pwd)/agentd.db"
```

macOS convenience (launchd install/uninstall):

```bash
AGENTD_AUTH_TOKEN="REPLACE_WITH_RANDOM_TOKEN" tools/install_agentd_launchd.sh
tools/uninstall_agentd_launchd.sh
```

Notes:
- `--yolo` enables unrestricted host tools (fully autonomous, side effects allowed). The Web UI defaults to requesting this.
- `--tools` acts as a **maximum** toolset; run requests and tool registry queries cannot exceed it.
- `--tools-root "@host"` makes relative paths stable (anchored to `--host-scope`). This avoids brittle “depends on process CWD”
  behavior for artifact fetches like `GET /api/v1/file?path=out/foo.wav&yolo=1`.
- If you omit `--db-path`, `agentd` defaults to creating/using `./agentd.db` in its working directory.
- If you bind to non-loopback (`--host 0.0.0.0`), `agentd` refuses to start without `--auth-token` unless you pass `--allow-unauth`.

### 2) Start the Web UI

Dev (hot reload):

```bash
cd ui
npm install
npm run dev -- --port 5173
```

If you prefer port `8100`:

```bash
cd ui
npm run dev -- --port 8100
```

Production build + local preview:

```bash
cd ui
npm ci
npm run build
npm run preview -- --host 127.0.0.1 --port 8100
```

Optional defaults (no rebuild): edit `ui/public/agentui-config.js` (copied to `ui/dist/`) to prefill:
- `connectionMode` (`direct` or `broker`)
- `daemonBaseUrl`, `brokerBaseUrl`, `brokerAgentId`, `brokerDeploymentId`
- `daemonAuthToken`, `brokerAuthToken` (if you accept storing tokens in a static file)
- `model`, `baseUrl`, `proxyUrl`, `timeoutMs`

Then open the UI and set:
- Daemon base URL: `http://127.0.0.1:8123`
- Daemon auth token (if enabled): the same token passed to `agentd --auth-token`
- Model/provider defaults (Settings → Model / Provider), or use the OpenRouter model picker if needed.
- Run limits (Settings → Run limits) to cap `max_steps` / tool-call budgets.
- Diagnostics (Settings → Diagnostics) to confirm provider keys and run `provider_test`.

### Manual verification (macOS)
1) Broker health (if using broker/connector):
```bash
curl -k https://127.0.0.1:8443/healthz
```
2) Agentd health:
```bash
curl http://127.0.0.1:8123/api/v1/health
curl http://127.0.0.1:8123/api/v1/ready
curl http://127.0.0.1:8123/metrics
```
Optional diagnostics (requires auth if enabled):
```bash
curl http://127.0.0.1:8123/api/v1/diagnostics
curl http://127.0.0.1:8123/api/v1/diagnostics/providers
```
See `docs/DIAGNOSTICS.md` for provider_test usage.
`/api/v1/diagnostics/providers` includes `base_url_source` (`config`/`env`/`default`) to explain how a provider base URL was chosen.
3) UI functional check:
- Run a short prompt that emits audio (artifact or scene).
- If autoplay is blocked, click once in the UI to unlock media playback.

If you want a one-command local check (agentd + WebUI only, no Docker/broker):
```bash
tools/verify_mac_local_stack.sh
```
Optional env:
- `MAC_LOCAL_SKIP_UI=1` (skip WebUI build/serve)
- `MAC_LOCAL_UI_INSTALL=0` (skip `npm ci` if deps already exist)
- `MAC_LOCAL_PROVIDER_TEST=1` (run diagnostics provider tests if keys are available)
- `MAC_LOCAL_PROVIDER_TEST_TIMEOUT_MS=30000` (provider test timeout override)

The Web UI defaults to:
- YOLO enabled
- client RPC enabled
- client RPC side effects enabled
- full tool-call/event visibility in History (so users can inspect tool arguments/results)

Reliability notes (important for production UX):
- The UI persists the active async `job_id` + SSE cursor in `localStorage`, so a browser refresh can resume a running job stream.
- The UI persists the selected `session_id` per daemon base URL in `localStorage` (helps when running multiple local daemons).
- The UI persists the Scene (client-side entities) per `session_id` in `localStorage`.
- The UI posts client acknowledgement events (`ui_action_shown`, `client_rpc_result`, `artifact_rendered`/`artifact_render_failed`)
  so agents can implement deterministic “Definition of Done” handshakes (see `docs/DOD_ACK.md`).

## Cleanup (disk usage)

Build artifacts and run logs can grow quickly (especially `build/`, `build-core*/`, and `out/`). To keep the repo lean:

```bash
tools/clean.sh
```

Options:
- `--aggressive`: drop build*/out regardless of size + UI build caches.
- `--purge-deps`: remove dependency caches (`ui/node_modules`, `.agent_deps`, `ref/*/venv`).
- `--purge-state`: remove stateful data (`state/`, `db/`, `memory/`, `session_*`) — **data loss**.
- `--purge-ref-git`: remove nested `.git` dirs under `ref/` for vendored repo bloat.
- `--report`: print a repo size report after cleanup.
- `--dry-run`: preview what would be deleted.
- `--out-max-days N`: prune log files older than N days (0 = delete all).
- `--threshold-gb N`: size threshold (GiB) for build/out removal.
- `--max-repo-gb N`: fail if total repo size exceeds N GiB after cleanup.

To inspect disk usage and spot bloat quickly:

```bash
tools/repo_size_report.py --depth 2 --top 20
```

Largest files (helps find sudden bloat):

```bash
tools/repo_size_report.py --largest-files 20 --largest-min-bytes 1048576
```

Exclude common bulky paths (git objects, builds, deps):

```bash
tools/repo_size_report.py --exclude-defaults --depth 2 --top 20
```

CI guard (fail if the repo exceeds a size cap):

```bash
tools/repo_size_report.py --exclude .git --max-total-gb 5 --fail-on-nested-git
```

List nested .git dirs (useful before running cleanup):

```bash
tools/repo_size_report.py --list-nested-git
```

Stub file scan (fails on empty/placeholder files):

```bash
tools/stub_file_scan.py --fail
```

Tracked file size guard (fails if any tracked file exceeds 10 MiB):

```bash
tools/tracked_file_guard.py --max-mb 10
```

Run all repo hygiene guards locally:

```bash
tools/verify_repo_guards.sh
```

## Docker Compose (prod-like local verification)

If you want a **prod-like stack** locally (Postgres + Keycloak OIDC + broker + connector + agentd + WebUI):

```bash
./tools/verify_compose_stack.sh
```

Note: the WebUI container mounts `tools/agentui-config.compose.js` to default to broker mode (edit as needed).
If you want prebuilt images instead of local builds, set:
- `BROKER_IMAGE`, `AGENTD_IMAGE`, `CONNECTOR_IMAGE`, `WEBUI_IMAGE`
- run with `COMPOSE_BUILD=0 COMPOSE_PULL=1` to pull missing images automatically.

If Docker is unavailable or resource constrained, use:
```bash
tools/verify_mac_local_stack.sh
```

If Docker builds are blocked but Docker can run containers, use the host-mode stack:
```bash
tools/verify_mac_full_stack_host.sh
```

Notes:
- The script will auto-pick free host ports for services that commonly conflict (Broker/Keycloak/Postgres) and export:
  - `BROKER_PUBLISHED_PORT` (default 8443)
  - `KEYCLOAK_PUBLISHED_PORT` (default 8081)
  - `POSTGRES_PUBLISHED_PORT` (default 5433)
- It will also auto-pick free host ports for the user-facing services:
  - `WEBUI_PUBLISHED_PORT` (default 8100)
  - `AGENTD_PUBLISHED_PORT` (default 8123)
- It sets `COMPOSE_PROJECT_NAME` automatically (defaults to `agent_${WEBUI_PUBLISHED_PORT}`) so you can run multiple stacks concurrently.
- Keycloak is intentionally accessed via `keycloak.lvh.me` (resolves to `127.0.0.1`) so the `iss` claim in minted JWTs
  matches what the broker validates. If you request tokens via `http://127.0.0.1:<port>`, you’ll get issuer-mismatch errors.
- If Docker build hits `unpigz`/`runc` resource errors, restart Docker Desktop or set `PIGZ=-p1 GZIP=-p1` to reduce
  decompression thread pressure.

This starts:
- WebUI: `http://127.0.0.1:${WEBUI_PUBLISHED_PORT}`
- agentd: `http://127.0.0.1:${AGENTD_PUBLISHED_PORT}` (auth token: `dev-agentd-token`)
- Keycloak: `http://keycloak.lvh.me:${KEYCLOAK_PUBLISHED_PORT}` (realm: `agentd`, user/pass: `test`/`test`)
- Broker: `https://127.0.0.1:${BROKER_PUBLISHED_PORT}` (self-signed CA for local dev; mTLS for connectors)

## Git remote (publishing)

This workspace may not have a git remote configured. If `git push` fails with “No configured push destination”,
configure `origin` explicitly:

```bash
git remote add origin <your_repo_url>
git push -u origin "$(git rev-parse --abbrev-ref HEAD)"
```

Or use the helper script (does not guess a URL):

```bash
AGENT_GIT_REMOTE_URL="<your_repo_url>" tools/setup_git_remote.sh --push
```

To run a full local verify + push in one command:

```bash
tools/publish.sh --skip-ui
```

The helper can also read `git_remote` from your gitignored `project.local.md`:

```bash
cp project.local.md.example project.local.md
# edit project.local.md and set:
# - git_remote: <your_repo_url>
tools/setup_git_remote.sh --push
```

If `origin` exists but points to the wrong place, pass `--force` to update it:

```bash
tools/setup_git_remote.sh --url "<your_repo_url>" --force --push
```

Note: there are additional host-tool network smokes (DeepSeek/OpenRouter) that validate bounded tools like
`fs_read` and `fs_find` end-to-end (model → tool call → tool output).

## CLI usage

### One prompt (persisted session by default)

```bash
export OPENAI_API_KEY=...
./build/agent run "hello"
```

Notes:
- The CLI prints the assistant text to **stdout**.
- When `--trace` is enabled (default), the CLI prints a compact transcript (tool calls + results + errors) to **stderr** so piping stdout remains clean.
- Use `--transcript-jsonl <path>` to additionally append raw tool-loop events as JSONL (useful for UI replay/debugging).

This uses session id `default` and stores it at `~/.agent/sessions/default.sess` (portable, JSON-free). If built with JSONCPP,
it also writes `~/.agent/sessions/default.json` for debugging/interoperability.
If this project is built without JSONCPP, `.sess` is the only persisted session format.

### Proxy (when outbound networking requires it)

If your environment requires an HTTP proxy for outbound HTTPS, pass an explicit proxy URL (or set env `HTTPS_PROXY` / `http_proxy`):

```bash
./build/agent run "hello" --proxy http://localhost:8120
```

### Explicit session id

```bash
./build/agent run "continue" --session myproj
```

### Disable persistence (ephemeral run)

```bash
./build/agent run "one shot" --no-session
```

### OpenRouter (optional headers)

```bash
export OPENROUTER_API_KEY=...
export OPENROUTER_API_BASE=https://openrouter.ai/api/v1
export OPENROUTER_HTTP_REFERER=https://example.com
export OPENROUTER_X_TITLE="agent"
./build/agent run "hi" --base-url "$OPENROUTER_API_BASE" --model "google/gemini-2.0-flash-001"
```

### Pick an OpenRouter verification model (cheap + tools + multimodal)

This repo includes a small host/dev utility that fetches OpenRouter's model catalog, filters to:
- multimodal-capable inputs (image/audio/video)
- OpenAI tools/tool_choice support
- total pricing within a range (default: $0.01–$0.50 per 1M tokens, prompt+completion)

It writes a report to `ref/openrouter/multimodal_latest.md` and prints a recommended model id:

```bash
python3 tools/openrouter_models.py --write
```

### DeepSeek tool use (calculator)

`deepseek-reasoner` can do tool calls but may reject forcing `tool_choice`, so for deterministic tool verification use `deepseek-chat`:

```bash
./build/agent run "Use calculator to compute (2+2)*10. Return exactly: 40" \
  --no-session \
  --base-url "https://api.deepseek.com" \
  --api-key "$DEEPSEEK_API_KEY" \
  --model "deepseek-chat" \
  --tools basic \
  --force-tool calculator \
  --require-tool-call
```

### Multimodal attachments (images + files)

The CLI supports attaching local files via `--attach <path>` (repeatable).

Behavior:
- Images (`.png/.jpg/.webp/...`) are sent as base64 `data:` URLs (model must support image inputs).
- Non-image files are included as **best-effort text excerpts** (binary files get a short base64 preview).
- Not every provider/model supports multimodal inputs. Even within the same provider, different models may accept/reject images.
  - If the model rejects the request, you'll typically see an HTTP `400` with a message like “image input not supported”.

Example (OpenAI-compatible vision-capable model):

```bash
./build/agent run "Describe this image." \
  --no-session \
  --tools none \
  --base-url "https://api.openai.com/v1" \
  --model "<vision-capable-model-id>" \
  --attach ./image.png
```

Example (Moonshot / Kimi K2.5, OpenAI-compatible API; base64 images only):

```bash
./build/agent run "Describe this image." \
  --no-session \
  --tools none \
  --base-url "https://api.moonshot.cn/v1" \
  --model "kimi-k2.5" \
  --api-key "$KIMI_API_KEY_CN" \
  --attach ./image.png
```

### Extensible tools (embedded-friendly)

Tool definitions and execution are host-pluggable:
- Core stores tool schemas in a portable registry: `core/include/agent/tools.h`
- Hosts implement the executor callback (GPIO, sensors, local RPC, etc.)
- CLI wires:
  - `basic` toolset (calculator) via `cli/src/toolset_basic.cpp`
  - `host` toolset (subprocess exec + diff-based file edits) via `cli/src/toolset_host.cpp`

Host toolset usage:

```bash
./build/agent run "List files, then show README.md" --tools host --tools-root .
```

### Runaway tool-call guard (repeat limit)

Some failure modes look like “capture camera → register artifact → repeat forever”. To keep runs bounded, the tool loop has a
guard that stops if the model repeats the **exact same tool call** too many times:

- CLI: `--max-repeated-tool-calls <n>` (default: `12`; set `0` to disable)
- Daemon/UI: request field `max_repeated_tool_calls` (see `docs/PROTOCOL.md`)
- For daemon runs, `max_steps` can be left blank in the UI to use the daemon’s default step cap (`/api/v1/config: daemon.max_steps_default`).
- For worst-case safety, daemon also applies a default cap on total tool calls (`/api/v1/config: daemon.max_tool_calls_total_default`).
- To guard against oversized tool call arguments, daemon can apply a default cap on tool call JSON length
  (`/api/v1/config: daemon.max_tool_call_args_chars_default`, `0` disables).
- For targeted safety (without breaking benign high-frequency tools like `fs_read`), daemon can apply explicit per-tool limits:
  - daemon defaults are visible at `/api/v1/config: daemon.tool_call_limits_default`
  - configure via `agentd --tool-call-limit proc_exec=4 --tool-call-limit shell_exec=16 --tool-call-limit artifact_register=16 --tool-call-limit ui_action=16`
  - or via env: `AGENTD_TOOL_CALL_LIMITS_DEFAULT="proc_exec=4,shell_exec=16,artifact_register=16,ui_action=16"`
  - per-run overrides: request field `tool_call_limits` (see `docs/PROTOCOL.md`)
  - defaults can be persisted via `/api/v1/config/update`:
    `max_steps_default`, `max_tool_calls_total_default`, `max_tool_calls_per_tool_default`,
    `max_tool_call_args_chars_default`, `tool_call_limits_default`

Seamless compaction for tool loops:
- In `--tools basic` / `--tools host` mode, the tool loop applies the same char-budget policy as the core runner:
  keep pinned `system` prefix + keep last `K` messages.
- When messages are dropped, a short deterministic `system` summary is inserted into the request so the model
  has some continuity (no extra model call).
- If the provider rejects a request as too large (context length / too many tokens), the tool loop retries after
  compacting more aggressively. This is effectively “spawning a new session” for stateless backends.

Session vs audit:
- Session message history:
  - `~/.agent/sessions/<id>.sess` (portable, JSON-free; primary)
  - `~/.agent/sessions/<id>.json` (optional, written when built with JSONCPP)
  stores **user/assistant conversation** only.
- Detailed tool timelines (tool calls/results + LLM request/response events) are stored in the per-session audit log
  (`~/.agent/sessions/<id>.events.jsonl`) and surfaced via CLI `--trace` output.
- Daemon (`agentd`) state is separate: sessions + audit live in SQLite (`db_path`, default: `./agentd.db`) and are exposed via
  endpoints like `GET /api/v1/session` and `GET /api/v1/session/audit`.

Host tool names:
- In `--host-policy readonly`, the host tool registry omits `shell_exec`, `proc_exec`, and `file_apply_patch` (read-only inspection only).
- In daemon restricted mode (`yolo=false`), the host tool registry omits `shell_exec` and `proc_exec` even when `host_policy=full`.
  Filesystem tools remain available; agentd no longer enforces a `tools_root` sandbox at runtime (use container-level isolation for multi-tenant safety).
- `shell_exec` (runs `/bin/sh -lc <cmd>`, returns JSON envelope with `exit_code`, `timed_out`, `truncated`, `output`)
- `proc_exec` (runs an argv array via `posix_spawnp`, no shell; returns JSON envelope with `argv`, `exit_code`, `timed_out`, `truncated`, `output`)
- `file_apply_patch` (applies a unified diff via `git apply`; returns the patch as a diff-style audit trail)
- `fs_stat` (file/dir metadata; returns structured fields + a human-readable `output`)
- `fs_list` (bounded directory listing; returns structured `entries` + `output`)
- `fs_find` (bounded file discovery; returns structured `entries` + `output`)
- `fs_read` (bounded file read with pagination by line; returns `content`/`output` + `has_more` + `next_start_line`)
- `text_search` (token-safe substring search; returns structured `matches` + `output`)

### UI actions (bidirectional UX)

In addition to `artifact_register` (explicit media artifacts), host tools include `ui_action` so the model can request
UI-side actions in a **typed** and **allowlisted** way (e.g. notifications, audio playback UI).

See `docs/CLIENT.md`.

Notes:
- For **inspection** (read/list/stat), prefer `fs_list` / `fs_read` / `fs_stat` because they provide bounded output and pagination
  (helps prevent token/context blow-ups). Use `rg/grep` first, then `fs_read` for narrow line ranges.
- For **file discovery**, prefer `fs_find` over `find`/`tree` when you need predictable output size and default excludes.
- For **search**, prefer `text_search` over `grep -R` when you need predictable output size (bounded matches + per-file size limits).
  - Optional: use `extensions` (e.g. `[".cpp",".h"]`) to restrict scanning.
- For host-side **mutating** file operations (remove/move/rename), prefer OS-native commands via `proc_exec` / `shell_exec` (e.g. `rm`, `mv`, `git`).
  If you are running the daemon in scoped mode (`yolo=false`), exec tools are omitted; use `file_apply_patch` for edits.
- For file edits, prefer `file_apply_patch` so the tool output includes a diff-style record of the change.
- Tool outputs are capped before being inserted back into the next LLM request context, to avoid overflowing the context window.

Default host system hint (CLI/daemon):
- When using `--tools host`, the CLI/daemon injects a one-time `system` message into an empty session to encourage
  **fast incremental inspection** (use `rg/grep`, `head`, `tail`, `awk`, `sed -n`) instead of reading large files wholesale.
- CLI: disable with `--no-default-system` or override with `--system "<your prompt>"`.
- CLI: select a built-in prompt profile with `--system-profile default|jules_codex`.
- Daemon: disable with `./build/agentd --no-default-system` or per-request with `no_default_system: true`;
  override per-request with `system: "<your prompt>"`.
- Daemon: select a built-in prompt profile with `./build/agentd --system-profile default|jules_codex`, env `AGENTD_SYSTEM_PROFILE`,
  or per-request with `system_profile: "default" | "jules_codex"`.

Optional LLM summaries for compaction (`--tools none`):
- In `--tools none` mode, you can optionally provide `--summary-model <name>` to generate a short LLM summary of the
  messages that are about to be dropped during compaction.
- The host inserts the summary as a `system` message starting with `AGENT_SESSION_SUMMARY_PREFIX` so the core can treat it
  as **not pinned** (allowing it to be replaced/compacted later instead of growing the pinned prefix forever).
- This is optional because it costs an extra model call; the default compaction behavior does not require it.

### Chat REPL

```bash
./build/agent chat --session default --tools host --tools-root .
```

Commands:
- `/exit` or `/quit` ends the REPL.

## Core library

- Header: `core/include/agent/agent.h`
- Scope: session model + char-budget compaction + role helpers
- No environment variable access in the core (host-only concern).

## Daemon + Web UI (day-1)

The recommended UX direction is **daemon-first**: run `agentd` locally and use the Web UI (or CLI) as a client.

### Run the daemon

```bash
./build/agentd --host 127.0.0.1 --port 8123
```

Safety:
- `agentd` refuses to bind to non-loopback hosts unless `--auth-token` is set.
- Override (insecure): `./build/agentd --host 0.0.0.0 --port 8123 --allow-unauth`

Proxy override:

```bash
./build/agentd --host 127.0.0.1 --port 8123 --proxy http://localhost:8120
```

SQLite DB (mandatory; canonical daemon state):

```bash
./build/agentd --host 127.0.0.1 --port 8123

# or pin it explicitly (recommended for production / multi-daemon):
./build/agentd --host 127.0.0.1 --port 8123 --db-path "$HOME/.agent/agentd.db"
```

The DB stores sessions, runs, events, tool records, artifacts, UI actions, client events, and audit records for debugging/replay.
See `docs/DB.md` for query examples.

Health check:

```bash
curl http://127.0.0.1:8123/api/v1/health
curl http://127.0.0.1:8123/api/v1/ready
```

YOLO vs host-scoped tools:
- Default daemon mode is YOLO (unrestricted) to match local development needs.
- `yolo` is now a *tool exposure* knob (primarily enabling/disabling process execution tools). It does not sandbox filesystem paths.
- For safer deployments, `agentd` supports `--host-policy full|readonly`:
  - `full`: enables process exec + patch application + filesystem inspection (`shell_exec`, `proc_exec`, `file_apply_patch`, `fs_*`, `text_search`)
  - `readonly`: disables process exec and patch application (keeps only `fs_*` + `text_search`)
  - Requests can also pass `host_policy: "readonly"` to *tighten* permissions for that run (cannot expand beyond daemon default).

Verbose inspection:
- Pass `verbose: true` to `POST /api/v1/run` to return a structured `events` log suitable for UIs
  (LLM request/response, tool calls, tool results).
- Pass `trace: true` to also return a plain-text transcript (`trace_text`).
- Optional: pass `max_capture_bytes` to cap large verbose event payloads for UI stability (default: 256KB; daemon clamps for tool loops).
- `agentd` ignores `SIGPIPE` so client disconnects (UI refresh, SSE close) do not terminate the daemon.
- The daemon also serves files for previews at `GET /api/v1/file?path=<...>&session_id=<sid>` (up to 10MB).
  - If `session_id` is provided and `path` is relative, the file is resolved under `<sessions_root>/<sid>/...`.
  - If `path` is absolute, it is served directly.

Assistant streaming (provider-dependent):
- Clients can set `stream_assistant: true` to request OpenAI-compatible SSE streaming (`stream: true`).
  - `tools: "none"`: the daemon emits `assistant_delta` events while the assistant message is streaming.
  - `tools: "basic"` / `"host"`: the daemon uses streaming requests for tool-loop steps and (best-effort) reconstructs tool calls
    from streamed `delta.tool_calls` (and legacy `delta.function_call`). It also emits `assistant_delta` events during the final assistant step (provider-dependent).
    Note: some providers ignore `stream: true` and return a normal JSON completion; the daemon falls back to non-stream parsing.
  - Implementation notes: `docs/STREAMING.md`

CLI streaming (stdout):
- For `agent run` / `agent chat`, pass `--stream-assistant` to stream assistant deltas to stdout (provider-dependent).
  - `--tools none`: streams the assistant message for a single request (`stream: true`).
  - `--tools basic|host`: streams assistant deltas during tool-loop steps (best-effort; depends on provider streaming + `delta.tool_calls`).

Tool schema introspection (extensible tools):
- `GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=<id>` returns the tool registry the daemon will expose:
  `name`, `description`, `parameters_json` (OpenAI-compatible JSON Schema).
- Tool exposure is also constrained by the daemon's `--host-policy` (response includes `effective_host_policy`).
- Requests cannot exceed the daemon's `--tools` setting; exceeding it returns HTTP 400.
- The response includes `daemon_tools` and (when provided) `requested_tools` for clarity.
- When `session_id` is provided (and `tools=host`), the registry may include **session-scoped tools** such as `ui_wait_event`.
- This is intended for “day-1 rich UI” features (rendering tool info, validating tool-call args) and for future clients.

OpenRouter model discovery (for verification + multimodal/tools filtering):
- `GET /api/v1/openrouter/models?min_total=0.01&max_total=0.50&require_multimodal_input=1&require_tools=1&limit=50`
  fetches OpenRouter’s `/models` catalog (using `OPENROUTER_API_KEY` or an `Authorization: Bearer ...` header),
  filters it, sorts by total price ($/1M prompt+completion), and returns a recommended cheapest model id.

Session browsing:
- `GET /api/v1/sessions` lists known sessions.
- `GET /api/v1/session?session_id=<id>` returns the message history.
- `GET /api/v1/session/audit?session_id=<id>&include_rotated=0|1` returns recent per-run audit entries (prompt + assistant + events).

Async runs (UI-friendly):
- `POST /api/v1/run_async` starts a background run and returns `{ ok, job_id }`.
- `GET /api/v1/job?job_id=<id>` returns `{ ok, status, result? }` (result shape matches `/api/v1/run`).
  - For live UI progress without SSE/WebSocket:
    - Add `include_events=1` to include the tool/LLM `events` captured so far (best-effort).
    - Use `cursor=<n>&max_events=<m>` to tail new events incrementally while the job is running.
- `GET /api/v1/job/stream?job_id=<id>&cursor=<n>` streams job progress via SSE:
  - emits `agent_event` (same objects as the `events` array)
  - ends with `job_done` containing the final `result`
- While jobs are running, the daemon may emit `heartbeat` events when no other events have been produced for a short period.
  This makes UIs resilient to slow providers and long-running tools (e.g. `sleep`, builds) without requiring token streaming.

Job cancellation (best-effort):
- `POST /api/v1/job/cancel?job_id=<id>` requests cancellation.
- Cancellation is cooperative:
  - the tool loop will stop at safe boundaries (between tool calls / LLM requests)
  - long-running host tools (`shell_exec` / `proc_exec`) will terminate their subprocess when cancellation is requested

### Run the Web UI

```bash
cd ui
NPM_CONFIG_CACHE=../build/npm-cache npm ci
npm run dev
```

Then open the dev server URL (defaults to `http://localhost:5173`) and point it at the daemon base URL.
Using a repo-local `NPM_CONFIG_CACHE` avoids permission issues with the global npm cache in some environments.

UI rendering notes:
- The UI renders a **Conversation** (message cards) derived from the daemon `events` stream:
  user prompt → assistant messages → tool calls/results.
- Markdown is rendered with GFM + syntax highlighting for code blocks.
- The settings panel is collapsible and all settings persist in the browser via `localStorage`.
- If outbound networking requires a proxy, set **HTTPS proxy** in the UI settings (it is sent as `proxy` in `POST /api/v1/run`).

Host filesystem tools (token safety):
- `fs_list` is designed for bounded output and now excludes common huge directories by default (e.g. `node_modules`, `build`, `dist`).
  - To include them, pass `use_default_excludes: false` (and/or `exclude_names` to fine-tune).
- `fs_list`, `fs_find`, and `text_search` support `exclude_globs` (fnmatch) to filter out noisy paths (generated files, vendored code, etc.).
- `fs_list`, `fs_find`, and `text_search` support `respect_gitignore: true` to skip `.gitignore`d paths (best-effort; currently reads the repo-root `.gitignore`).
- `fs_stat` supports an optional bounded line count for small text files (`count_lines: true`) so the model can decide whether a file is huge without dumping it.
- `fs_read` supports paging (`start_line`, `max_lines`, `end_line`) and a character cap (`max_chars`).
