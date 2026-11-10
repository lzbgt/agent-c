#!/usr/bin/env python3
"""Qualify the public Arch deployment; private credentials never enter output."""
import argparse
from datetime import datetime, timezone
import http.cookiejar
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path.home() / ".config/agent-c/gateway.json")
    parser.add_argument("--output", type=Path, default=ROOT / "build/arch_live_verification.json")
    parser.add_argument("--live-model", action="store_true")
    args = parser.parse_args()
    cfg = json.loads(args.config.read_text())
    origin = cfg["public_url"]
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect(), urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def call(path, payload=None, authenticated=False):
        headers = {"Content-Type": "application/json", "Origin": origin}
        if authenticated:
            headers["Authorization"] = "Bearer " + cfg["operator_token"]
        request = urllib.request.Request(origin + path, data=None if payload is None else json.dumps(payload).encode(), headers=headers)
        try:
            with opener.open(request, timeout=40) as response:
                return response.status, dict(response.headers), response.read(8 * 1024 * 1024)
        except urllib.error.HTTPError as error:
            return error.code, dict(error.headers), error.read(8192)

    status, _, body = call("/healthz")
    health = json.loads(body)
    assert status == 200 and health["source_revision"] == cfg["source_revision"]
    assert call("/api/v1/config")[0] == 401
    assert call("/broker/v1/agents")[0] == 401
    assert json.loads(call("/auth/session")[2])["authenticated"] is False
    runtime = call("/agentui-config.js")[2].decode()
    assert "identityEnabled:true" in runtime
    assert all(secret not in runtime for secret in (cfg["operator_token"], cfg["daemon_token"], cfg["identity_credential"]))
    providers = json.loads(call("/auth/providers")[2])
    redirects = {}
    for provider, expected in (("google", "accounts.google.com"), ("github", "github.com"), ("apple", "appleid.apple.com")):
        assert providers[provider] is True
        status, headers, _ = call("/auth/" + provider + "/start")
        assert status == 303
        location = headers.get("Location") or headers.get("location")
        assert urllib.parse.urlsplit(location).hostname == "payments.hubstack.cn"
        try:
            opener.open(location, timeout=20)
        except urllib.error.HTTPError as error:
            assert error.code in (302, 303)
            target = error.headers["Location"]
            assert urllib.parse.urlsplit(target).hostname == expected
            redirects[provider] = {"shared_broker": True, "provider_host": expected}
        else:
            raise AssertionError("Shared sign-in did not redirect to the provider")
    assert call("/api/v1/health", authenticated=True)[0] == 200
    payload = json.loads((ROOT / "docs/examples/workflows/sop_inspection.json").read_text())
    payload["trace_id"] = "arch-deployment-qualification"
    payload["idempotency_key"] = "arch-qualification-" + cfg["source_revision"][:12]
    status, _, body = call("/api/v1/workflow/submit", payload, True)
    result = json.loads(body)
    assert status == 200 and result["ok"]
    workflow_id = result["workflow_id"]
    for _ in range(100):
        _, _, body = call("/api/v1/workflow?workflow_id=" + urllib.parse.quote(workflow_id) + "&include_tasks=1&include_spec=1", authenticated=True)
        detail = json.loads(body)
        if detail["workflow"]["status"] in ("done", "error", "cancelled"):
            break
        time.sleep(0.1)
    assert detail["workflow"]["status"] == "done" and len(detail["tasks"]) == 4
    assert detail["spec"]["sop_compiled"]["definition_hash"].startswith("sha256:")
    model = {"executed": False}
    if args.live_model:
        status, _, body = call("/api/v1/run", {"prompt": "Return exactly AGENT_C_ARCH_READY", "tools": "none", "model": "deepseek-v4-pro", "no_session": True, "timeout_ms": 25000, "max_steps": 1}, True)
        response = json.loads(body)
        assert status == 200 and response.get("ok") and response.get("assistant_text", "").strip() == "AGENT_C_ARCH_READY", "Live provider qualification failed; inspect private daemon diagnostics"
        model = {"executed": True, "model": "deepseek-v4-pro", "expected_reply": True, "tools": "none"}
    report = {
        "checked_at": datetime.now(timezone.utc).isoformat(), "url": origin,
        "source_revision": health["source_revision"], "https_health": True,
        "anonymous_execution_denied": True, "browser_runtime_contains_no_credentials": True,
        "shared_provider_redirects": redirects, "live_user_callback_verified": False,
        "administrator_authority": "Pinned verified provider proofs and optional Agent-C product account IDs; Panda product IDs and email are not authorization inputs",
        "sop_workflow": {"workflow_id": workflow_id, "status": "done", "tasks": 4}, "live_model": model,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
