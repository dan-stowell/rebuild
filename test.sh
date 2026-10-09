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
cat c/libc.c c/cc.c | $RUN out/cc.wasm > out/t7 || true
check out/t7 out/cc.wasm "C compiler compiles itself to a fixed point"
cat c/libc.c c/cc.c | ./run.sh out/cc.wasm > out/t8 || true
check out/t8 out/cc.wasm "runtime runs the C compiler on itself"
cat c/libc.c c/tests/basic.c | $RUN out/cc.wasm > out/basic.wasm
$RUN out/basic.wasm > out/t9 || true
check out/t9 c/tests/basic.out "C test program"
for t in fnptr; do
  cat c/libc.c c/tests/$t.c | $RUN out/cc.wasm > out/$t.wasm
  ./run.sh out/$t.wasm < /dev/null > out/t10 || true
  check out/t10 c/tests/$t.out "C test $t (on our runtime)"
done
for t in lua/tests/*.lua; do
  lua/run.sh $t > out/t11 2>&1 || true
  check out/t11 ${t%.lua}.out "Lua test $(basename $t)"
done
if [ "$1" = "--slow" ]; then
  { wc -c < out/compiler.wasm; cat out/compiler.wasm stage2/runtime.sx; } |
    ./run.sh out/runtime.wasm > out/t6 || true
  check out/t6 out/runtime.wasm "runtime in runtime runs compiler on runtime.sx"
fi
exit $fail
