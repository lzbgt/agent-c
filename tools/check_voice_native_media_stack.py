#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence


@dataclass(frozen=True)
class NativeDep:
    key: str
    role: str
    pkg_config_names: Sequence[str] = ()
    brew_formula: Optional[str] = None
    header_names: Sequence[str] = ()
    library_names: Sequence[str] = ()


DEPS: List[NativeDep] = [
    NativeDep(
        "opus",
        "audio codec",
        pkg_config_names=("opus",),
        brew_formula="opus",
        header_names=("opus/opus.h",),
        library_names=("libopus.dylib", "libopus.a"),
    ),
    NativeDep(
        "portaudio",
        "audio I/O",
        pkg_config_names=("portaudio-2.0",),
        brew_formula="portaudio",
        header_names=("portaudio.h",),
        library_names=("libportaudio.dylib", "libportaudio.a"),
    ),
    NativeDep(
        "srtp",
        "SRTP transport security",
        pkg_config_names=("libsrtp2", "srtp"),
        brew_formula="srtp",
        header_names=("srtp2/srtp.h",),
        library_names=("libsrtp2.dylib", "libsrtp2.a"),
    ),
    NativeDep(
        "libusrsctp",
        "data channel / SCTP",
        pkg_config_names=("usrsctp",),
        brew_formula="libusrsctp",
        header_names=("usrsctp.h",),
        library_names=("libusrsctp.dylib", "libusrsctp.a"),
    ),
    NativeDep(
        "libjuice",
        "ICE connectivity",
        pkg_config_names=("libjuice",),
        brew_formula="libjuice",
        header_names=("juice/juice.h",),
        library_names=("libjuice.dylib", "libjuice.a"),
    ),
    NativeDep(
        "libdatachannel",
        "higher-level WebRTC stack",
        pkg_config_names=("libdatachannel",),
        brew_formula="libdatachannel",
        header_names=("rtc/rtc.h", "rtc/rtc.hpp"),
        library_names=("libdatachannel.dylib", "libdatachannel.a"),
    ),
]


def run(cmd: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=False)


def pkg_config_status(dep: NativeDep) -> Dict[str, object]:
    if not dep.pkg_config_names:
        return {"installed": False, "reason": "no pkg-config name configured"}
    if shutil.which("pkg-config") is None:
        return {"installed": False, "reason": "pkg-config not found"}
    reasons: List[str] = []
    for name in dep.pkg_config_names:
        probe = run(["pkg-config", "--modversion", name])
        if probe.returncode == 0:
            return {
                "installed": True,
                "version": probe.stdout.strip(),
                "pkg_config_name": name,
                "pkg_config_names": list(dep.pkg_config_names),
            }
        reason = probe.stderr.strip() or probe.stdout.strip() or "package not found"
        reasons.append(f"{name}: {reason}")
    return {
        "installed": False,
        "pkg_config_names": list(dep.pkg_config_names),
        "reason": "; ".join(reasons),
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


def brew_prefix(dep: NativeDep) -> str:
    if not dep.brew_formula or shutil.which("brew") is None:
        return ""
    probe = run(["brew", "--prefix", dep.brew_formula])
    if probe.returncode != 0:
        return ""
    return probe.stdout.strip()


def filesystem_status(dep: NativeDep) -> Dict[str, object]:
    if not dep.header_names and not dep.library_names:
      return {"installed": False, "reason": "no filesystem probe configured"}

    roots: List[str] = []
    prefix = brew_prefix(dep)
    if prefix:
        roots.extend([prefix, os.path.join(prefix, "opt", dep.brew_formula or "")])
    if dep.brew_formula:
        roots.extend(
            [
                f"/opt/homebrew/opt/{dep.brew_formula}",
                f"/usr/local/opt/{dep.brew_formula}",
            ]
        )
    roots.extend(["/opt/homebrew", "/usr/local", "/usr"])

    seen = set()
    ordered_roots = []
    for root in roots:
        root = root.strip()
        if not root or root in seen:
            continue
        seen.add(root)
        ordered_roots.append(root)

    header_path = ""
    for root in ordered_roots:
        for header_name in dep.header_names:
            candidate = os.path.join(root, "include", header_name)
            if os.path.exists(candidate):
                header_path = candidate
                break
        if header_path:
            break

    library_path = ""
    for root in ordered_roots:
        for library_name in dep.library_names:
            candidate = os.path.join(root, "lib", library_name)
            if os.path.exists(candidate):
                library_path = candidate
                break
        if library_path:
            break

    if header_path and library_path:
        return {
            "installed": True,
            "header_path": header_path,
            "library_path": library_path,
        }
    reasons = []
    if not header_path:
        reasons.append("headers not found")
    if not library_path:
        reasons.append("libraries not found")
    return {
        "installed": False,
        "reason": ", ".join(reasons) if reasons else "filesystem probe failed",
    }


def dep_installed(item: Dict[str, object]) -> bool:
    return bool((item.get("pkg_config") or {}).get("installed") or (item.get("filesystem") or {}).get("installed"))


def readiness_summary(report: Dict[str, Dict[str, object]]) -> Dict[str, object]:
    installed = {key for key, item in report.items() if dep_installed(item)}
    audio_primitives_ready = {"opus", "portaudio"}.issubset(installed)
    transport_primitives_ready = {"srtp", "libusrsctp", "libjuice"}.issubset(installed)
    embedded_webrtc_candidate_ready = audio_primitives_ready and transport_primitives_ready
    missing_pkg_config = [
        key for key, item in report.items() if not (item.get("pkg_config") or {}).get("installed")
    ]
    available_via_brew = [
        key for key, item in report.items()
        if not dep_installed(item)
        and (item.get("brew") or {}).get("available")
    ]
    unavailable_via_brew = [
        key for key, item in report.items()
        if not dep_installed(item)
        and not (item.get("brew") or {}).get("available")
    ]
    return {
        "audio_primitives_ready": audio_primitives_ready,
        "transport_primitives_ready": transport_primitives_ready,
        "embedded_webrtc_candidate_ready": embedded_webrtc_candidate_ready,
        "missing_pkg_config": missing_pkg_config,
        "missing_any_install_surface": [key for key, item in report.items() if not dep_installed(item)],
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
            "filesystem": filesystem_status(dep),
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
