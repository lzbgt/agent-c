#!/usr/bin/env python3
"""Build docs/HANDBOOK.md from the curated overview and a source index."""
from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
OVERVIEW = ROOT / "docs/handbook/OVERVIEW.md"
OUTPUT = ROOT / "docs/HANDBOOK.md"
SOURCES = [
    ROOT / "README.md",
    ROOT / "DESIGN.md",
    ROOT / "docs/AGENTD_LIB.md",
    ROOT / "docs/BROKER.md",
    ROOT / "docs/CLIENT.md",
    ROOT / "docs/DB.md",
    ROOT / "docs/DEPLOYMENT.md",
    ROOT / "docs/DIAGNOSTICS.md",
    ROOT / "docs/DOD_ACK.md",
    ROOT / "docs/EDGE_INTEROP.md",
    ROOT / "docs/EMBEDDED_C_API.md",
    ROOT / "docs/ESP32S3_AGENT_CORE_MATURITY.md",
    ROOT / "docs/LIMITS.md",
    ROOT / "docs/MACOS_PACKAGING.md",
    ROOT / "docs/MEMORY.md",
    ROOT / "docs/OREN_LANG_ECOSYSTEM.md",
    ROOT / "docs/PLATFORM_SUPPORT.md",
    ROOT / "docs/PROTOCOL.md",
    ROOT / "docs/STREAMING.md",
    ROOT / "docs/TOOLS.md",
    ROOT / "docs/VENDORED.md",
    ROOT / "docs/WORKFLOWS.md",
    ROOT / "docs/openapi/README.md",
    ROOT / "docs/spec/README.md",
]


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise SystemExit(f"Missing source doc: {path}")


def _render() -> str:
    parts: list[str] = []
    parts.append("<!-- GENERATED FILE. DO NOT EDIT. -->\n")
    parts.append("<!-- Edit docs/handbook/OVERVIEW.md and source docs; run tools/build_handbook_bundle.py. -->\n\n")

    overview = _read_text(OVERVIEW).rstrip()
    parts.append(overview)

    parts.append("\n\n---\n\n# Source Index\n\n")
    parts.append("The handbook is a curated summary. For full detail, refer to the source docs below.\n\n")
    for path in SOURCES:
        parts.append(f"- `{path.relative_to(ROOT)}`\n")

    parts.append("\n")
    return "".join(parts)


def main() -> int:
    check = False
    args = sys.argv[1:]
    if args:
        if args == ["--check"]:
            check = True
        else:
            raise SystemExit("Usage: tools/build_handbook_bundle.py [--check]")

    rendered = _render()
    if check:
        existing = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if existing != rendered:
            sys.stderr.write("docs/HANDBOOK.md is out of date. Run tools/build_handbook_bundle.py.\\n")
            return 1
        return 0

    OUTPUT.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
