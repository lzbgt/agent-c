#!/usr/bin/env bash
set -euo pipefail

if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go not found" >&2
  exit 77
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${ROOT}/build"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/broker_audio_signal_loopback_smoke.log"

cd "${ROOT}/broker"

go test ./internal/broker -run TestAudioSessionSignalLoopback -count=1 >"${LOG_FILE}" 2>&1

echo "broker_audio_signal_loopback_smoke OK"
