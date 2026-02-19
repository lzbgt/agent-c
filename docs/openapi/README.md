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

## Bundling helper

For consumers that do not resolve `$ref` values, use:

```bash
tools/openapi_bundle.py docs/openapi/agentd.yaml -o out/agentd.openapi.yaml
tools/openapi_bundle.py docs/openapi/broker.yaml -o out/broker.openapi.yaml
```
