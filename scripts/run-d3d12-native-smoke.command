#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT_DIR"

mkdir -p test/artifacts/d3d12-native-smoke
LOG="test/artifacts/d3d12-native-smoke/gui-command.log"

{
  echo "run-d3d12-native-smoke.command"
  echo "started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "pwd=$PWD"
  echo "uid=$(id -u)"
  echo "arch=$(arch)"
  echo "launchctl_manager=$(launchctl managername 2>/dev/null || true)"
  set +e
  APITRACE_REQUIRE_D3D_NATIVE_REPLAY=1 bash scripts/test-d3d12-native-smoke.sh
  smoke_status=$?
  set -e
  echo "finished_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "exit_status=$smoke_status"
  exit "$smoke_status"
} 2>&1 | tee "$LOG"
