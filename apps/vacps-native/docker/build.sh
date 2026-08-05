#!/usr/bin/env bash
# Build vacps-agent-linux-x86_64 inside the Alpine/Clang Docker image.
# Forwards host http(s)_proxy into docker run (and image build when needed).
#
# From monorepo root:
#   bash apps/vacps-native/docker/build.sh                 # reuse existing image
#   bash apps/vacps-native/docker/build.sh debug
#   bash apps/vacps-native/docker/build.sh asan            # ASan+UBSan Debug
#   bash apps/vacps-native/docker/build.sh tsan            # TSan Debug
#   bash apps/vacps-native/docker/build.sh release --rebuild-image
#   bash apps/vacps-native/docker/build.sh release --native-only
#   VACPS_NATIVE_REBUILD_IMAGE=1 bash apps/vacps-native/docker/build.sh
#
# --native-only: compile-only product path (vacps-agent). Reuses the existing
# image (does not imply --rebuild-image), skips host TypeScript/npm build+test,
# builds only target vacps-agent, keeps binary checks, and intentionally skips
# the full business JS lifecycle
# smoke (compile-only mode: no script bundle/test or agent lifecycle run).
#
# asan/tsan modes use dedicated build dirs (build/asan-ubsan, build/tsan),
# dynamic Debug linking, and Docker --security-opt seccomp=unconfined so
# io_uring syscalls and TSan ASLR disable are not blocked by the default
# seccomp profile. release/debug Docker security is unchanged.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
IMAGE="${VACPS_NATIVE_IMAGE:-vacps-native-build:alpine-3.24.1-clang22}"
PRESET="release"
REBUILD_IMAGE="${VACPS_NATIVE_REBUILD_IMAGE:-0}"
NATIVE_ONLY=0

USAGE="usage: $0 [release|debug|asan|tsan] [--rebuild-image] [--native-only]"

for arg in "$@"; do
  case "$arg" in
    release|linux-x86_64-musl-release) PRESET=release ;;
    debug|linux-x86_64-musl-debug) PRESET=debug ;;
    asan) PRESET=asan ;;
    tsan) PRESET=tsan ;;
    --rebuild-image) REBUILD_IMAGE=1 ;;
    --native-only) NATIVE_ONLY=1 ;;
    -h|--help)
      echo "$USAGE" >&2
      exit 0
      ;;
    *)
      echo "unknown arg: $arg" >&2
      echo "$USAGE" >&2
      exit 2
      ;;
  esac
done

case "$PRESET" in
  release)
    PRESET_NAME=linux-x86_64-musl-release
    BUILD_PRESET=release
    BUILD_DIR=release
    ;;
  debug)
    PRESET_NAME=linux-x86_64-musl-debug
    BUILD_PRESET=debug
    BUILD_DIR=debug
    ;;
  asan)
    PRESET_NAME=linux-x86_64-musl-asan-ubsan
    BUILD_PRESET=asan-ubsan
    BUILD_DIR=asan-ubsan
    ;;
  tsan)
    PRESET_NAME=linux-x86_64-musl-tsan
    BUILD_PRESET=tsan
    BUILD_DIR=tsan
    ;;
esac

# Collect proxy-related env from host (both upper/lower case forms).
proxy_build_args=()
proxy_run_env=()
for v in HTTP_PROXY HTTPS_PROXY NO_PROXY ALL_PROXY \
         http_proxy https_proxy no_proxy all_proxy; do
  val="${!v-}"
  if [[ -n "${val}" ]]; then
    proxy_build_args+=(--build-arg "${v}=${val}")
    proxy_run_env+=(-e "${v}=${val}")
  fi
done

