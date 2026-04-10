#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional


@dataclass(frozen=True)
class NativeDep:
    key: str
    role: str
    pkg_config: Optional[str] = None
    brew_formula: Optional[str] = None


DEPS: List[NativeDep] = [
    NativeDep("opus", "audio codec", pkg_config="opus", brew_formula="opus"),
    NativeDep("portaudio", "audio I/O", pkg_config="portaudio-2.0", brew_formula="portaudio"),
    NativeDep("srtp", "SRTP transport security", pkg_config="srtp", brew_formula="srtp"),
    NativeDep("libusrsctp", "data channel / SCTP", pkg_config="usrsctp", brew_formula="libusrsctp"),
    NativeDep("libjuice", "ICE connectivity", pkg_config="libjuice", brew_formula="libjuice"),
    NativeDep("libdatachannel", "higher-level WebRTC stack", pkg_config="libdatachannel", brew_formula="libdatachannel"),
]


def run(cmd: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=False)


def pkg_config_status(dep: NativeDep) -> Dict[str, object]:
    if not dep.pkg_config:
        return {"installed": False, "reason": "no pkg-config name configured"}
    if shutil.which("pkg-config") is None:
        return {"installed": False, "reason": "pkg-config not found"}
    probe = run(["pkg-config", "--modversion", dep.pkg_config])
    if probe.returncode == 0:
        return {
            "installed": True,
            "version": probe.stdout.strip(),
            "pkg_config_name": dep.pkg_config,
        }
    reason = probe.stderr.strip() or probe.stdout.strip() or "package not found"
    return {
        "installed": False,
        "pkg_config_name": dep.pkg_config,
        "reason": reason,
    }


def brew_status(dep: NativeDep) -> Dict[str, object]:
    if not dep.brew_formula:
        return {"available": False, "reason": "no Homebrew formula configured"}
    if shutil.which("brew") is None:
        return {"available": False, "reason": "brew not found"}
    probe = run(["brew", "info", "--json=v2", dep.brew_formula])
    if probe.returncode != 0:
        return {
            "available": False,
            "formula": dep.brew_formula,
            "reason": probe.stderr.strip() or probe.stdout.strip() or "formula lookup failed",
        }
    try:
        payload = json.loads(probe.stdout)
    except json.JSONDecodeError as exc:
        return {
            "available": False,
            "formula": dep.brew_formula,
            "reason": f"invalid brew JSON: {exc}",
        }
    formulas = payload.get("formulae") or []
    if not formulas:
        return {
            "available": False,
            "formula": dep.brew_formula,
            "reason": "formula not returned by brew info",
        }
    formula = formulas[0]
    versions = formula.get("versions") or {}
    installed = formula.get("installed") or []
    return {
        "available": True,
        "formula": formula.get("name") or dep.brew_formula,
        "desc": formula.get("desc") or "",
        "homepage": formula.get("homepage") or "",
        "version": versions.get("stable") or "",
        "installed": bool(installed),
        "installed_versions": [entry.get("version") for entry in installed if entry.get("version")],
    }


def readiness_summary(report: Dict[str, Dict[str, object]]) -> Dict[str, object]:
    installed = {key for key, item in report.items() if (item.get("pkg_config") or {}).get("installed")}
    audio_primitives_ready = {"opus", "portaudio"}.issubset(installed)
    transport_primitives_ready = {"srtp", "libusrsctp", "libjuice"}.issubset(installed)
    embedded_webrtc_candidate_ready = audio_primitives_ready and transport_primitives_ready
    missing_pkg_config = [
        key for key, item in report.items() if not (item.get("pkg_config") or {}).get("installed")
    ]
    available_via_brew = [
        key for key, item in report.items()
        if not (item.get("pkg_config") or {}).get("installed")
        and (item.get("brew") or {}).get("available")
    ]
    unavailable_via_brew = [
        key for key, item in report.items()
        if not (item.get("pkg_config") or {}).get("installed")
        and not (item.get("brew") or {}).get("available")
    ]
    return {
        "audio_primitives_ready": audio_primitives_ready,
        "transport_primitives_ready": transport_primitives_ready,
        "embedded_webrtc_candidate_ready": embedded_webrtc_candidate_ready,
        "missing_pkg_config": missing_pkg_config,
        "missing_but_available_via_brew": available_via_brew,
        "missing_and_not_available_via_brew": unavailable_via_brew,
    }


def inspect_stack() -> Dict[str, object]:
    report: Dict[str, Dict[str, object]] = {}
    for dep in DEPS:
        report[dep.key] = {
            "role": dep.role,
            "pkg_config": pkg_config_status(dep),
            "brew": brew_status(dep),
        }
    return {
        "ok": True,
        "tool": "check_voice_native_media_stack.py",
        "deps": report,
        "readiness": readiness_summary(report),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect the local native voice media stack prerequisites for agentd."
    )
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output")
    args = parser.parse_args()

    report = inspect_stack()
    json.dump(report, sys.stdout, indent=2 if args.pretty else None, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
