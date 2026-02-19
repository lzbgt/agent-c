#!/usr/bin/env python3
"""Inline local OpenAPI $ref blocks for paths/components (recursively).

This helper expects a root spec that uses:
  paths:
    $ref: ./<subdir>/paths.yaml
  components:
    $ref: ./<subdir>/components.yaml

It inlines local file references (including fragment refs such as
./components/common.yaml#/TraceID) for consumers that do not resolve $ref.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


def _read_lines(path: Path) -> List[str]:
    return path.read_text().splitlines()


def _parse_ref(line: str) -> Tuple[Optional[str], Optional[str]]:
    stripped = line.strip()
    if not stripped.startswith("$ref:"):
        raise ValueError(f"expected $ref line, got: {line!r}")
    value = stripped[len("$ref:") :].strip()
    if not value:
        raise ValueError("empty $ref value")
    if value.startswith("'") and value.endswith("'"):
        value = value[1:-1]
    if value.startswith('"') and value.endswith('"'):
        value = value[1:-1]
    if value.startswith("#"):
        return None, value
    if "://" in value:
        return None, value
    if "#" in value:
        path, fragment = value.split("#", 1)
        if not path:
            return None, f"#{fragment}"
        return path, fragment
    return value, None


def _fragment_key(fragment: str) -> str:
    frag = fragment.lstrip("#")
    if frag.startswith("/"):
        frag = frag[1:]
    if not frag or "/" in frag:
        raise ValueError(f"unsupported $ref fragment: {fragment!r}")
    return frag


def _extract_block(lines: Sequence[str], key: str) -> List[str]:
    for idx, line in enumerate(lines):
        if line.strip() != f"{key}:":
            continue
        base_indent = len(line) - len(line.lstrip())
        block: List[str] = []
        for next_line in lines[idx + 1 :]:
            if next_line.strip() and (len(next_line) - len(next_line.lstrip())) <= base_indent:
                break
            block.append(next_line)
        trim = base_indent + 2
        trimmed: List[str] = []
        for item in block:
            if not item:
                trimmed.append("")
                continue
            if item.startswith(" " * trim):
                trimmed.append(item[trim:])
            else:
                trimmed.append(item.lstrip())
        return trimmed
    raise ValueError(f"missing fragment key: {key}")


def _inline_lines(
    lines: Sequence[str], base_dir: Path, indent: int, seen: set[Tuple[Path, str]]
) -> List[str]:
    out: List[str] = []
    block_indent: Optional[int] = None
    for line in lines:
        stripped = line.strip()
        line_indent = len(line) - len(line.lstrip())
        if block_indent is not None:
            if not stripped or line_indent > block_indent:
                out.append((" " * indent + line) if line else "")
                continue
            block_indent = None
        if ":" in stripped:
            _, tail = stripped.split(":", 1)
            tail = tail.strip()
            if tail.startswith("|") or tail.startswith(">"):
                block_indent = line_indent
        if stripped.startswith("$ref:"):
            ref_path, fragment = _parse_ref(stripped)
            if ref_path is None:
                out.append((" " * indent + line) if line else "")
                continue
            ref_file = (base_dir / ref_path).resolve()
            ref_key = fragment or ""
            token = (ref_file, ref_key)
            if token in seen:
                raise ValueError(f"recursive $ref detected: {ref_file}#{ref_key}")
            seen.add(token)
            ref_lines = _read_lines(ref_file)
            next_indent = indent + line_indent
            if fragment:
                key = _fragment_key(fragment)
                block = _extract_block(ref_lines, key)
                inlined = _inline_lines(block, ref_file.parent, next_indent, seen)
            else:
                inlined = _inline_lines(ref_lines, ref_file.parent, next_indent, seen)
            seen.remove(token)
            out.extend(inlined)
        else:
            out.append((" " * indent + line) if line else "")
    return out


def bundle_spec(spec_path: Path) -> str:
    base_dir = spec_path.parent
    lines = _read_lines(spec_path)
    bundled = _inline_lines(lines, base_dir, 0, set())
    return "\n".join(bundled) + "\n"


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
