#!/usr/bin/env python3
"""Apply a repo skill package with backups and audit logging."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence


def _abort(msg: str) -> None:
    raise SystemExit(msg)


def _git_root(cwd: Path) -> Path:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=cwd, text=True
        ).strip()
        if out:
            return Path(out)
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return cwd


def _load_manifest(path: Path) -> Dict[str, Any]:
    if not path.exists():
        _abort(f"manifest not found: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        _abort(f"manifest JSON invalid: {exc}")


def _as_list(value: Any) -> List[str]:
    if value is None:
        return []
    if not isinstance(value, list):
        _abort("manifest field must be a list")
    out: List[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            _abort("manifest list entries must be non-empty strings")
        out.append(item.strip())
    return out


def _normalize_manifest(manifest: Dict[str, Any]) -> Dict[str, Any]:
    required = ["skill", "version", "description"]
    for key in required:
        if key not in manifest or not isinstance(manifest[key], str):
            _abort(f"manifest missing required field: {key}")
    manifest = dict(manifest)
    manifest["adds"] = _as_list(manifest.get("adds"))
    manifest["modifies"] = _as_list(manifest.get("modifies"))
    manifest["patches"] = _as_list(manifest.get("patches"))
    if "apply" in manifest and not isinstance(manifest["apply"], str):
        _abort("manifest apply must be a string path if present")
    if "post_apply" in manifest:
        manifest["post_apply"] = _as_list(manifest.get("post_apply"))
    else:
        manifest["post_apply"] = []
    return manifest


def _iter_backup_paths(adds: Sequence[str], modifies: Sequence[str]) -> List[str]:
    seen = set()
    ordered: List[str] = []
    for entry in list(adds) + list(modifies):
        if entry in seen:
            continue
        seen.add(entry)
        ordered.append(entry)
    return ordered


def _backup_path(src: Path, dst_root: Path) -> None:
    if not src.exists():
        return
    dst = dst_root / src.as_posix()
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)


def _run(cmd: Sequence[str], cwd: Path, env: Optional[Dict[str, str]] = None) -> None:
    result = subprocess.run(cmd, cwd=cwd, env=env)
    if result.returncode != 0:
        _abort(f"command failed ({result.returncode}): {' '.join(cmd)}")


def _apply_patches(
    patches: Sequence[Path],
    root: Path,
    dry_run: bool,
) -> List[str]:
    applied: List[str] = []
    for patch in patches:
        if not patch.exists():
            _abort(f"patch not found: {patch}")
        _run(["git", "apply", "--check", str(patch)], cwd=root)
        if dry_run:
            continue
        _run(["git", "apply", str(patch)], cwd=root)
        applied.append(str(patch))
    return applied


def _collect_patches(skill_dir: Path, manifest: Dict[str, Any]) -> List[Path]:
    explicit = [skill_dir / p for p in manifest.get("patches", [])]
    if explicit:
        return explicit
    patches_dir = skill_dir / "patches"
    if not patches_dir.exists():
        return []
    return sorted(patches_dir.glob("*.patch"))


def _record_state(
    state_path: Path,
    entry: Dict[str, Any],
) -> None:
    state_path.parent.mkdir(parents=True, exist_ok=True)
    if state_path.exists():
        try:
            state = json.loads(state_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            state = []
    else:
        state = []
    if not isinstance(state, list):
        state = []
    state.append(entry)
    state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description="Apply a repo skill package")
    parser.add_argument("skill_dir", help="Path to skill directory")
    parser.add_argument(
        "--root",
        help="Repo root (default: git root of cwd)",
        default=None,
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and stage without applying patches or scripts",
    )
    args = parser.parse_args(argv)

    skill_dir = Path(args.skill_dir).expanduser().resolve()
    repo_root = Path(args.root).expanduser().resolve() if args.root else _git_root(Path.cwd())

    manifest_path = skill_dir / "manifest.json"
    manifest = _normalize_manifest(_load_manifest(manifest_path))

    timestamp = dt.datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    out_dir = repo_root / "out" / "skills" / manifest["skill"] / timestamp
    backup_dir = out_dir / "backup"

    backup_dir.mkdir(parents=True, exist_ok=True)

    to_backup = _iter_backup_paths(manifest["adds"], manifest["modifies"])
    for rel_path in to_backup:
        _backup_path(repo_root / rel_path, backup_dir)

    patches = _collect_patches(skill_dir, manifest)
    applied_patches = _apply_patches(patches, repo_root, args.dry_run)

    env = os.environ.copy()
    env.update(
        {
            "SKILL_DIR": str(skill_dir),
            "SKILL_ROOT": str(repo_root),
            "SKILL_OUT": str(out_dir),
            "SKILL_NAME": manifest["skill"],
            "SKILL_VERSION": manifest["version"],
            "SKILL_DRY_RUN": "1" if args.dry_run else "0",
        }
    )

    apply_script = manifest.get("apply")
    if apply_script:
        apply_path = skill_dir / apply_script
        if not apply_path.exists():
            _abort(f"apply script not found: {apply_path}")
        if not args.dry_run:
            _run(["bash", str(apply_path)], cwd=repo_root, env=env)

    if not args.dry_run:
        for cmd in manifest.get("post_apply", []):
            _run(["bash", "-lc", cmd], cwd=repo_root, env=env)

    record = {
        "skill": manifest["skill"],
        "version": manifest["version"],
        "description": manifest["description"],
        "manifest": str(manifest_path),
        "timestamp": timestamp,
        "repo_root": str(repo_root),
        "backup_dir": str(backup_dir),
        "applied_patches": applied_patches,
        "dry_run": bool(args.dry_run),
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "applied.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8"
    )

    if not args.dry_run:
        _record_state(repo_root / "state" / "skills_state.json", record)

    print(f"skill applied: {manifest['skill']} ({manifest['version']})")
    print(f"output: {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
