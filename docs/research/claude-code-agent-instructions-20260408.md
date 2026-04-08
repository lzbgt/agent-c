# Claude Code Borrow: Hierarchical Project Instructions

Date: 2026-04-08

## Source material saved locally

- Official repo README: `docs/research/sources/claude-code/README.md`
- Official memory/instructions docs: `docs/research/sources/claude-code/memory.html`
- Official sub-agents docs: `docs/research/sources/claude-code/sub-agents.html`
- Official slash commands docs: `docs/research/sources/claude-code/slash-commands.html`
- Official hooks docs: `docs/research/sources/claude-code/hooks.html`

## What was valuable

The highest-leverage Claude Code idea for this repo was not "memory" because `agent` already has a substantial durable
memory system. The most valuable gap was Claude Code's hierarchical project instruction loading:

- persistent repo-local instructions
- ordered loading from broader scope to narrower scope
- automatic refresh on the next run instead of relying on chat history

The official Claude Code memory page explicitly distinguishes durable memory from instruction files and documents
hierarchical `CLAUDE.md` loading. That maps well onto this framework's existing `AGENTS.md` convention.

## What was implemented here

Borrowed concept:

- Load `AGENTS.md` files from the working directory upward for host runs.
- Order them parent-first so deeper directories can override broader repo rules.
- Pin the resulting instructions into the session so they survive compaction.
- Re-read and refresh the pinned project instructions on later runs if the files change.

Repo-specific adaptation:

- Claude Code delivers instruction files outside the system prompt path. This framework instead injects a bounded,
  pinned `system` message because the core session model already has a strong notion of leading pinned system messages.
- The injected prompt is marked with `PROJECT_INSTRUCTIONS=agmd-v1` so long-lived sessions can deduplicate and refresh it.
- The implementation is bounded to avoid runaway prompt growth:
  - up to 8 `AGENTS.md` files
  - up to 16 KiB per file
  - up to 48 KiB total

## Why this fit better than other Claude Code features

Other Claude Code ideas are interesting, but they were lower leverage for the current framework state:

- slash commands:
  useful, but this repo already has multiple operator surfaces and a deeper control-plane problem
- sub-agents:
  the repo already has workflow/delegation machinery; the bigger near-term gap was operator guidance, not execution fan-out
- hooks:
  valuable, but they introduce a larger safety and policy surface than project instruction loading

Project instruction loading is small enough to land safely and large enough to materially improve host-run behavior.

## Follow-on ideas worth considering later

- Scoped runtime skills/commands for repeatable workflows, closer to Claude Code slash commands and skills.
- Hook points around tool execution with explicit policy review and audit logging.
- Path-scoped instruction fragments once the base `AGENTS.md` flow proves stable in production.
