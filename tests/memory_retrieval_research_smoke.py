#!/usr/bin/env python3
"""Exercise the actual foreground research command without provider access."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="agent-memory-screen-") as temp:
    root = Path(temp)
    memory = root / "memory"
    memory.mkdir()
    (memory / "MEMORY.md").write_text("Cedarport supplier is JuniperWorks.\n\nJuniperWorks is based in Riverton.\n\n## Deprecated\nJuniperWorks was based in Sundale.\n")
    manifest = root / "screen.json"
    manifest.write_text(json.dumps({"schema": "agent.memory.screen.input.v1", "files": ["MEMORY.md"], "cases": [
        {"id": "current", "question": "Where is Cedarport supplier located?"},
        {"id": "history", "question": "Sundale", "view": "historical"},
    ]}))
    output = root / "results" / "screen.json"
    cmd = [binary, "--memory-root", str(memory), "--manifest", str(manifest), "--output", str(output), "--lexical-only"]
    completed = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=10)
    result = json.loads(completed.stdout)
    assert result["status"] == "complete" and len(result["arms"]) == 2
    assert result["usage"] is None and result["budget_before_dispatch"] is None
    assert result["arms"][0]["context"] and "Sundale" not in json.dumps(result["arms"][0]["context"])
    assert result["arms"][1]["context"][0]["historical"] is True
    before = output.read_bytes()
    again = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    assert again.returncode == 2 and output.read_bytes() == before and "no automatic replay" in again.stderr
    assert list((output.parent / ".revisions").rglob("*.blob")), "prepared and prior completed arms were not retained"
    manifest.write_text(json.dumps({"schema": "agent.memory.screen.input.v1", "files": ["MEMORY.md"], "cases": [
        {"id": "outside", "question": "private", "files": ["../foreign.md"]},
    ]}))
    outside = root / "outside.json"
    rejected = subprocess.run([*cmd[:-3], "--output", str(outside), "--lexical-only"], capture_output=True, text=True, timeout=10)
    assert rejected.returncode == 2
print("Research CLI: frozen sources, explicit history, durable partial receipts and no automatic replay passed")
