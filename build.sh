#!/bin/sh
# The bootstrap chain.  Needs only a WASI host runtime (default: wasmtime).
set -e
cd "$(dirname "$0")"
RUN=${RUN:-wasmtime}
mkdir -p out
$RUN seed/seed.wasm    < stage1/compiler.hex > out/compiler.wasm      # hex -> sx compiler
$RUN out/compiler.wasm < c/cc.sx             > out/cc0.wasm           # sx -> bootstrap C compiler
cat c/libc.c c/cc.c     | $RUN out/cc0.wasm  > out/cc1.wasm           # the C compiler, by cc0
cat c/libc.c c/cc.c     | $RUN out/cc1.wasm  > out/cc.wasm            # ... and by itself
cat c/libc.c c/wasm.c   | $RUN out/cc.wasm   > out/wasm.wasm          # the wasm interpreter
cat c/libc.c rt/num.c lua/luac.c | $RUN out/cc.wasm > out/luac.wasm  # Lua -> C compiler
cat c/libc.c rt/num.c rt/rt.c scheme/read.c scheme/scc.c | $RUN out/cc.wasm > out/scc.wasm  # Scheme -> C
cat c/libc.c go/gc0.c   | $RUN out/cc.wasm   > out/gc0.wasm           # bootstrap Go compiler (C)
cat go/rt.go go/gc.go   | $RUN out/gc0.wasm  > out/gc1.wasm           # the Go compiler, by gc0
cat go/rt.go go/gc.go   | $RUN out/gc1.wasm  > out/gc.wasm            # ... and by itself
ls -l out/*.wasm
