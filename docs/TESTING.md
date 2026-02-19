# Testing Guide

This guide covers local verification, network smokes, and the real browser E2E harness.

## Quick verify (build + tests)

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

One-command verify (configure + build + tests; logs under `build/`):

```bash
tools/verify.sh
```

Include repo hygiene guards:

```bash
tools/verify.sh --repo-guards
```

Source `${HOME}/.env` before verification (provider keys for smokes):

```bash
tools/verify_prod.sh
```

## Network smoke tests

`ctest` includes network smokes (OpenRouter + DeepSeek). They run when keys are available via:
- environment variables, or
- `.not_in_repo` (preferred, gitignored), or
- `project.local.md` (gitignored).

Disable all network tests:

```bash
export AGENT_DISABLE_NETWORK_TESTS=1
```

Skip only OpenRouter tests:

```bash
export AGENT_TEST_SKIP_OPENROUTER=1
```

Network tests assume an HTTP proxy may be required; the scripts default to `http://localhost:8120`
via `https_proxy` / `http_proxy`. Use `AGENT_TEST_DISABLE_PROXY=1` to bypass the proxy.

Key file formats and precedence live in `README.md` (search for “Local secrets file”).

## Real end-to-end (agentd + browser) test

This repo includes a real E2E harness that drives the Web UI in a headless browser (Playwright)
and makes live provider calls via `agentd`.

Prereqs:
- `.not_in_repo` populated with provider keys (or env vars set)
- `./build/agentd` built (`tools/verify.sh`)
- UI deps installed (`cd ui && npm install`)

Run:

```bash
tools/e2e_real.sh
```

Logs and Playwright artifacts are written under `build/e2e/`.
