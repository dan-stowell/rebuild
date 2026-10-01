#!/bin/sh
# Build the chain, then check that the runtime reproduces every stage.
set -e
cd "$(dirname "$0")"
RUN=${RUN:-wasmtime}; export RUN
./build.sh > /dev/null
ok() { echo "ok   $1"; }
$RUN seed/seed.wasm < seed/seed.hex | cmp - seed/seed.wasm && ok "seed assembles itself"
$RUN out/compiler.wasm < tests/hello.sx > out/hello.wasm
./run.sh out/hello.wasm < /dev/null | cmp - tests/hello.out && ok "runtime runs hello"
./run.sh seed/seed.wasm < seed/seed.hex | cmp - seed/seed.wasm && ok "runtime runs seed on seed.hex"
./run.sh seed/seed.wasm < stage1/compiler.hex | cmp - out/compiler.wasm && ok "runtime runs seed on compiler.hex"
./run.sh out/compiler.wasm < stage2/runtime.sx | cmp - out/runtime.wasm && ok "runtime runs compiler on runtime.sx"
if [ "$1" = "--slow" ]; then
  { wc -c < out/compiler.wasm; cat out/compiler.wasm stage2/runtime.sx; } |
    ./run.sh out/runtime.wasm | cmp - out/runtime.wasm && ok "runtime in runtime runs compiler on runtime.sx"
fi
