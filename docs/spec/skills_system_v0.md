# Skills System (v0)

Date: 2026-03-04
Status: draft (rolling)

## Summary

This spec defines a **minimal, auditable skills system** for applying scripted
changes to this repo. A skill is a directory containing a `manifest.json` plus
optional patches and scripts. Skills are applied with backups and recorded
state so changes are reviewable and reversible.

## Goals

- Provide a **lightweight transform pipeline** for common changes (providers,
  connectors, policy presets) without bloating runtime config.
- Keep changes **auditable**: backups + applied metadata are written to `out/`.
- Keep the system **safe by default**: strict manifest validation and explicit
  apply/preview steps.

## Non-goals

- Replacing the normal git workflow (skills are an optional helper).
- Building a full plugin marketplace or dependency resolver.

## Terminology

- **Skill**: a repo transform package (directory with `manifest.json`).
- **Apply**: run patches/scripts against the repo and record results.
- **Preview**: apply the skill in a temporary git worktree and capture diffs.

## Skill layout

```
<skill>/
  manifest.json
  patches/         # optional
  apply.sh         # optional
```

## Manifest schema

See `tools/skills/schema.json` for the canonical schema. Core fields:

- `skill` (string, required): short name
- `version` (string, required): semver-like version string
- `description` (string, required): human-readable summary
- `adds` (array<string>, optional): expected new paths
- `modifies` (array<string>, optional): expected modified paths
- `patches` (array<string>, optional): patch files relative to the skill dir
- `apply` (string, optional): script to execute (relative to the skill dir)
- `post_apply` (array<string>, optional): shell commands run after apply

## Execution flows

### Apply

```
python3 tools/skills/apply_skill.py /path/to/skill
```

Behavior:
- Creates backups under `out/skills/<skill>/<timestamp>/backup/`.
- Applies patches (if any) and runs `apply` + `post_apply` steps.
- Writes `out/skills/<skill>/<timestamp>/applied.json`.
- Appends to `state/skills_state.json` (gitignored).

### Preview

```
python3 tools/skills/preview_skill.py /path/to/skill
```

Behavior:
- Creates a temporary git worktree at `out/skills/preview/<timestamp>/worktree`.
- Applies the skill inside the worktree.
- Captures `diff.patch` + `diff.stat` in `out/skills/preview/<timestamp>/`.
- Removes the worktree unless `--keep-worktree` is provided.

### Validate

```
python3 tools/skills/validate_manifest.py /path/to/skill
```

Behavior:
- Validates required fields + basic safety rules (no overlap in adds/modifies,
  patches exist when referenced, apply script exists).

### Status

```
python3 tools/skills/skill_status.py
```

Behavior:
- Reads `state/skills_state.json` and prints a summary table.

## Safety considerations

- Skills are **opt-in**. Operators must run the scripts explicitly.
- Backups are retained per skill application to enable manual rollback.
- Manifest validation is enforced in repo guards for template skills.

## Follow-up ideas

- Optional CI check for non-template skills when present.
- WebUI “Guided change” panel for preview/apply flows.

## Acceptance criteria

- Spec is linked from `docs/spec/README.md`.
- Skill tooling remains self-contained under `tools/skills/`.
