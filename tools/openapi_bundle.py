#!/usr/bin/env python3
"""Inline simple OpenAPI $ref blocks for paths/components.

This helper expects a root spec that uses:
  paths:
    $ref: ./<subdir>/paths.yaml
  components:
    $ref: ./<subdir>/components.yaml

It inlines those files for consumers that do not resolve $ref.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import List


def _read_lines(path: Path) -> List[str]:
    return path.read_text().splitlines()


def _parse_ref(line: str) -> str:
    stripped = line.strip()
    if not stripped.startswith("$ref:"):
        raise ValueError(f"expected $ref line, got: {line!r}")
    value = stripped[len("$ref:") :].strip()
    if not value:
        raise ValueError("empty $ref value")
    if value.startswith("#"):
        raise ValueError("fragment-only $ref is not supported")
    if value.startswith("'") and value.endswith("'"):
        value = value[1:-1]
    if value.startswith('"') and value.endswith('"'):
        value = value[1:-1]
    if "#" in value:
        value = value.split("#", 1)[0]
    return value


def _inline_block(lines: List[str], key: str, base_dir: Path) -> List[str]:
    out: List[str] = []
    idx = 0
    while idx < len(lines):
        line = lines[idx]
        out.append(line)
        if line == f"{key}:":
            idx += 1
            # skip blank lines between key and $ref
            while idx < len(lines) and not lines[idx].strip():
                out.append(lines[idx])
                idx += 1
            if idx >= len(lines):
                raise ValueError(f"missing $ref for {key}")
            ref_line = lines[idx]
            ref_path = _parse_ref(ref_line)
            ref_file = (base_dir / ref_path).resolve()
            ref_lines = _read_lines(ref_file)
            for ref_line_item in ref_lines:
                out.append(f"  {ref_line_item}" if ref_line_item else "")
            idx += 1
            continue
        idx += 1
    return out


def bundle_spec(spec_path: Path) -> str:
    base_dir = spec_path.parent
    lines = _read_lines(spec_path)
    lines = _inline_block(lines, "paths", base_dir)
    lines = _inline_block(lines, "components", base_dir)
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Inline OpenAPI $ref blocks for paths/components.")
    parser.add_argument("spec", type=Path, help="Root OpenAPI spec (agentd.yaml/broker.yaml)")
    parser.add_argument("-o", "--output", type=Path, help="Output file path (defaults to stdout)")
    args = parser.parse_args()

    bundled = bundle_spec(args.spec)
    if args.output:
        args.output.write_text(bundled)
        return 0
    print(bundled, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
