#!/usr/bin/env python3
"""Copy only the active LangLearn merchant credentials into an isolated config.

Run as root on the Arch host. No source configuration or payment provider is
modified. This does not register webhooks or create orders.
"""
import argparse
import json
import os
from pathlib import Path
import pwd
import secrets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--compose-file", type=Path, required=True)
    parser.add_argument("--service", default="langlearn-cloud-1")
    args = parser.parse_args()
    owner = pwd.getpwnam(args.owner)
    home = Path(owner.pw_dir)
    target_dir = home / ".config/hubstack-support"
    target = target_dir / "config.json"
    if target.exists():
        raise SystemExit("Private support config already exists; preserve it and review any rotation separately")
    service = json.loads(args.compose_file.read_text())["services"][args.service]
    env = service["environment"]
    if env["LANGLEARN_PAYPAL_MODE"] != "live" or str(env["LANGLEARN_PAYPAL_ENABLED"]).lower() != "true":
        raise SystemExit("The selected LangLearn service does not have an enabled live merchant")
    container_path = Path(env["LANGLEARN_PAYPAL_CONFIG_FILE"])
    candidates = []
    for volume in service["volumes"]:
        destination = Path(volume["target"])
        if volume.get("type") == "bind" and container_path.is_relative_to(destination):
            candidates.append((len(destination.parts), Path(volume["source"]) / container_path.relative_to(destination)))
    if not candidates:
        raise SystemExit("Cannot resolve the mounted merchant configuration")
    credential_path = max(candidates)[1]
    stat = credential_path.stat()
    if stat.st_mode & 0o077 or not 0 < stat.st_size <= 16384:
        raise SystemExit("Merchant credentials must be bounded and owner-only")
    values = {}
    for line in credential_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise SystemExit("Invalid merchant credential syntax")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key.strip()] = value
    root = Path(__file__).resolve().parents[2]
    config = json.loads((root / "support_service/config.example.json").read_text())
    config["database"] = str(home / ".local/share/hubstack-support/state/support.sqlite3")
    config["cookie_secret"] = secrets.token_hex(32)
    config["paypal"] = {"mode": "live", "client_id": values["LIVE_CLIENT_ID"],
        "secret": values["LIVE_SECRET"], "merchant_id": env["LANGLEARN_PAYPAL_MERCHANT_ID"]}
    if not all(config["paypal"].values()):
        raise SystemExit("Active merchant configuration is incomplete")
    target_dir.mkdir(parents=True, mode=0o700, exist_ok=True)
    os.chown(target_dir, owner.pw_uid, owner.pw_gid)
    target_dir.chmod(0o700)
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as file:
        os.fchown(file.fileno(), owner.pw_uid, owner.pw_gid)
        file.write(json.dumps(config, indent=2) + "\n")
        file.flush()
        os.fsync(file.fileno())
    print(json.dumps({"private_config": str(target), "mode": "live", "source_modified": False,
                      "orders_created": 0}))


if __name__ == "__main__":
    main()
