# Vendored Subtrees (Read-Only Policy)

This repo includes upstream snapshots under `ref/` for reference and parity.
These directories are **read-only** in this repo to avoid divergence from their
upstream sources. If changes are required, prefer updating upstream or copying
the needed logic into first-party code under `tools/`, `scripts/`, or another
project-owned location.

Guard: `tools/vendored_guard.py` fails CI if files under `ref/` change. Override
with `ALLOW_VENDORED_CHANGES=1` only when intentionally updating a vendored
snapshot.
Optional local hook install/uninstall:
`tools/install_git_hooks.sh`, `tools/uninstall_git_hooks.sh`.
Hook status: `tools/hooks_status.sh`.

## Current vendored subtrees

- `ref/claude-mem/` (upstream: thedotmack/claude-mem)
  - `ref/claude-mem/openclaw/test-install.sh` is a 2.3k+ line upstream test
    suite; it remains read-only here. If we need to adjust behavior, create a
    smaller first-party test harness instead of modifying the vendored file.
- `ref/ds-cli/` (upstream: ds-cli)
- `ref/openrouter/` (upstream: OpenRouter references)
