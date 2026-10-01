#!/bin/sh
# The bootstrap chain.  Needs only a WASI host runtime (default: wasmtime).
set -e
cd "$(dirname "$0")"
RUN=${RUN:-wasmtime}
mkdir -p out
$RUN seed/seed.wasm         < stage1/compiler.hex > out/compiler.wasm
[ -f stage2/runtime.sx ] && $RUN out/compiler.wasm < stage2/runtime.sx > out/runtime.wasm
ls -l out
