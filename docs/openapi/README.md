# OpenAPI specs layout

The OpenAPI specs are split into a small root file plus referenced subfiles.

- `agentd.yaml` references:
  - `agentd/paths.yaml`
  - `agentd/components.yaml` (which indexes `agentd/components/*.yaml`)
- `broker.yaml` references:
  - `broker/paths.yaml`
  - `broker/components.yaml`

Update the `paths.yaml` and `components.yaml` files for most changes. For
agentd schemas, edit the domain-specific files under
`docs/openapi/agentd/components/`. Keep the root specs (`agentd.yaml`,
`broker.yaml`) focused on metadata, tags, and top-level `$ref` wiring so
tooling can resolve the full contract.

When a fragment under `agentd/components/` or `broker/components/` references
`#/components/schemas/...`, that schema must also be exported through the
spec-local `components.yaml` index. The UI type generator now validates those
exports before bundling so missing registry entries fail with a focused error.

## Bundling helper

For consumers that do not resolve `$ref` values, use:

```bash
tools/openapi_bundle.py docs/openapi/agentd.yaml -o out/agentd.openapi.yaml
tools/openapi_bundle.py docs/openapi/broker.yaml -o out/broker.openapi.yaml
python3 tools/check_openapi_component_refs.py docs/openapi/agentd.yaml
python3 tools/check_openapi_component_refs.py docs/openapi/broker.yaml
```

## WebUI generated types

The WebUI now keeps generated TypeScript types under `ui/src/api/generated/`.
Refresh them from the repo root with:

```bash
cd ui && npm run openapi:types
```

CI/local verification checks these files with `npm run openapi:types:check`,
so OpenAPI edits need regenerated UI types in the same change.
