# Tool Sandbox Mount Allowlist (v0)

Date: 2026-03-04
Status: draft (rolling)

## Summary

Define a **host-side mount allowlist** for sandboxed tool execution. The
allowlist lives outside the repo so agents cannot modify it, and is consulted
before any host path is mounted into a sandboxed tool runner.

## Goals

- Prevent tool sandboxes from mounting sensitive host paths by default.
- Keep allowlist configuration **tamper‑proof** (not writable by agents).
- Provide clear, auditable guardrails for operators.
- Preserve existing `host_policy` semantics (readonly vs full) while tightening
  the sandbox’s filesystem exposure.

## Non‑goals

- Enforcing policy for in‑process host tools (this spec targets sandboxed tools).
- Replacing existing tool server sandboxing (this spec complements it).

## Threat model

- **Prompt injection** could coerce an agent into reading secrets from host
  mounts. We must constrain what can be mounted even if a tool server is
  compromised.
- **Path traversal** or symlink tricks could escape allowlisted roots if not
  resolved properly.

## Configuration

Allowlist file (outside repo):

- `~/.config/agent/mount-allowlist.json`

Example:

```json
{
  "allowed_roots": [
    { "path": "~/Documents", "readonly": true },
    { "path": "/data/projects", "readonly": false }
  ],
  "blocked_patterns": [
    ".ssh",
    ".gnupg",
    ".aws",
    ".kube",
    ".env",
    "id_rsa",
    "private_key"
  ],
  "non_main_readonly": true
}
```

### Fields

- `allowed_roots`: array of root paths that may be mounted. Each entry supports:
  - `path` (string, required)
  - `readonly` (bool, default true)
- `blocked_patterns`: denylist substrings checked against real paths.
- `non_main_readonly`: if true, force non‑main sandboxes to read‑only even if
  a root is configured writable.

## Validation rules

1. Expand `~` to home directory.
2. Resolve symlinks (`realpath`) for candidate mounts and roots.
3. Require candidate to be within an allowed root.
4. Reject any candidate matching blocked patterns (path segment or substring).
5. Enforce read‑only if `non_main_readonly` and sandbox is non‑main.
6. Reject container mount paths outside the allowed sandbox mount prefix
   (e.g., `/workspace/extra`).

## Runtime behavior

- If the allowlist file is missing or invalid, **additional mounts are blocked**.
- A warning is logged once at startup; no fallback to permissive behavior.
- Project root is mounted read‑only in sandbox mode; writable paths must be
  explicitly mounted (workspace, IPC, scratch).

## Integration points

- `agentd` sandbox tool runner:
  - Add allowlist load + validation module.
  - Enforce allowlist before passing mounts to the sandbox runtime.
- `caps` / config endpoint:
  - Report whether allowlist is present + loaded (boolean + path).
- WebUI:
  - Surface allowlist status in diagnostics (read‑only view).

## Logging & audit

- Log allowlist load success/failure once at startup.
- Log blocked mount attempts with reason (pattern match, outside root, etc.).

## Follow‑ups

- Add a helper script to scaffold the allowlist file with safe defaults.
- Add a test harness for path validation (realpath traversal, blocked patterns).

## Acceptance criteria

- Spec linked from `docs/spec/README.md`.
- TODO entry added for implementation milestones.
