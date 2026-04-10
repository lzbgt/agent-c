#!/usr/bin/env python3
"""Shared helpers for runtime skill catalog discovery and validation."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Sequence


ALLOWED_KINDS = {
    "instruction_pack",
    "workflow_bundle",
    "team_bundle",
    "policy_bundle",
}

ALLOWED_KEYS = {
    "skill_id",
    "version",
    "description",
    "kind",
    "requires",
    "inputs_schema",
    "instruction_fragments",
    "workflow_template",
    "team_template",
    "policy_preset",
    "ui",
}

ALLOWED_REQUIRES_KEYS = {"tools", "plugins", "features"}
IGNORED_DISCOVERY_DIRS = {"templates", "__pycache__"}
SKILL_ID_RE = re.compile(r"^[a-z0-9]+(?:[-_][a-z0-9]+)*$")
SEMVER_LIKE_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z.+-]*$")


class RuntimeSkillError(Exception):
    """Raised for validation, discovery, or resolution failures."""


@dataclass(frozen=True)
class RuntimeSkillEntry:
    """A runtime skill manifest discovered from a catalog root."""

    root: Path
    manifest_path: Path
    manifest: Dict[str, Any]

    @property
    def skill_id(self) -> str:
        return str(self.manifest["skill_id"])


def repo_root(cwd: Path) -> Path:
    """Return the repo root, falling back to cwd when git is unavailable."""

    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=cwd, text=True
        ).strip()
        if out:
            return Path(out)
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return cwd


def default_catalog_roots(root: Path) -> List[Path]:
    """Return the v0 search order for runtime skill catalogs."""

    return [root / "tools" / "runtime_skills", root / "state" / "runtime_skills"]


def canonical_json(data: Any) -> str:
    """Serialize data into a stable JSON string."""

    return json.dumps(data, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def manifest_hash(manifest: Mapping[str, Any]) -> str:
    """Return a stable sha256 hash for a normalized manifest snapshot."""

    return hashlib.sha256(canonical_json(dict(manifest)).encode("utf-8")).hexdigest()


def load_data_file(path: Path) -> Any:
    """Load JSON, or YAML when PyYAML is available."""

    if not path.exists():
        raise RuntimeSkillError(f"file not found: {path}")

    suffix = path.suffix.lower()
    text = path.read_text(encoding="utf-8")
    if suffix == ".json":
        try:
            return json.loads(text)
        except json.JSONDecodeError as exc:
            raise RuntimeSkillError(f"invalid JSON in {path}: {exc}") from exc
    if suffix in {".yaml", ".yml"}:
        try:
            import yaml  # type: ignore
        except ModuleNotFoundError as exc:
            raise RuntimeSkillError(
                f"YAML inputs require PyYAML to be installed: {path}"
            ) from exc
        try:
            return yaml.safe_load(text)
        except Exception as exc:  # pragma: no cover - depends on optional parser
            raise RuntimeSkillError(f"invalid YAML in {path}: {exc}") from exc
    raise RuntimeSkillError(f"unsupported file type: {path}")


def _expect_non_empty_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise RuntimeSkillError(f"missing or invalid field: {field}")
    return value.strip()


def _validate_string_list(value: Any, field: str) -> List[str]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise RuntimeSkillError(f"{field} must be a list")
    items: List[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise RuntimeSkillError(f"{field} entries must be non-empty strings")
        items.append(item.strip())
    return items


def _validate_requires(value: Any) -> Dict[str, List[str]]:
    if value is None:
        return {"tools": [], "plugins": [], "features": []}
    if not isinstance(value, dict):
        raise RuntimeSkillError("requires must be an object")
    extra = set(value.keys()) - ALLOWED_REQUIRES_KEYS
    if extra:
        raise RuntimeSkillError(f"unexpected requires keys: {', '.join(sorted(extra))}")
    return {
        "tools": _validate_string_list(value.get("tools"), "requires.tools"),
        "plugins": _validate_string_list(value.get("plugins"), "requires.plugins"),
        "features": _validate_string_list(value.get("features"), "requires.features"),
    }


def _validate_inputs_schema(value: Any) -> None:
    if value is None:
        return
    if not isinstance(value, dict):
        raise RuntimeSkillError("inputs_schema must be an object")
    schema_type = value.get("type")
    if schema_type is not None and schema_type != "object":
        raise RuntimeSkillError("inputs_schema.type must be 'object' in v0")
    properties = value.get("properties")
    if properties is not None and not isinstance(properties, dict):
        raise RuntimeSkillError("inputs_schema.properties must be an object")
    required = value.get("required")
    if required is not None:
        _validate_string_list(required, "inputs_schema.required")


def _validate_instruction_fragments(value: Any) -> None:
    if value is None:
        return
    if not isinstance(value, dict):
        raise RuntimeSkillError("instruction_fragments must be an object")
    shared = value.get("shared")
    roles = value.get("roles")
    if shared is not None:
        _validate_string_list(shared, "instruction_fragments.shared")
    if roles is not None:
        if not isinstance(roles, dict):
            raise RuntimeSkillError("instruction_fragments.roles must be an object")
        for role, fragments in roles.items():
            if not isinstance(role, str) or not role.strip():
                raise RuntimeSkillError(
                    "instruction_fragments.roles keys must be non-empty strings"
                )
            _validate_string_list(
                fragments, f"instruction_fragments.roles.{role.strip()}"
            )


def validate_manifest_data(manifest: Any) -> Dict[str, Any]:
    """Validate a runtime skill manifest and return it as a dict."""

    if not isinstance(manifest, dict):
        raise RuntimeSkillError("manifest must be a JSON object")

    extra_keys = set(manifest.keys()) - ALLOWED_KEYS
    if extra_keys:
        raise RuntimeSkillError(f"unexpected keys: {', '.join(sorted(extra_keys))}")

    skill_id = _expect_non_empty_string(manifest.get("skill_id"), "skill_id")
    if not SKILL_ID_RE.match(skill_id):
        raise RuntimeSkillError(
            "skill_id must use lowercase letters, digits, '-' or '_'"
        )

    version = _expect_non_empty_string(manifest.get("version"), "version")
    if not SEMVER_LIKE_RE.match(version):
        raise RuntimeSkillError("version must be a semver-like string")

    _expect_non_empty_string(manifest.get("description"), "description")

    kind = _expect_non_empty_string(manifest.get("kind"), "kind")
    if kind not in ALLOWED_KINDS:
        raise RuntimeSkillError(
            f"kind must be one of: {', '.join(sorted(ALLOWED_KINDS))}"
        )

    _validate_requires(manifest.get("requires"))
    _validate_inputs_schema(manifest.get("inputs_schema"))
    _validate_instruction_fragments(manifest.get("instruction_fragments"))

    for field in ("workflow_template", "team_template", "policy_preset", "ui"):
        value = manifest.get(field)
        if value is not None and not isinstance(value, dict):
            raise RuntimeSkillError(f"{field} must be an object")

    ui = manifest.get("ui")
    if isinstance(ui, dict):
        for label_field in ("label", "category", "icon"):
            if label_field in ui and (
                not isinstance(ui[label_field], str) or not ui[label_field].strip()
            ):
                raise RuntimeSkillError(f"ui.{label_field} must be a non-empty string")

    return dict(manifest)


def load_manifest(path: Path) -> Dict[str, Any]:
    """Load and validate a manifest file."""

    data = load_data_file(path)
    if path.name != "manifest.json":
        raise RuntimeSkillError(f"runtime skill manifests must be named manifest.json: {path}")
    return validate_manifest_data(data)


def iter_manifest_paths(paths: Iterable[Path]) -> List[Path]:
    """Return manifest.json paths from dirs or manifest paths."""

    manifests: List[Path] = []
    for path in paths:
        if path.is_dir():
            candidate = path / "manifest.json"
            if candidate.exists():
                manifests.append(candidate)
        elif path.name == "manifest.json":
            manifests.append(path)
    return manifests


def discover_manifest_paths(roots: Sequence[Path]) -> List[Path]:
    """Recursively find manifest.json files in runtime skill roots."""

    found: List[Path] = []
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("manifest.json"):
            if any(part in IGNORED_DISCOVERY_DIRS for part in path.parts):
                continue
            found.append(path)
    return sorted(found)


def discover_skills(roots: Sequence[Path]) -> List[RuntimeSkillEntry]:
    """Load the catalog in the configured search order, first match wins."""

    entries: List[RuntimeSkillEntry] = []
    seen_skill_ids: set[str] = set()
    for root in roots:
        if not root.exists():
            continue
        root = root.resolve()
        manifests = [
            path
            for path in sorted(root.rglob("manifest.json"))
            if not any(part in IGNORED_DISCOVERY_DIRS for part in path.parts)
        ]
        for manifest_path in manifests:
            manifest = load_manifest(manifest_path)
            skill_id = str(manifest["skill_id"])
            if skill_id in seen_skill_ids:
                continue
            seen_skill_ids.add(skill_id)
            entries.append(
                RuntimeSkillEntry(
                    root=root,
                    manifest_path=manifest_path.resolve(),
                    manifest=manifest,
                )
            )
    return entries


def find_skill(skill_id: str, roots: Sequence[Path]) -> RuntimeSkillEntry:
    """Find a skill by id using v0 search order."""

    for entry in discover_skills(roots):
        if entry.skill_id == skill_id:
            return entry
    raise RuntimeSkillError(f"runtime skill not found: {skill_id}")


def merge_capabilities(
    *,
    capabilities_file: Path | None,
    tools: Sequence[str],
    plugins: Sequence[str],
    features: Sequence[str],
) -> Dict[str, List[str]]:
    """Merge capability inputs from flags and an optional JSON/YAML file."""

    merged = {"tools": list(tools), "plugins": list(plugins), "features": list(features)}
    if capabilities_file is None:
        return merged

    data = load_data_file(capabilities_file)
    if not isinstance(data, dict):
        raise RuntimeSkillError("capabilities file must contain an object")
    for key in ("tools", "plugins", "features"):
        merged[key].extend(_validate_string_list(data.get(key), f"capabilities.{key}"))
    return {key: sorted(set(values)) for key, values in merged.items()}


def missing_requirements(
    manifest: Mapping[str, Any], capabilities: Mapping[str, Sequence[str]]
) -> Dict[str, List[str]]:
    """Return missing requirement names per capability type."""

    requires = _validate_requires(manifest.get("requires"))
    missing: Dict[str, List[str]] = {}
    for key in ("tools", "plugins", "features"):
        available = set(capabilities.get(key, []))
        missing_values = [item for item in requires[key] if item not in available]
        if missing_values:
            missing[key] = missing_values
    return missing


def validate_inputs(inputs: Any, schema: Any, path: str = "inputs") -> None:
    """Validate user inputs against a small JSON-schema subset."""

    if schema is None:
        return
    if not isinstance(schema, dict):
        raise RuntimeSkillError("inputs_schema must be an object")

    expected_type = schema.get("type")
    enum_values = schema.get("enum")
    if enum_values is not None and inputs not in enum_values:
        raise RuntimeSkillError(f"{path} must be one of: {enum_values}")

    if expected_type is None:
        return

    if expected_type == "object":
        if not isinstance(inputs, dict):
            raise RuntimeSkillError(f"{path} must be an object")
        properties = schema.get("properties") or {}
        if properties and not isinstance(properties, dict):
            raise RuntimeSkillError(f"{path} schema properties must be an object")
        required = schema.get("required") or []
        if required and not isinstance(required, list):
            raise RuntimeSkillError(f"{path} schema required must be a list")
        for key in required:
            if key not in inputs:
                raise RuntimeSkillError(f"{path}.{key} is required")
        additional_properties = schema.get("additionalProperties", True)
        if additional_properties is False and properties:
            extra = set(inputs.keys()) - set(properties.keys())
            if extra:
                raise RuntimeSkillError(
                    f"{path} contains unexpected keys: {', '.join(sorted(extra))}"
                )
        for key, subschema in properties.items():
            if key in inputs:
                validate_inputs(inputs[key], subschema, f"{path}.{key}")
        return

    if expected_type == "array":
        if not isinstance(inputs, list):
            raise RuntimeSkillError(f"{path} must be an array")
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(inputs):
                validate_inputs(item, item_schema, f"{path}[{index}]")
        return

    if expected_type == "string":
        if not isinstance(inputs, str):
            raise RuntimeSkillError(f"{path} must be a string")
        return

    if expected_type == "boolean":
        if not isinstance(inputs, bool):
            raise RuntimeSkillError(f"{path} must be a boolean")
        return

    if expected_type == "integer":
        if isinstance(inputs, bool) or not isinstance(inputs, int):
            raise RuntimeSkillError(f"{path} must be an integer")
        return

    if expected_type == "number":
        if isinstance(inputs, bool) or not isinstance(inputs, (int, float)):
            raise RuntimeSkillError(f"{path} must be a number")
        return

    if expected_type == "null":
        if inputs is not None:
            raise RuntimeSkillError(f"{path} must be null")
        return

    raise RuntimeSkillError(f"{path} uses unsupported schema type: {expected_type}")


def _materialize_workflow_request(
    manifest: Mapping[str, Any], inputs: Mapping[str, Any]
) -> Dict[str, Any] | None:
    workflow_template = manifest.get("workflow_template")
    if workflow_template is None:
        return None
    if not isinstance(workflow_template, dict):
        raise RuntimeSkillError("workflow_template must be an object")

    workflow_request = json.loads(json.dumps(workflow_template, ensure_ascii=False))
    existing_inputs = workflow_request.get("inputs")
    if existing_inputs is None:
        merged_inputs: Dict[str, Any] = {}
    elif isinstance(existing_inputs, dict):
        merged_inputs = dict(existing_inputs)
    else:
        raise RuntimeSkillError("workflow_template.inputs must be an object when present")
    merged_inputs.update(dict(inputs))
    if merged_inputs:
        workflow_request["inputs"] = merged_inputs

    defaults = workflow_request.get("defaults")
    if defaults is None:
        defaults = {}
    elif not isinstance(defaults, dict):
        raise RuntimeSkillError("workflow_template.defaults must be an object when present")
    else:
        defaults = dict(defaults)
    policy_preset = manifest.get("policy_preset")
    if isinstance(policy_preset, dict):
        max_steps = policy_preset.get("max_steps")
        if isinstance(max_steps, int) and max_steps >= 0 and "max_steps" not in defaults:
            defaults["max_steps"] = max_steps
    if defaults:
        workflow_request["defaults"] = defaults

    workflow_request["runtime_skill"] = {
        "skill_id": manifest["skill_id"],
        "skill_version": manifest["version"],
        "manifest_sha256": manifest_hash(manifest),
        "inputs": dict(inputs),
    }
    return workflow_request


def build_resolution_document(
    entry: RuntimeSkillEntry,
    *,
    inputs: Mapping[str, Any],
    capabilities: Mapping[str, Sequence[str]] | None,
) -> Dict[str, Any]:
    """Build a resolved runtime skill document for audit or materialization."""

    manifest = entry.manifest
    schema = manifest.get("inputs_schema")
    validate_inputs(dict(inputs), schema)

    missing = missing_requirements(manifest, capabilities or {})
    if capabilities and missing:
        details = ", ".join(
            f"{key}={values}" for key, values in sorted(missing.items())
        )
        raise RuntimeSkillError(f"missing runtime requirements: {details}")

    return {
        "skill_id": manifest["skill_id"],
        "skill_version": manifest["version"],
        "description": manifest["description"],
        "kind": manifest["kind"],
        "manifest_sha256": manifest_hash(manifest),
        "source_manifest": str(entry.manifest_path),
        "inputs": dict(inputs),
        "manifest": manifest,
        "resolved": {
            "requires": manifest.get("requires", {}),
            "instruction_fragments": manifest.get("instruction_fragments", {}),
            "policy_preset": manifest.get("policy_preset", {}),
            "team_template": manifest.get("team_template", {}),
            "workflow_template": manifest.get("workflow_template", {}),
            "ui": manifest.get("ui", {}),
        },
        "materialized": {
            "workflow_request": _materialize_workflow_request(manifest, inputs),
        },
        "capabilities_checked": bool(capabilities),
    }
