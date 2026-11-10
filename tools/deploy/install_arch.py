#!/usr/bin/env python3
"""Install one current Agent-C snapshot on the owning Arch user account."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def private_write(path, value):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as file:
        file.write(value)
    path.chmod(0o600)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--identity-file", type=Path, required=True)
    parser.add_argument("--listen", default="10.147.16.2:18784")
    parser.add_argument("--model", default="deepseek-v4-pro")
    parser.add_argument("--publish-ingress", action="store_true")
    args = parser.parse_args()
    if subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=ROOT).returncode:
        raise SystemExit("Commit the reviewed source snapshot before deployment")
    private = Path.home() / ".config" / "agent-c"
    data = Path.home() / ".local" / "share" / "agent-c"
    private.mkdir(mode=0o700, parents=True, exist_ok=True)
    private.chmod(0o700)
    identity = json.loads(args.identity_file.read_text())
    if identity["application_id"] != "agent-c" or identity["admin_email"] != "lzbgt@icloud.com" or not (identity.get("admin_account_ids") or identity.get("admin_identities")):
        raise SystemExit("A registered Agent-C credential and verified system administrator are required")
    postgres = private / "postgres.env"
    if not postgres.exists():
        private_write(postgres, "POSTGRES_USER=agentc\nPOSTGRES_DB=agentc\nPOSTGRES_PASSWORD=" + secrets.token_hex(32) + "\n")
    pg = dict(line.split("=", 1) for line in postgres.read_text().splitlines() if "=" in line)
    gateway = private / "gateway.json"
    cfg = json.loads(gateway.read_text()) if gateway.exists() else {}
    cfg.update({
        "listen": args.listen, "public_url": "https://agent.hubstack.cn",
        "application_id": identity["application_id"], "instance_id": identity["instance_id"],
        "identity_credential": identity["credential"], "admin_email": identity["admin_email"],
        "admin_account_ids": identity["admin_account_ids"],
        "admin_identities": identity.get("admin_identities", []),
        "database_url": "postgres://agentc:" + pg["POSTGRES_PASSWORD"] + "@127.0.0.1:15439/agentc?sslmode=disable",
        "daemon_url": "http://127.0.0.1:18123", "daemon_token": cfg.get("daemon_token") or secrets.token_hex(32),
        "operator_token": cfg.get("operator_token") or secrets.token_hex(32),
    })
    cfg.setdefault("voice", {"env_file": str(Path.home() / ".env"), "endpoint": "wss://dashscope.aliyuncs.com/api-ws/v1/realtime", "model": "qwen3.5-omni-flash-realtime", "voice": "Ethan"})
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    artifacts = {"bin/agentd": ROOT / "build/agentd", "bin/agentd-gateway": ROOT / "build/agentd-gateway"}
    artifacts.update({"web/" + str(file.relative_to(ROOT / "ui/dist")): file for file in sorted((ROOT / "ui/dist").rglob("*")) if file.is_file()})
    if not (ROOT / "ui/dist/index.html").exists() or not all(file.is_file() for file in artifacts.values()):
        raise SystemExit("Build agentd, agentd-gateway and the WebUI before installation")
    hashes = {name: hashlib.sha256(file.read_bytes()).hexdigest() for name, file in artifacts.items()}
    previous_manifest = data / "current/manifest.json"
    previous = json.loads(previous_manifest.read_text()) if previous_manifest.is_file() else {}
    daemon_env = "AGENTD_AUTH_TOKEN=" + cfg["daemon_token"] + "\nAGENTD_DOTENV_PATH=" + str(Path.home() / ".env") + "\nOPENAI_API_BASE=https://api.deepseek.com\nAGENT_MODEL=" + args.model + "\n"
    daemon_unit = Path.home() / ".config/systemd/user/agent-c-daemon.service"
    restart_daemon = (
        previous.get("artifacts", {}).get("bin/agentd") != hashes["bin/agentd"]
        or not (private / "daemon.env").is_file()
        or (private / "daemon.env").read_text() != daemon_env
        or not daemon_unit.is_file()
        or daemon_unit.read_bytes() != (ROOT / "deploy/arch/agent-c-daemon.service").read_bytes()
    )
    build_id = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()[:12]
    release = data / "releases" / (revision[:12] + "-" + build_id)
    if not release.exists():
        release.mkdir(parents=True, mode=0o700)
        for name, file in artifacts.items():
            destination = release / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, destination)
        (release / "manifest.json").write_text(json.dumps({"source_revision": revision, "artifacts": hashes}, indent=2) + "\n")
    cfg["source_revision"] = revision
    cfg["web_dir"] = str(data / "current/web")
    private_write(gateway, json.dumps(cfg, indent=2) + "\n")
    private_write(private / "daemon.env", daemon_env)
    for name in ("state", "workspace"):
        (data / name).mkdir(mode=0o700, parents=True, exist_ok=True)
    candidate = data / "current.next"
    candidate.unlink(missing_ok=True)
    candidate.symlink_to(release, target_is_directory=True)
    candidate.replace(data / "current")
    unit_dir = Path.home() / ".config/systemd/user"
    unit_dir.mkdir(parents=True, exist_ok=True)
    for name in ("agent-c-daemon.service", "agent-c-gateway.service"):
        shutil.copy2(ROOT / "deploy/arch" / name, unit_dir / name)
    env = {**os.environ, "AGENTC_PRIVATE_DIR": str(private)}
    subprocess.run(["docker", "compose", "-p", "agent-c", "-f", str(ROOT / "deploy/arch/compose.yml"), "up", "-d", "--wait"], env=env, check=True)
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "--user", "enable", "agent-c-daemon.service", "agent-c-gateway.service"], check=True)
    subprocess.run(["systemctl", "--user", "restart" if restart_daemon else "start", "agent-c-daemon.service"], check=True)
    subprocess.run(["systemctl", "--user", "restart", "agent-c-gateway.service"], check=True)
    if args.publish_ingress:
        subprocess.run(["scp", "-q", str(ROOT / "deploy/arch/traefik-agent-c.yml"), "root@hubstack.cn:/tmp/agent-c-traefik.yml"], check=True)
        subprocess.run(["ssh", "-o", "BatchMode=yes", "root@hubstack.cn", "install -m 0644 /tmp/agent-c-traefik.yml /etc/traefik/agent-c.yml && rm /tmp/agent-c-traefik.yml"], check=True)
    print(json.dumps({"source_revision": revision, "release": str(release), "url": cfg["public_url"], "data_host": "Arch", "daemon_restarted": restart_daemon, "cloud_runtime_installed": False}))


if __name__ == "__main__":
    main()
