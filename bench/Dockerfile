# bench/Dockerfile — full multi-language benchmark toolchain.
#
# Separate from the dev image (../Dockerfile): much heavier (~3-4 GB) because it
# installs .NET, Rust, Go, and a JDK on top of the cstar build toolchain. Based on the
# Emscripten SDK image (multi-arch: builds on Apple-Silicon podman) which already
# provides node + clang + python3 + emcc.
#
# Build:  bench/run build-image
#
# NOTE: use a multi-arch tag. `:latest` publishes arm64 so it runs NATIVELY on
# Apple-Silicon podman — pinned tags like :3.1.74 are amd64-only and would run
# under slow qemu emulation (which crashes rustc-LTO and `dotnet publish`).
FROM emscripten/emsdk:latest

ARG DEBIAN_FRONTEND=noninteractive
ARG TARGETARCH

# cstar build deps + native toolchains + Lua + JDK + measurement tools.
# OpenJDK apt packages are arch-aware, so no TARGETARCH handling is needed. Java 21 is the
# LTS bundled by current Minecraft (Java Edition, 1.20.5+) — the standard HotSpot JVM we
# benchmark. If the base distro's repos ever lack openjdk-21, fall back to default-jdk-headless.
RUN apt-get update && apt-get install -y --no-install-recommends \
      bison flex make clang \
      gcc g++ \
      lua5.4 \
      openjdk-21-jdk-headless \
      time hyperfine \
      curl ca-certificates xz-utils \
 && rm -rf /var/lib/apt/lists/*

# Go (official tarball; arch-aware).
ARG GO_VERSION=1.22.5
RUN set -eux; \
    case "${TARGETARCH:-arm64}" in arm64) GOARCH=arm64;; *) GOARCH=amd64;; esac; \
    curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GOARCH}.tar.gz" | tar -C /usr/local -xz
ENV PATH="/usr/local/go/bin:${PATH}" \
    GOTOOLCHAIN=local GOCACHE=/tmp/gocache GOPATH=/tmp/go

# Rust (rustup, minimal profile).
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.79.0
ENV PATH="/opt/cargo/bin:${PATH}"

# .NET SDK (official install script; arch-aware).
RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
 && bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /opt/dotnet \
 && rm /tmp/dotnet-install.sh
ENV PATH="/opt/dotnet:${PATH}" \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1

# TypeScript (node comes from emsdk).
RUN npm i -g typescript@5.5.4

WORKDIR /work
