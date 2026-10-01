#!/bin/sh
# Build the chain, then check that the runtime reproduces every stage.
set -e
cd "$(dirname "$0")"
RUN=${RUN:-wasmtime}; export RUN
./build.sh > /dev/null
check() { if cmp -s "$1" "$2"; then echo "ok   $3"; else echo "FAIL $3"; fail=1; fi; }
fail=0
$RUN seed/seed.wasm < seed/seed.hex > out/t1 || true
check out/t1 seed/seed.wasm "seed assembles itself"
$RUN out/compiler.wasm < tests/hello.sx > out/hello.wasm
./run.sh out/hello.wasm < /dev/null > out/t2 || true
check out/t2 tests/hello.out "runtime runs hello"
./run.sh seed/seed.wasm < seed/seed.hex > out/t3 || true
check out/t3 seed/seed.wasm "runtime runs seed on seed.hex"
./run.sh seed/seed.wasm < stage1/compiler.hex > out/t4 || true
check out/t4 out/compiler.wasm "runtime runs seed on compiler.hex"
./run.sh out/compiler.wasm < stage2/runtime.sx > out/t5 || true
check out/t5 out/runtime.wasm "runtime runs compiler on runtime.sx"
if [ "$1" = "--slow" ]; then
  { wc -c < out/compiler.wasm; cat out/compiler.wasm stage2/runtime.sx; } |
    ./run.sh out/runtime.wasm > out/t6 || true
  check out/t6 out/runtime.wasm "runtime in runtime runs compiler on runtime.sx"
fi
exit $fail
