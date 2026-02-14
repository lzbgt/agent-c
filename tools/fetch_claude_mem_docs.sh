#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/docs/references/claude-mem"

mkdir -p "${OUT_DIR}"

curl -fsSL "https://raw.githubusercontent.com/thedotmack/claude-mem/main/README.md" -o "${OUT_DIR}/README.md"
curl -fsSL "https://raw.githubusercontent.com/thedotmack/claude-mem/main/LICENSE" -o "${OUT_DIR}/LICENSE"
curl -fsSL "https://docs.claude-mem.ai/usage/search-tools" -o "${OUT_DIR}/search-tools.html"
curl -fsSL "https://docs.claude-mem.ai/progressive-disclosure" -o "${OUT_DIR}/progressive-disclosure.html"
curl -fsSL "https://docs.claude-mem.ai/usage/private-tags" -o "${OUT_DIR}/private-tags.html"

echo "Saved claude-mem references to ${OUT_DIR}"
