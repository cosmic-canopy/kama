# kama development/build toolchain.
#
# Portable between podman and docker (both read `Dockerfile` by default).
# Based on the official Emscripten SDK image, which already provides emcc, node,
# python and the wasm clang. On top we add the host toolchain needed to build
# the kama compiler itself: a modern bison (the grammar uses %code/api.pure
# full, requiring bison >= 2.7; Debian ships 3.x), flex, make, and clang.
#
# The image is toolchain-only: the kama source tree is bind-mounted at /work
# (see tools/cdev), so day-to-day edits don't require rebuilding the image.
#
# NOTE: pin this tag once verified against a known-good emcc (the WebGPU port
# flag has changed across emscripten releases).
FROM emscripten/emsdk:latest

# `apt-get upgrade` first: the emsdk base is rebuilt only with an emscripten release, so its packages carry whatever
# was current then. CVE-2026-111497 (the Brotli decoder) was fixed in Ubuntu's libbrotli1 1.1.0-2ubuntu0.1 weeks after
# the base was built; a rebuild of this image now takes such fixes.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get upgrade -y --no-install-recommends \
 && apt-get install -y --no-install-recommends \
      bison \
      flex \
      make \
      clang \
      libclang-rt-18-dev \
      openssh-client \
      diffutils \
      git \
 && rm -rf /var/lib/apt/lists/*
# openssh-client provides `ssh-keygen -Y sign/verify` (SSHSIG) for `kama publish --key` + `--verify`
# (M3.2a package signing). Absent it, signing/verification skip gracefully.
# diffutils + git are what the GUARDS compare and resolve with, and tools/run-checks.sh refuses to start
# without them. Both almost certainly come with the emsdk base already — listed anyway, because "the base
# image probably has it" is precisely the assumption that left the Windows leg reporting a clean tree as
# "the binary's AGENTS.md differs from agents/AGENTS.md" for months. Naming them costs nothing and makes
# a future base-image change say so instead of misreporting it as a content failure.
# libclang-rt-18-dev ships the compiler-rt runtime (libclang_rt.asan/ubsan.*) that the bare
# `clang` metapackage omits on arm64 — without it `-fsanitize=address,undefined` fails to LINK.
# It tracks the clang version above (18 here); bump the suffix if the base image's clang moves.

# Node 24 LTS, ahead of the emsdk's own Node on PATH and as emcc's (EM_NODE_JS): the emsdk pins its own Node (22.16, then
# 24.19.0 in the base of 2026-10-31), older than the release that fixed CVE-2026-111497, the Brotli decoder (24.22.1),
# and CI's wasm job sets up the same Node 24. Checked against the release's SHASUMS256.txt. Bump NODE_VERSION with the
# LTS line's security releases.
ARG NODE_VERSION=24.22.2
RUN arch=$(dpkg --print-architecture) && case "$arch" in amd64) n=x64 ;; arm64) n=arm64 ;; *) echo "no Node for $arch" >&2; exit 1 ;; esac \
 && f="node-v${NODE_VERSION}-linux-${n}.tar.xz" \
 && curl -fsSLO "https://nodejs.org/dist/v${NODE_VERSION}/${f}" \
 && curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/SHASUMS256.txt" | grep " ${f}\$" | sha256sum -c - \
 && mkdir -p /opt/node && tar -xJf "$f" -C /opt/node --strip-components=1 && rm "$f"
ENV PATH=/opt/node/bin:$PATH
ENV EM_NODE_JS=/opt/node/bin/node

# Browser test harness for the wasm web transports (WebTransport / WebRTC / WebSocket-in-browser E2E, which
# node can't run). Chromium + Playwright live in the IMAGE, not the repo, so the tree stays small. NODE_PATH
# lets a repo script `require('playwright')`; PLAYWRIGHT_BROWSERS_PATH pins the browser to a stable location.
# aioquic provides a dependency-free HTTP/3 server for the WebTransport echo test.
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers
ENV NODE_PATH=/opt/pw/node_modules
RUN mkdir -p /opt/pw && cd /opt/pw && npm init -y >/dev/null 2>&1 \
 && npm install --no-fund --no-audit playwright@latest \
 && npx --yes playwright install --with-deps chromium \
 && pip3 install --no-cache-dir --break-system-packages aioquic \
 && rm -rf /var/lib/apt/lists/*

# Hang diagnostics for the test watchdog. `run_tests.sh` kills a fixture that overruns and prints what it
# can see: process state, wchan, syscall, open fds, per-thread state, and node's own report. Two of the
# three hang shapes write NO node report — node produces one on the MAIN THREAD, so a thread spinning or
# parked in a synchronous syscall is invisible to it, and that is exactly what a filesystem fixture under
# emscripten NODEFS does. `eu-stack -p <pid>` is the only thing that names the frame in that case.
#
# elfutils, not gdb: it is a few MB against gdb's hundred-plus, and `eu-stack` is the whole of what the
# watchdog wants. Verified before adding that the container can already ptrace a sibling process
# (`ptrace_scope` is 0 and the container runs as uid 0), so this needs no --cap-add and no seccomp change.
#
# LAST layer on purpose: everything above it — the apt toolchain, and especially the Chromium/Playwright
# install — is expensive and cached, and putting a new package in the first RUN would rebuild all of it.
RUN apt-get update \
 && apt-get install -y --no-install-recommends elfutils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
