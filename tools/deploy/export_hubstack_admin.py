#!/usr/bin/env python3
"""Export private owner proofs on the identity host from a verified Panda role.

The caller must obtain --panda-owner-account from the existing Panda administrator
record, not an email claim. This reads platform metadata and creates no membership.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import urllib.parse


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service-config", type=Path, required=True)
    parser.add_argument("--panda-owner-account", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"usr_[a-f0-9]{32}", args.panda_owner_account):
        raise SystemExit("A verified product account ID is required")
    service = json.loads(args.service_config.read_text())
    dsn = service["operations_database_url"]
    database = urllib.parse.urlsplit(dsn).path.lstrip("/") if "://" in dsn else dict(word.split("=", 1) for word in shlex.split(dsn) if "=" in word).get("dbname", "")
    # Never let a complete DSN (which may contain credentials) reach argv or an
    # exception representation. Local container access needs only the DB name.
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", database):
        raise SystemExit("Cannot identify the platform database safely")
    container = "hubstack-payments-payments-postgres-1"
    inspected = json.loads(subprocess.check_output(["docker", "inspect", container]))[0]
    env = dict(value.split("=", 1) for value in inspected["Config"]["Env"] if "=" in value)
    base = ["docker", "exec", container, "psql", "-U", env.get("POSTGRES_USER", "postgres"), "-d", database, "-Atc"]
    owner = args.panda_owner_account
    query = "SELECT json_build_object('proofs',(SELECT json_agg(json_build_object('provider',b.provider,'namespace',b.namespace,'subject',b.subject)) FROM identity_bindings b WHERE b.account_id=m.account_id),'agentc_account',(SELECT subject FROM identity_memberships WHERE account_id=m.account_id AND app_id='agent-c')) FROM identity_memberships m JOIN identity_accounts a ON a.id=m.account_id WHERE m.app_id='panda' AND m.subject='" + owner + "' AND a.status='active'"
    result = json.loads(subprocess.check_output(base + [query], text=True))
    if not result["proofs"]:
        raise SystemExit("The verified owner has no active provider bindings")
    data = {"admin_email": "lzbgt@icloud.com", "admin_identities": result["proofs"], "admin_account_ids": [result["agentc_account"]] if result["agentc_account"] else []}
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as file:
        json.dump(data, file)
    print(json.dumps({"verified_provider_proofs": len(data["admin_identities"]), "existing_agentc_membership": bool(data["admin_account_ids"])}))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        raise SystemExit("Administrator proof export failed: " + type(error).__name__)
