# cstar development/build toolchain.
#
# Portable between podman and docker (both read `Dockerfile` by default).
# Based on the official Emscripten SDK image, which already provides emcc, node,
# python and the wasm clang. On top we add the host toolchain needed to build
# the cstar compiler itself: a modern bison (the grammar uses %code/api.pure
# full, requiring bison >= 2.7; Debian ships 3.x), flex, make, and clang.
#
# The image is toolchain-only: the cstar source tree is bind-mounted at /work
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
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
