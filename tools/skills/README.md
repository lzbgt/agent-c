# Skills (Repo Transform Packages)

This folder defines a lightweight, auditable way to apply scripted changes to
this repo ("skills"). A skill is a directory with a `manifest.json` plus
optional patches or scripts. Applying a skill creates backups and an audit
record under `out/skills/` and appends state to `state/skills_state.json`
(gitignored).

## Skill layout

```
my-skill/
  manifest.json
  patches/
    0001-change.patch
  apply.sh              # optional
```

## Manifest format

Required fields:
- `skill`: short name
- `version`: semver string
- `description`: human-readable summary

Optional fields:
- `adds`: list of new paths expected to be added
- `modifies`: list of paths expected to be modified
- `patches`: list of patch paths (relative to skill dir)
- `apply`: script to execute (relative to skill dir)
- `post_apply`: list of shell commands to run after apply

Example:

```json
{
  "skill": "add-provider-foo",
  "version": "0.1.0",
  "description": "Add Foo provider defaults + docs",
  "adds": ["docs/PROVIDERS_FOO.md"],
  "modifies": ["daemon/src/runtime_config.cpp"],
  "patches": ["patches/0001-foo-provider.patch"],
  "apply": "apply.sh",
  "post_apply": ["tools/verify_repo_guards.sh"]
}
```

## Apply a skill

```bash
python3 tools/skills/apply_skill.py /path/to/skill
```

Dry-run (validate and back up without applying patches/scripts):

```bash
python3 tools/skills/apply_skill.py /path/to/skill --dry-run
```

Notes:
- Backups are stored under `out/skills/<skill>/<timestamp>/backup/`.
- Applied records are stored in `out/skills/<skill>/<timestamp>/applied.json`.
- State is appended to `state/skills_state.json` (gitignored).
