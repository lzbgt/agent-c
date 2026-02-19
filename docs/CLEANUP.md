# Cleanup and Repo Hygiene

Build artifacts and run logs can grow quickly (especially `build/`, `build-core*/`, and `out/`). Use the cleanup
tools to keep the repo lean and enforce size guardrails.

## Basic cleanup

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

## Repo size reports

Inspect disk usage and spot bloat quickly:

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

List nested .git dirs:

```bash
tools/repo_size_report.py --list-nested-git
```

## Guard scripts

Stub file scan (fails on empty/placeholder files):

```bash
tools/stub_file_scan.py --fail
```

Tracked file size guard (fails if any tracked file exceeds 10 MiB):

```bash
tools/tracked_file_guard.py --max-mb 10
```

Untracked file size guard (ignores common build/cache dirs):

```bash
tools/untracked_file_guard.py --exclude-defaults --max-mb 100
```

Vendored subtree guard (fails if `ref/` changes relative to base; requires base refs in CI; also checks staged/unstaged
working tree locally):

```bash
tools/vendored_guard.py --path ref
```

## Vendored guard hooks

Install the local pre-commit hook:

```bash
tools/install_git_hooks.sh
```

Remove the hook later:

```bash
tools/uninstall_git_hooks.sh
```

Check local hook status:

```bash
tools/hooks_status.sh
```

JSON output:

```bash
tools/hooks_status.sh --json
```

Fail if the vendored guard is not installed:

```bash
tools/hooks_status.sh --check
```

Scriptable check with JSON:

```bash
tools/hooks_status.sh --json --check
```
