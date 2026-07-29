# vacps-native

Native VACPS Agent (C++23), **Linux x86_64 musl static ELF**.

Design: `temp/native.md` (repo-local). Sibling of Node agent `apps/vacps`.

## Build (required: Docker)

```bash
# from monorepo root
bash apps/vacps-native/docker/build.sh          # Release static
bash apps/vacps-native/docker/build.sh debug    # Debug (dynamic OK)
```

Image base: `alpine:3.24.1` with Clang 22 + lld 22 (see `Dockerfile`).

Artifact:

```text
apps/vacps-native/build/release/vacps-agent-linux-x86_64
```

Smoke:

```bash
./apps/vacps-native/build/release/vacps-agent-linux-x86_64 --version
file ./apps/vacps-native/build/release/vacps-agent-linux-x86_64
```

## Status (scaffold)

| Piece | Status |
|-------|--------|
| Docker toolchain | done |
| CMake presets | done |
| `--version` binary | done |
| Boost.Asio / Beast HTTP | not yet |
| QuickJS / SQLite / OpenSSL | not yet |
| Control-plane protocol | not yet |

## Layout

```text
apps/vacps-native/
├── Dockerfile
├── CMakeLists.txt
├── CMakePresets.json
├── docker/build.sh
├── src/
└── README.md
```