if ((${#proxy_build_args[@]} > 0)); then
  echo "==> using host proxy for docker run/build"
else
  echo "==> warning: no http_proxy/https_proxy in environment" >&2
fi

# Prefer sg docker when group is assigned but session not refreshed.
run_docker() {
  if docker info >/dev/null 2>&1; then
    docker "$@"
  elif command -v sg >/dev/null 2>&1 && getent group docker | grep -q "\b${USER}\b"; then
    # shellcheck disable=SC2048,SC2086
    sg docker -c "docker $(printf '%q ' "$@")"
  else
    docker "$@"
  fi
}

image_exists() {
  run_docker image inspect "$IMAGE" >/dev/null 2>&1
}

if [[ "$REBUILD_IMAGE" == "1" ]] || ! image_exists; then
  if [[ "$REBUILD_IMAGE" == "1" ]]; then
    echo "==> rebuilding image $IMAGE (--rebuild-image)"
  else
    echo "==> image $IMAGE not found; building once"
  fi
  run_docker build \
    "${proxy_build_args[@]+"${proxy_build_args[@]}"}" \
    -t "$IMAGE" \
    -t vacps-native-build:latest \
    -f "$ROOT/apps/vacps-native/Dockerfile" \
    "$ROOT/apps/vacps-native"
else
  echo "==> reusing existing image $IMAGE (pass --rebuild-image to rebuild)"
fi

# Business script (TypeScript) builds on the host — Alpine image has no Node.
# typecheck path-maps @vacps/contracts → packages/contracts/src, so contracts must
# have its deps (zod) installed (pnpm workspace or local npm in packages/contracts).
# Skipped in --native-only: compile product binary only, no JS bundle pipeline.
if [[ "$NATIVE_ONLY" != "1" && -f "$ROOT/apps/vacps-native/script/package.json" ]]; then
  echo "==> ensure monorepo contracts deps for script typecheck/esbuild"
  if command -v pnpm >/dev/null 2>&1 && [[ -f "$ROOT/pnpm-lock.yaml" ]]; then
    if [[ ! -d "$ROOT/node_modules" ]]; then
      (cd "$ROOT" && pnpm install --frozen-lockfile)
    fi
    (cd "$ROOT" && pnpm --filter @vacps/contracts build) || true
  elif [[ -f "$ROOT/packages/contracts/package.json" ]]; then
    (cd "$ROOT/packages/contracts" && npm install --omit=dev --no-fund --no-audit)
  fi

  echo "==> npm run build + test (script/)"
  if [[ -f "$ROOT/apps/vacps-native/script/package-lock.json" ]]; then
    (cd "$ROOT/apps/vacps-native/script" && npm ci && npm run build && npm test)
  else
    (cd "$ROOT/apps/vacps-native/script" && npm install && npm run build && npm test)
  fi
fi

if [[ "$NATIVE_ONLY" == "1" ]]; then
  echo "==> cmake --preset $PRESET_NAME && build vacps-agent"
else
  echo "==> cmake --preset $PRESET_NAME && build"
fi

# Sanitizer containers need seccomp unconfined:
# - ASan+UBSan: default profile blocks io_uring_setup/enter used by vacps:fs
# - TSan: needs to disable ASLR; default profile blocks that path too
docker_run_extra=()
case "$PRESET" in
  asan)
    echo "==> asan mode: docker --security-opt seccomp=unconfined (io_uring syscalls)"
    docker_run_extra+=(--security-opt seccomp=unconfined)
    ;;
  tsan)
    echo "==> tsan mode: docker --security-opt seccomp=unconfined (io_uring + TSan ASLR)"
    docker_run_extra+=(--security-opt seccomp=unconfined)
    ;;
esac

# Container body: default parallel 2, hard max 4 (never expand host nproc).
run_docker run --rm \
  "${proxy_run_env[@]+"${proxy_run_env[@]}"}" \
  "${docker_run_extra[@]+"${docker_run_extra[@]}"}" \
  -v "$ROOT:/workspace" \
  -w /workspace/apps/vacps-native \
  "$IMAGE" \
  bash -lc "
    set -euo pipefail
    export http_proxy=\"\${http_proxy:-\${HTTP_PROXY:-}}\"
    export https_proxy=\"\${https_proxy:-\${HTTPS_PROXY:-}}\"
    export no_proxy=\"\${no_proxy:-\${NO_PROXY:-}}\"
    echo \"proxy in container: http_proxy=\${http_proxy:-<empty>}\"
    # Cover direct binary smoke (--version / lifecycle).
    case '$PRESET' in
      asan)
        export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
        export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
        ;;
      tsan)
        export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
        ;;
    esac
    cmake --preset $PRESET_NAME
    # Cap parallelism: high -j can OOM/freeze WSL hosts. Default 2, hard max 4.
    _j=\"\${CMAKE_BUILD_PARALLEL_LEVEL:-2}\"
    case \"\$_j\" in
      ''|*[!0-9]*) _j=2 ;;
    esac
    if [ \"\$_j\" -lt 1 ]; then _j=2; fi
    if [ \"\$_j\" -gt 4 ]; then _j=4; fi
    export CMAKE_BUILD_PARALLEL_LEVEL=\"\$_j\"
    if [[ '$NATIVE_ONLY' == '1' ]]; then
      cmake --build --preset $BUILD_PRESET --parallel \"\$CMAKE_BUILD_PARALLEL_LEVEL\" --target vacps-agent
    else
      cmake --build --preset $BUILD_PRESET --parallel \"\$CMAKE_BUILD_PARALLEL_LEVEL\"
    fi
    BIN=build/${BUILD_DIR}/vacps-agent-linux-x86_64
    ls -la \"\$BIN\"
    if command -v file >/dev/null 2>&1; then
      file \"\$BIN\"
    else
      # Alpine build image has no file(1); fall back to readelf if present.
      (readelf -h \"\$BIN\" 2>/dev/null | head -20) || true
    fi
    if [[ '$BUILD_PRESET' == release ]]; then
      (ldd \"\$BIN\" 2>&1 || true) | head -20
    fi
    \"\$BIN\" --version
    if [[ '$NATIVE_ONLY' == '1' ]]; then
      echo '==> lifecycle smoke intentionally skipped in --native-only mode (not a full validation)'
    elif [[ -f script/dist/vacps.mjs ]]; then
      echo '==> script load smoke'
      rm -f /tmp/vacps-script-run.log
      smoke_timeout_seconds=3
      if [[ '$PRESET' == asan || '$PRESET' == tsan ]]; then
        smoke_timeout_seconds=15
      fi
      set +e
      # Smoke has no CP key; production installs must set CONTROL_PLANE_PUBLIC_KEY.
      VACPS_ALLOW_INSECURE_NO_AUTH=1 VACPS_LISTEN_PORT=18793 \
        timeout -s INT \"\$smoke_timeout_seconds\" \"\$BIN\" \
        --script script/dist/vacps.mjs --data-dir /tmp/vacps-script-smoke \
        >/tmp/vacps-script-run.log 2>&1
      rc=\$?
      set -e
      # 124 = timeout (expected: agent keeps running); 0 = clean exit; others = crash
      if [[ \$rc -ne 0 && \$rc -ne 124 ]]; then
        echo \"script agent crashed rc=\$rc:\" >&2
        cat /tmp/vacps-script-run.log >&2
        exit 1
      fi
      if ! grep -q 'business script ready' /tmp/vacps-script-run.log; then
        echo 'script load failed:' >&2
        cat /tmp/vacps-script-run.log >&2
        exit 1
      fi
      if ! grep -q 'application initialize host=' /tmp/vacps-script-run.log; then
        echo 'initialize() did not log:' >&2
        cat /tmp/vacps-script-run.log >&2
        exit 1
      fi
      echo 'script load ok'
    fi
  "

echo "==> done"
