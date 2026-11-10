#!/usr/bin/env python3
"""Run on the identity service host. Never print application credentials."""
import argparse
import http.cookiejar
import json
import os
from pathlib import Path
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service-config", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--public-url", default="https://agent.hubstack.cn")
    parser.add_argument("--admin-proof-file", type=Path, required=True)
    args = parser.parse_args()
    trusted = json.loads(args.admin_proof_file.read_text())
    if args.public_url != "https://agent.hubstack.cn" or trusted.get("admin_email") != "lzbgt@icloud.com" or not trusted.get("admin_identities"):
        raise SystemExit("This provisioner is scoped to the Agent-C installation")
    output = Path(args.output)
    service = json.loads(Path(args.service_config).read_text())
    origin = service["public_origin"]
    client = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    csrf = ""

    def call(method, route, data=None, credential=None):
        headers = {"Content-Type": "application/json", "Origin": origin}
        if csrf:
            headers["X-CSRF-Token"] = csrf
        if credential:
            headers["Authorization"] = "Bearer " + credential
        request = urllib.request.Request(origin + route, data=None if data is None else json.dumps(data).encode(), method=method, headers=headers)
        with client.open(request, timeout=20) as response:
            return json.loads(response.read(1048576))

    try:
        csrf = call("POST", "/admin/api/login", {"key": service["admin_key"]})["csrf_token"]
        apps = call("GET", "/admin/api/apps")["items"]
        found = any(app["id"] == "agent-c" for app in apps)
        if output.exists():
            private = json.loads(output.read_text())
            if private["application_id"] != "agent-c" or not found:
                raise RuntimeError("Existing Agent-C identity does not match the registry")
        elif found:
            raise RuntimeError("Agent-C application already exists; recover its private credential before proceeding")
        else:
            created = call("POST", "/admin/api/apps", {"id": "agent-c", "name": "Agent-C"})
            private = {"application_id": "agent-c", "credential": created["token"]}
            output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as file:
                json.dump(private, file)
        # Product account IDs are intentionally different across applications.
        # Bootstrap from verified provider proofs, never Panda's product ID.
        private.update({"admin_email": trusted["admin_email"], "admin_identities": trusted["admin_identities"], "admin_account_ids": trusted.get("admin_account_ids", [])})
        settings = call("GET", "/admin/api/apps/agent-c/identity")["client"]
        callbacks = [args.public_url + "/auth/" + provider + "/callback" for provider in ("google", "github", "apple")]
        desired = {"enabled": True, "providers": ["google", "github", "apple"], "redirect_uris": callbacks, "deletion_enabled": False, "revision": settings["revision"]}
        if not settings["enabled"] or settings["redirect_uris"] != callbacks or settings["providers"] != desired["providers"]:
            call("PUT", "/admin/api/apps/agent-c/identity", desired)
        probe = next(iter(private["admin_account_ids"]), "usr_" + "0" * 32)
        state = call("POST", "/v1/identity/account-state", {"account_id": probe}, private["credential"])
        if state["application_id"] != "agent-c":
            raise RuntimeError("Registered credential has the wrong application scope")
        private["instance_id"] = state["instance_id"]
        # The destination is private and belongs only to this newly registered app.
        fd = os.open(output, os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as file:
            json.dump(private, file)
        providers = call("GET", "/v1/identity/providers", credential=private["credential"])
        print(json.dumps({"application_id": "agent-c", "callbacks": callbacks, "providers": providers, "admin_account_pinned": True}))
    finally:
        if csrf:
            try:
                call("POST", "/admin/api/logout", {})
            except Exception:
                pass


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # HTTP/library errors can contain response details; report only category.
        raise SystemExit("Agent-C identity provisioning failed: " + type(error).__name__)
