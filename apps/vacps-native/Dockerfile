# VACPS Native Agent build image (x86_64 musl, Clang 22).
# Build: docker build -t vacps-native-build -f apps/vacps-native/Dockerfile apps/vacps-native
FROM alpine:3.24.1

RUN apk add --no-cache \
    bash \
    ca-certificates \
    clang22 \
    lld22 \
    llvm22 \
    cmake \
    ninja-build \
    make \
    perl \
    git \
    curl \
    pkgconf \
    linux-headers \
    musl-dev \
    libstdc++-dev \
    libgcc-static

ENV CC=clang-22
ENV CXX=clang++-22
ENV AR=llvm-ar
ENV RANLIB=llvm-ranlib
ENV NM=llvm-nm
ENV STRIP=llvm-strip

WORKDIR /workspace
