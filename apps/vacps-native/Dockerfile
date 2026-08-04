# VACPS Native Agent build image (x86_64 musl, Clang 22).
# Build with host proxy (recommended):
#   docker build \
#     --build-arg HTTP_PROXY="$http_proxy" \
#     --build-arg HTTPS_PROXY="$https_proxy" \
#     --build-arg NO_PROXY="$no_proxy" \
#     --build-arg http_proxy="$http_proxy" \
#     --build-arg https_proxy="$https_proxy" \
#     --build-arg no_proxy="$no_proxy" \
#     -t vacps-native-build -f apps/vacps-native/Dockerfile apps/vacps-native
#
# Or: bash apps/vacps-native/docker/build.sh  (forwards proxy automatically)
FROM alpine:3.24.1

# Proxy for apk / curl / git during image build and later container runs.
# Values come from `docker build --build-arg` (empty if not passed).
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ARG http_proxy
ARG https_proxy
ARG no_proxy

ENV HTTP_PROXY=${HTTP_PROXY} \
    HTTPS_PROXY=${HTTPS_PROXY} \
    NO_PROXY=${NO_PROXY} \
    http_proxy=${http_proxy} \
    https_proxy=${https_proxy} \
    no_proxy=${no_proxy}

# g++ / gcc: provides crtbeginS.o + libgcc that Clang's driver expects on Alpine
# (clang alone cannot link without these runtime objects).
# compiler-rt: Clang sanitizer runtimes (ASan/UBSan/TSan/LSan) under musl triple;
# without it -fsanitize=* fails to link in this image.
RUN apk add --no-cache \
    bash \
    ca-certificates \
    clang22 \
    lld22 \
    llvm22 \
    compiler-rt \
    cmake \
    ninja-build \
    samurai \
    make \
    binutils \
    g++ \
    perl \
    git \
    curl \
    pkgconf \
    linux-headers \
    musl-dev \
    libstdc++-dev \
    libgcc-static \
    openssl-dev \
    openssl-libs-static \
    liburing-dev

ENV CC=clang-22
ENV CXX=clang++-22
ENV AR=llvm-ar
ENV RANLIB=llvm-ranlib
ENV NM=llvm-nm
ENV STRIP=llvm-strip
# Prefer lld; also set via CMakePresets so try_compile picks it up.
ENV LDFLAGS="-fuse-ld=lld"

WORKDIR /workspace
