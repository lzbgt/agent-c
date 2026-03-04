#!/usr/bin/env python3
"""Validate skill manifest(s) against the local schema rules."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List


ALLOWED_KEYS = {
    "skill",
    "version",
    "description",
    "adds",
    "modifies",
    "patches",
    "apply",
    "post_apply",
}


class ManifestError(Exception):
    pass


def _abort(msg: str) -> None:
    raise ManifestError(msg)


def _load_manifest(path: Path) -> Dict[str, Any]:
    if not path.exists():
        _abort(f"manifest not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        _abort(f"manifest JSON invalid: {exc}")


def _validate_list(value: Any, field: str) -> List[str]:
    if value is None:
        return []
    if not isinstance(value, list):
        _abort(f"{field} must be a list")
    out: List[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            _abort(f"{field} entries must be non-empty strings")
        out.append(item.strip())
    return out


def _validate_manifest(path: Path) -> None:
    manifest = _load_manifest(path)
    if not isinstance(manifest, dict):
        _abort("manifest must be a JSON object")

    extra_keys = set(manifest.keys()) - ALLOWED_KEYS
    if extra_keys:
        _abort(f"unexpected keys: {', '.join(sorted(extra_keys))}")

    for key in ("skill", "version", "description"):
        if key not in manifest or not isinstance(manifest[key], str) or not manifest[key].strip():
            _abort(f"missing or invalid field: {key}")

    adds = _validate_list(manifest.get("adds"), "adds")
    modifies = _validate_list(manifest.get("modifies"), "modifies")
    patches = _validate_list(manifest.get("patches"), "patches")
    post_apply = _validate_list(manifest.get("post_apply"), "post_apply")

    apply_script = manifest.get("apply")
    if apply_script is not None:
        if not isinstance(apply_script, str) or not apply_script.strip():
            _abort("apply must be a non-empty string path")
        apply_path = path.parent / apply_script
        if not apply_path.exists():
            _abort(f"apply script not found: {apply_path}")

    for patch in patches:
        patch_path = path.parent / patch
        if not patch_path.exists():
            _abort(f"patch not found: {patch_path}")

    if (path.parent / "patches").exists() and not patches:
        # patches dir exists but manifest doesn't reference patches
        _abort("patches/ exists but manifest.patches is empty")

    # Basic sanity: don't allow overlap between adds/modifies
    overlap = set(adds).intersection(modifies)
    if overlap:
        _abort(f"adds/modifies overlap: {', '.join(sorted(overlap))}")

    _ = post_apply  # reserved for future checks


def _iter_manifests(paths: Iterable[Path]) -> List[Path]:
    manifests: List[Path] = []
    for p in paths:
        if p.is_dir():
            candidate = p / "manifest.json"
            if candidate.exists():
                manifests.append(candidate)
        elif p.name == "manifest.json":
            manifests.append(p)
    return manifests


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Validate skill manifest(s)")
    parser.add_argument("paths", nargs="+", help="Skill dir(s) or manifest.json files")
    args = parser.parse_args(argv)

    manifests = _iter_manifests([Path(p).expanduser().resolve() for p in args.paths])
    if not manifests:
        print("no manifests found", file=sys.stderr)
        return 2

    errors = 0
    for manifest in manifests:
        try:
            _validate_manifest(manifest)
            print(f"OK: {manifest}")
        except ManifestError as exc:
            errors += 1
            print(f"FAIL: {manifest}: {exc}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
