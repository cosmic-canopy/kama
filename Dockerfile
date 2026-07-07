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

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bison \
      flex \
      make \
      clang \
      libclang-rt-18-dev \
 && rm -rf /var/lib/apt/lists/*
# libclang-rt-18-dev ships the compiler-rt runtime (libclang_rt.asan/ubsan.*) that the bare
# `clang` metapackage omits on arm64 — without it `-fsanitize=address,undefined` fails to LINK.
# It tracks the clang version above (18 here); bump the suffix if the base image's clang moves.

WORKDIR /work
