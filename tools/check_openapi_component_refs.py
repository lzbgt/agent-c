#!/usr/bin/env python3
"""Validate that root-schema refs are exported through a spec's components.yaml."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SCHEMA_REF_RE = re.compile(r"#/components/schemas/([A-Za-z0-9_]+)")
SCHEMA_EXPORT_RE = re.compile(r"^  ([A-Za-z0-9_]+):\s*$")


def load_schema_exports(components_index: Path) -> set[str]:
    text = components_index.read_text(encoding="utf-8")
    exports: set[str] = set()
    in_schemas = False
    for line in text.splitlines():
        if not in_schemas:
            if line.strip() == "schemas:":
                in_schemas = True
            continue
        if line and not line.startswith("  "):
            break
        match = SCHEMA_EXPORT_RE.match(line)
        if match:
            exports.add(match.group(1))
    return exports


def spec_fragment_files(root_spec: Path) -> list[Path]:
    spec_dir = root_spec.parent / root_spec.stem
    files = [root_spec]
    if spec_dir.is_dir():
        files.extend(sorted(spec_dir.rglob("*.yaml")))
    return files


def collect_missing_exports(root_spec: Path) -> dict[Path, list[str]]:
    spec_dir = root_spec.parent / root_spec.stem
    components_index = spec_dir / "components.yaml"
    if not components_index.is_file():
        raise FileNotFoundError(f"missing components index: {components_index}")
    exports = load_schema_exports(components_index)

    missing_by_file: dict[Path, list[str]] = {}
    for yaml_path in spec_fragment_files(root_spec):
        text = yaml_path.read_text(encoding="utf-8")
        missing = sorted({name for name in SCHEMA_REF_RE.findall(text) if name not in exports})
        if missing:
            missing_by_file[yaml_path] = missing
    return missing_by_file


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate OpenAPI #/components/schemas refs are exported via components.yaml.",
    )
    parser.add_argument("spec", type=Path, help="Root OpenAPI spec, for example docs/openapi/agentd.yaml")
    args = parser.parse_args()

    root_spec = args.spec.resolve()
    try:
        missing_by_file = collect_missing_exports(root_spec)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not missing_by_file:
        return 0

    components_index = root_spec.parent / root_spec.stem / "components.yaml"
    print(
        f"openapi component export drift: {components_index} is missing schema exports",
        file=sys.stderr,
    )
    for yaml_path, missing in sorted(missing_by_file.items()):
        rel_path = yaml_path.relative_to(root_spec.parent.parent)
        print(f"  {rel_path}:", file=sys.stderr)
        for name in missing:
            print(f"    - {name}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
