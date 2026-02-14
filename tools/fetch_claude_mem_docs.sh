#!/usr/bin/env bash
set -euo pipefail

root="${1:-ref/claude-mem}"
base="https://raw.githubusercontent.com/thedotmack/claude-mem/main"
docs_base="${base}/docs/public"

mkdir -p "${root}/docs/public"

curl -fsSL "${base}/README.md" -o "${root}/README.md"
curl -fsSL "${docs_base}/docs.json" -o "${root}/docs/public/docs.json"

pages=(
  "introduction"
  "usage/search-tools"
  "usage/private-tags"
  "usage/folder-context"
  "progressive-disclosure"
  "context-engineering"
  "beta-features"
  "endless-mode"
  "hooks-architecture"
  "architecture/overview"
  "architecture/database"
  "architecture/search-architecture"
)

for page in "${pages[@]}"; do
  out="${root}/docs/public/${page}.mdx"
  mkdir -p "$(dirname "${out}")"
  curl -fsSL "${docs_base}/${page}.mdx" -o "${out}"
done
