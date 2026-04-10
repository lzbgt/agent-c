# Runtime Skills (Declarative Runtime Reuse Bundles)

This folder implements the v0 local tooling for **runtime skills** described in
`docs/spec/runtime_skills_v0.md`.

Runtime skills are **not** executable plugins and **not** repo-transform skills.
They are declarative bundles that reference existing runtime primitives such as
tools, workflow templates, team templates, policy presets, and instruction
fragments.

## Layout

```text
tools/runtime_skills/
  catalog/
    <skill-id>/
      manifest.json
  local/
    <skill-id>/          # local operator skills, gitignored
      manifest.json
  templates/
    sample-runtime-skill/
      manifest.json
```

The default v0 search order is:

1. `tools/runtime_skills/`
2. `state/runtime_skills/`

Within each root, the first discovered `skill_id` wins.

## Manifest format

Required fields:

- `skill_id`
- `version`
- `description`
- `kind`

Optional fields:

- `requires.tools`
- `requires.plugins`
- `requires.features`
- `inputs_schema`
- `instruction_fragments`
- `workflow_template`
- `team_template`
- `policy_preset`
- `ui`

## Commands

List skills from the default catalog search path:

```bash
python3 tools/runtime_skills/list_skills.py
python3 tools/runtime_skills/list_skills.py --json
```

Validate manifest(s):

```bash
python3 tools/runtime_skills/validate_manifest.py tools/runtime_skills/catalog/researcher-writer-reviewer
python3 tools/runtime_skills/validate_manifest.py tools/runtime_skills/catalog/coding-test-gate
```

Resolve a runtime skill by `skill_id` plus inputs:

```bash
python3 tools/runtime_skills/resolve_skill.py researcher-writer-reviewer \
  --inputs-json '{"goal":"Investigate regression cause"}'
```

Resolve with capability checks:

```bash
python3 tools/runtime_skills/resolve_skill.py coding-test-gate \
  --inputs-json '{"goal":"Fix failing test","test_command":"pytest -q"}' \
  --feature approval_queue
```

The resolver now emits `materialized.workflow_request` for `workflow_bundle`
skills. The materialized request:

- deep-copies `workflow_template`
- merges user inputs into `workflow_request.inputs`
- seeds `defaults.max_steps` from `policy_preset.max_steps` when absent
- attaches `runtime_skill.{skill_id,skill_version,manifest_sha256,inputs}` for audit

Create a new local runtime skill from the template:

```bash
python3 tools/runtime_skills/create_skill.py my-runtime-skill
```

## Notes

- Inputs are always accepted from JSON. YAML input files are also accepted when
  `PyYAML` is installed in the current Python environment.
- The resolver emits a concrete JSON document that includes:
  - `skill_id`
  - `skill_version`
  - normalized `manifest_sha256`
  - input values
  - the manifest snapshot
- agentd now exposes the same catalog through:
  - `GET /api/v1/runtime_skills`
  - `POST /api/v1/runtime_skills/resolve`
- The WebUI workflow composer consumes those endpoints to offer “start from skill”
  for `workflow_bundle` entries.
- Validation is data-only and does not execute arbitrary code.
