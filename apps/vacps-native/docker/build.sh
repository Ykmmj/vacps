#!/usr/bin/env bash
# Build vacps-agent-linux-x86_64 inside the Alpine/Clang Docker image.
# Run from monorepo root:
#   bash apps/vacps-native/docker/build.sh
#   bash apps/vacps-native/docker/build.sh debug
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IMAGE="${VACPS_NATIVE_IMAGE:-vacps-native-build}"
PRESET="${1:-release}"

case "$PRESET" in
  release|linux-x86_64-musl-release) PRESET_NAME=linux-x86_64-musl-release; BUILD_PRESET=release ;;
  debug|linux-x86_64-musl-debug) PRESET_NAME=linux-x86_64-musl-debug; BUILD_PRESET=debug ;;
  *)
    echo "usage: $0 [release|debug]" >&2
    exit 2
    ;;
esac

echo "==> docker build $IMAGE"
docker build -t "$IMAGE" -f "$ROOT/apps/vacps-native/Dockerfile" "$ROOT/apps/vacps-native"

echo "==> cmake --preset $PRESET_NAME && build"
docker run --rm \
  -v "$ROOT:/workspace" \
  -w /workspace/apps/vacps-native \
  "$IMAGE" \
  bash -lc "
    set -euo pipefail
    cmake --preset $PRESET_NAME
    cmake --build --preset $BUILD_PRESET
    BIN=build/${BUILD_PRESET}/vacps-agent-linux-x86_64
    ls -la \"\$BIN\"
    file \"\$BIN\"
    if [[ '$BUILD_PRESET' == release ]]; then
      (ldd \"\$BIN\" 2>&1 || true) | head -20
    fi
    \"\$BIN\" --version
  "
echo "==> done"
