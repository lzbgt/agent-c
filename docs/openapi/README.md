# OpenAPI specs layout

The OpenAPI specs are split into a small root file plus referenced subfiles.

- `agentd.yaml` references:
  - `agentd/paths.yaml`
  - `agentd/components.yaml`
- `broker.yaml` references:
  - `broker/paths.yaml`
  - `broker/components.yaml`

Update the `paths.yaml` and `components.yaml` files for most changes. Keep the
root specs (`agentd.yaml`, `broker.yaml`) focused on metadata, tags, and the
top-level `$ref` wiring so tooling can resolve the full contract.
