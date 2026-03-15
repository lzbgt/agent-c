#!/usr/bin/env python3
import argparse
import base64
import hashlib
import hmac
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Optional


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(2)


def load_json(path: str) -> object:
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception as exc:
        fail(f"failed to load JSON from {path}: {exc}")


def resolve_bundle(obj: object) -> dict:
    if isinstance(obj, dict):
        if obj.get("schema") == "edge_auth_cert_roots_v1":
            return obj
        if "cert_roots" in obj:
            return resolve_bundle(obj["cert_roots"])
        body = obj.get("body")
        if isinstance(body, dict) and "cert_roots" in body:
            return resolve_bundle(body["cert_roots"])
    fail("could not resolve edge_auth_cert_roots_v1 bundle")


def canonical_json_bytes(obj: dict) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def signable_bundle(bundle: dict) -> dict:
    copy = dict(bundle)
    copy.pop("attest", None)
    return copy


def verify_attestation(
    bundle: dict,
    *,
    hmac_key: Optional[str],
    agent_ed25519_tool: Optional[str],
    ed25519_pubkey_b64: Optional[str],
    require_attest: bool,
) -> dict:
    att = bundle.get("attest")
    if not isinstance(att, dict):
        if require_attest:
            fail("bundle is missing attest block")
        return {"present": False, "verified": False, "alg": None, "kid": None}

    alg = str(att.get("alg") or "")
    kid = str(att.get("kid") or "")
    sig = str(att.get("sig") or "")
    if not alg or not kid or not sig:
        fail("bundle attest block is missing alg/kid/sig")

    canon = canonical_json_bytes(signable_bundle(bundle))
    if alg == "hmac-sha256":
        if not hmac_key:
            fail("hmac attestation present but --hmac-key was not provided")
        expected = base64.b64encode(hmac.new(hmac_key.encode("utf-8"), canon, hashlib.sha256).digest()).decode("ascii")
        ok = hmac.compare_digest(sig, expected)
        if not ok:
            fail("hmac attestation verification failed")
        return {"present": True, "verified": True, "alg": alg, "kid": kid}

    if alg == "ed25519":
        tool = agent_ed25519_tool
        pubkey = str(att.get("pubkey") or ed25519_pubkey_b64 or "")
        if not tool:
            fail("ed25519 attestation present but --agent-ed25519-tool was not provided")
        if not pubkey:
            fail("ed25519 attestation present but no pubkey is available in bundle or args")
        proc = subprocess.run(
            [tool, "--verify", "--pk-b64", pubkey, "--sig-b64", sig],
            input=canon,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if proc.returncode != 0:
            stderr = proc.stderr.decode("utf-8", errors="replace").strip()
            fail(f"ed25519 attestation verification failed: {stderr or 'signature mismatch'}")
        return {"present": True, "verified": True, "alg": alg, "kid": kid}

    fail(f"unsupported attest alg: {alg}")


def collect_roots(bundle: dict) -> Dict[str, str]:
    roots = bundle.get("cert_roots_pem")
    if not isinstance(roots, dict):
        fail("bundle cert_roots_pem is missing or invalid")
    out: Dict[str, str] = {}
    for key, value in roots.items():
        if not isinstance(key, str) or not isinstance(value, str):
            fail("bundle cert_roots_pem contains a non-string entry")
        out[key] = value
    return out


def write_ca_file(bundle: dict, out_path: str) -> dict:
    roots = collect_roots(bundle)
    ordered = []
    for kid in sorted(roots):
        pem = roots[kid]
        if not pem.endswith("\n"):
            pem += "\n"
        ordered.append(pem)
    Path(out_path).write_text("".join(ordered), encoding="utf-8")
    return {
        "ca_file": out_path,
        "cert_root_count": len(roots),
        "cert_root_kids": sorted(roots.keys()),
    }


def inspect_command(args: argparse.Namespace) -> int:
    bundle = resolve_bundle(load_json(args.bundle_json))
    attestation = verify_attestation(
        bundle,
        hmac_key=args.hmac_key,
        agent_ed25519_tool=args.agent_ed25519_tool,
        ed25519_pubkey_b64=args.ed25519_pubkey_b64,
        require_attest=args.require_attest,
    )
    summary = {
        "ok": True,
        "schema": bundle.get("schema"),
        "rotation_epoch": bundle.get("rotation_epoch"),
        "updated_utc_ms": bundle.get("updated_utc_ms"),
        "cert_root_count": len(collect_roots(bundle)),
        "cert_root_kids": sorted(collect_roots(bundle).keys()),
        "attestation": attestation,
    }
    if args.ca_file:
        summary.update(write_ca_file(bundle, args.ca_file))
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


def verify_cert_command(args: argparse.Namespace) -> int:
    bundle = resolve_bundle(load_json(args.bundle_json))
    attestation = verify_attestation(
        bundle,
        hmac_key=args.hmac_key,
        agent_ed25519_tool=args.agent_ed25519_tool,
        ed25519_pubkey_b64=args.ed25519_pubkey_b64,
        require_attest=args.require_attest,
    )

    if args.ca_file:
        ca_path = args.ca_file
        temp_dir = None
    else:
        temp_dir = tempfile.TemporaryDirectory(prefix="edge-cert-roots-")
        ca_path = os.path.join(temp_dir.name, "ca_roots.pem")
    ca_meta = write_ca_file(bundle, ca_path)

    cmd = [args.openssl_bin, "verify", "-CAfile", ca_path]
    chain_temp = None
    if args.untrusted_cert:
        if temp_dir is None:
            temp_dir = tempfile.TemporaryDirectory(prefix="edge-cert-roots-")
        chain_temp = os.path.join(temp_dir.name, "untrusted_chain.pem")
        with open(chain_temp, "w", encoding="utf-8") as fh:
            for item in args.untrusted_cert:
                text = Path(item).read_text(encoding="utf-8")
                if not text.endswith("\n"):
                    text += "\n"
                fh.write(text)
        cmd.extend(["-untrusted", chain_temp])
    cmd.append(args.cert)
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    stdout = proc.stdout.decode("utf-8", errors="replace").strip()
    stderr = proc.stderr.decode("utf-8", errors="replace").strip()
    if temp_dir is not None:
        temp_dir.cleanup()

    result = {
        "ok": proc.returncode == 0,
        "cert": args.cert,
        "openssl_cmd": cmd,
        "openssl_stdout": stdout,
        "openssl_stderr": stderr,
        "bundle": {
            "schema": bundle.get("schema"),
            "rotation_epoch": bundle.get("rotation_epoch"),
            "updated_utc_ms": bundle.get("updated_utc_ms"),
            "cert_root_count": ca_meta["cert_root_count"],
            "cert_root_kids": ca_meta["cert_root_kids"],
        },
        "attestation": attestation,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if proc.returncode == 0 else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inspect and verify edge_auth_cert_roots_v1 bundles.")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_bundle_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--bundle-json", required=True, help="Path to a JSON file containing the bundle or wrapper response")
        p.add_argument("--require-attest", action="store_true", help="Fail if the bundle does not contain an attest block")
        p.add_argument("--hmac-key", help="Shared secret used for hmac-sha256 bundle attestation verification")
        p.add_argument("--agent-ed25519-tool", help="Path to the project agent_ed25519_tool for Ed25519 verification")
        p.add_argument("--ed25519-pubkey-b64", help="Override Ed25519 pubkey (base64) if the bundle does not carry one")

    inspect = sub.add_parser("inspect", help="Inspect a cert-roots bundle and optionally verify its attestation")
    add_bundle_args(inspect)
    inspect.add_argument("--ca-file", help="Optional output path for the concatenated CA PEM file")
    inspect.set_defaults(func=inspect_command)

    verify = sub.add_parser("verify-cert", help="Verify a certificate chain against the cert-roots bundle with openssl verify")
    add_bundle_args(verify)
    verify.add_argument("--cert", required=True, help="Leaf certificate PEM to verify")
    verify.add_argument("--untrusted-cert", action="append", default=[], help="Optional intermediate certificate PEM(s)")
    verify.add_argument("--ca-file", help="Optional explicit output path for the generated CA PEM bundle")
    verify.add_argument("--openssl-bin", default=os.environ.get("OPENSSL", "openssl"), help="openssl binary path")
    verify.set_defaults(func=verify_cert_command)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
