#!/bin/sh
# The bootstrap chain.  Needs only a WASI host runtime (default: wasmtime).
set -e
cd "$(dirname "$0")"
RUN=${RUN:-wasmtime}
mkdir -p out
$RUN seed/seed.wasm      < stage1/compiler.hex > out/compiler.wasm  # hex -> sx compiler
$RUN out/compiler.wasm   < stage2/runtime.sx   > out/runtime.wasm   # sx -> wasm interpreter
$RUN out/compiler.wasm   < c/cc.sx             > out/cc0.wasm       # sx -> bootstrap C compiler
cat c/libc.c c/cc.c | $RUN out/cc0.wasm        > out/cc1.wasm       # C compiler, built by cc0
cat c/libc.c c/cc.c | $RUN out/cc1.wasm        > out/cc.wasm        # ... and by itself
ls -l out/*.wasm
