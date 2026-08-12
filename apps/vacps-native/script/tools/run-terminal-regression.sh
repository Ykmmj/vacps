#!/usr/bin/env bash
set -euo pipefail

SCRIPT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NATIVE_ROOT="$(cd "$SCRIPT_ROOT/.." && pwd)"
BIN="${VACPS_NATIVE_BIN:-$NATIVE_ROOT/build/release/vacps-agent-linux-x86_64}"
TEST_SCRIPT="$SCRIPT_ROOT/dist/terminal-interaction-regression.mjs"

for program in /usr/bin/vim /usr/bin/less /usr/bin/python3 /bin/bash; do
  if [[ ! -x "$program" ]]; then
    echo "terminal regression requires executable $program" >&2
    exit 1
  fi
done
if [[ ! -x "$BIN" ]]; then
  echo "terminal regression binary is missing or not executable: $BIN" >&2
  echo "build it with: CMAKE_BUILD_PARALLEL_LEVEL=4 bash apps/vacps-native/docker/build.sh release --native-only" >&2
  exit 1
fi
if [[ ! -f "$TEST_SCRIPT" ]]; then
  echo "terminal regression bundle is missing: $TEST_SCRIPT" >&2
  exit 1
fi

RUN_ROOT="$(mktemp -d /tmp/vacps-terminal-regression.XXXXXX)"
LOG="$RUN_ROOT/agent.log"
cleanup() {
  rm -rf -- "$RUN_ROOT"
}
trap cleanup EXIT

VACPS_ALLOW_INSECURE_NO_AUTH=1 \
  "$BIN" \
  --script "$TEST_SCRIPT" \
  --data-dir "$RUN_ROOT/data" \
  >"$LOG" 2>&1 &
agent_pid=$!

# The product agent is intentionally long-lived. Poll only for the regression
# completion marker, then stop exactly the process started above.
for ((attempt = 0; attempt < 150; attempt += 1)); do
  if grep -q 'terminal_interaction_regression: 6/6 passed' "$LOG"; then
    break
  fi
  if ! kill -0 "$agent_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if kill -0 "$agent_pid" 2>/dev/null; then
  kill -INT "$agent_pid"
  for ((attempt = 0; attempt < 50; attempt += 1)); do
    if ! kill -0 "$agent_pid" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
fi
if kill -0 "$agent_pid" 2>/dev/null; then
  kill -KILL "$agent_pid"
fi
set +e
wait "$agent_pid"
rc=$?
set -e

cat "$LOG"
if ! grep -q 'terminal_interaction_regression: 6/6 passed' "$LOG"; then
  echo "terminal interaction regression did not complete successfully (agent rc=$rc)" >&2
  exit 1
fi

echo "terminal interaction regression ok"
