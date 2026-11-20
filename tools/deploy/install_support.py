#!/usr/bin/env python3
"""Install only the independent Hubstack support service and its exact ingress."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def atomic_private(path, data):
    temporary = path.with_name(path.name + ".next-" + secrets.token_hex(6))
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as file:
        file.write(data)
        file.flush()
        os.fsync(file.fileno())
    temporary.replace(path)


def link(path, destination):
    temporary = path.with_name(path.name + ".next-" + secrets.token_hex(6))
    temporary.symlink_to(destination, target_is_directory=True)
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--publish-ingress", action="store_true")
    args = parser.parse_args()
    files = [p for p in sorted((ROOT / "support_service").rglob("*"))
             if p.is_file() and "__pycache__" not in p.parts and "tests" not in p.parts]
    deployment_files = [ROOT / "tools/deploy/install_support.py", ROOT / "deploy/arch/hubstack-support.service",
                        ROOT / "deploy/arch/traefik-support.yml"]
    relative = [str(p.relative_to(ROOT)) for p in files + deployment_files]
    subprocess.run(["git", "ls-files", "--error-unmatch", "--", *relative], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    if subprocess.run(["git", "diff", "--quiet", "HEAD", "--", *relative], cwd=ROOT).returncode:
        raise SystemExit("Commit the support source before installing its reproducible release")
    if args.config.stat().st_mode & 0o077 or args.config.stat().st_uid != os.getuid():
        raise SystemExit("Private config must belong to the installing user and have mode 0600")
    config = json.loads(args.config.read_text())
    if config["public_url"] != "https://support.hubstack.cn" or config["listen"] != "10.147.16.2:21960":
        raise SystemExit("This installer is scoped to support.hubstack.cn on the designated Arch port")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    build = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()[:12]
    data = Path.home() / ".local/share/hubstack-support"
    state = data / "state"
    state.mkdir(parents=True, mode=0o700, exist_ok=True)
    private = Path.home() / ".config/hubstack-support"
    private.mkdir(parents=True, mode=0o700, exist_ok=True)
    release = data / "releases" / (revision[:12] + "-" + build)
    if not release.exists():
        release.mkdir(parents=True, mode=0o700)
        for file in files:
            target = release / file.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, target)
        (release / "manifest.json").write_text(json.dumps({"source_revision": revision, "artifacts": hashes}, indent=2) + "\n")
    if (data / "current").is_symlink():
        link(data / "previous", (data / "current").resolve())
    runtime = private / "runtime.json"
    if runtime.exists():
        atomic_private(private / "runtime.previous.json", runtime.read_text())
    config["source_revision"] = revision
    atomic_private(runtime, json.dumps(config, indent=2) + "\n")
    link(data / "current", release)
    units = Path.home() / ".config/systemd/user"
    units.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "deploy/arch/hubstack-support.service", units / "hubstack-support.service")
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "--user", "enable", "hubstack-support.service"], check=True)
    subprocess.run(["systemctl", "--user", "restart", "hubstack-support.service"], check=True)
    # Avoid owner proxy settings for the dedicated local readiness probe.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for attempt in range(60):
        try:
            request = urllib.request.Request("http://10.147.16.2:21960/readyz", headers={"Host": "support.hubstack.cn"})
            with opener.open(request, timeout=1) as response:
                ready = json.load(response)
            if ready["revision"] == revision:
                break
        except (OSError, ValueError):
            pass
        time.sleep(0.5)
    else:
        raise SystemExit("Support service did not become ready; inspect only its user journal")
    if args.publish_ingress:
        remote = "/tmp/hubstack-support-" + secrets.token_hex(8) + ".yml"
        subprocess.run(["scp", "-q", str(ROOT / "deploy/arch/traefik-support.yml"), "root@hubstack.cn:" + remote], check=True)
        subprocess.run(["ssh", "-o", "BatchMode=yes", "root@hubstack.cn",
            "install -m 0644 " + shlex.quote(remote) + " /etc/traefik/hubstack-support.yml && rm " + shlex.quote(remote)], check=True)
    print(json.dumps({"revision": revision, "release": str(release), "url": config["public_url"],
                      "runtime_host": "Arch", "cloud_runtime_installed": False}))


if __name__ == "__main__":
    main()
