#!/bin/sh
# Build the chain, then check it: every stage reproduces itself, also when
# run by our own interpreter (out/wasm.wasm), and the test programs work.
#   ./test.sh [--slow]     (--slow: also the interpreter inside itself)
set -e
cd "$(dirname "$0")"
RUN=${RUN:-wasmtime}; export RUN
./build.sh > /dev/null
fail=0
check() { if cmp -s "$1" "$2"; then echo "ok   $3"; else echo "FAIL $3"; fail=1; fi; }
w() { ./run.sh "$@"; }   # run a module on our interpreter
set +e

$RUN seed/seed.wasm < seed/seed.hex > out/t; check out/t seed/seed.wasm "seed assembles itself"
w seed/seed.wasm < seed/seed.hex > out/t;   check out/t seed/seed.wasm "  ... on our interpreter"
w seed/seed.wasm < stage1/compiler.hex > out/t; check out/t out/compiler.wasm "seed assembles the sx compiler (interpreted)"
w out/compiler.wasm < c/cc.sx > out/t;      check out/t out/cc0.wasm "sx compiler compiles cc.sx (interpreted)"
cat c/libc.c c/cc.c | $RUN out/cc.wasm > out/t; check out/t out/cc.wasm "C compiler: fixed point"
cat c/libc.c c/cc.c | w out/cc.wasm > out/t;    check out/t out/cc.wasm "  ... on our interpreter"
cat c/libc.c c/wasm.c | w out/cc.wasm > out/t;  check out/t out/wasm.wasm "interpreter compiles itself (interpreted)"
cat c/libc.c rt/num.c lua/luac.c | w out/cc.wasm > out/t; check out/t out/luac.wasm "luac (interpreted)"
$RUN out/compiler.wasm < tests/hello.sx > out/hello.wasm
w out/hello.wasm < /dev/null > out/t;       check out/t tests/hello.out "sx test program (interpreted)"
for t in basic fnptr switch types tail; do
  cat c/libc.c c/tests/$t.c | $RUN out/cc.wasm > out/$t.wasm
  $RUN out/$t.wasm > out/t;                 check out/t c/tests/$t.out "C test $t"
  w out/$t.wasm < /dev/null > out/t;        check out/t c/tests/$t.out "  ... on our interpreter"
done
for t in lua/tests/*.lua; do
  lua/run.sh $t > out/t 2>&1;               check out/t ${t%.lua}.out "Lua test $(basename $t)"
  w out/luac.wasm < $t > out/t.c
  cat c/libc.c rt/num.c lua/lrt.c out/t.c | w out/cc.wasm > out/t.wasm
  w out/t.wasm < /dev/null > out/t 2>&1;    check out/t ${t%.lua}.out "  ... entirely on our interpreter"
done
cat c/libc.c rt/num.c rt/rt.c scheme/read.c scheme/scc.c | w out/cc.wasm > out/t; check out/t out/scc.wasm "scc (interpreted)"
SRT="c/libc.c rt/num.c rt/rt.c scheme/read.c scheme/srt.c"
for t in scheme/tests/*.scm; do
  scheme/run.sh $t > out/t 2>&1;            check out/t ${t%.scm}.out "Scheme test $(basename $t)"
  w out/scc.wasm < $t > out/t.c
  cat $SRT out/t.c | w out/cc.wasm > out/t.wasm
  w out/t.wasm < /dev/null > out/t 2>&1;    check out/t ${t%.scm}.out "  ... entirely on our interpreter"
done
check out/gc1.wasm out/gc.wasm "bootstrap Go compiler (gc0, in C) and gc agree"
cat go/rt.go go/gc.go | $RUN out/gc.wasm > out/t; check out/t out/gc.wasm "Go compiler: fixed point"
cat go/rt.go go/gc.go | w out/gc.wasm > out/t;    check out/t out/gc.wasm "  ... on our interpreter"
for t in go/tests/*.go; do
  go/run.sh $t > out/t 2>&1;                check out/t ${t%.go}.out "Go test $(basename $t)"
  cat go/rt.go $t | w out/gc.wasm > out/t.wasm
  w out/t.wasm < /dev/null > out/t 2>&1;    check out/t ${t%.go}.out "  ... entirely on our interpreter"
done
if [ "$1" = "--slow" ]; then
  { wc -c < out/cc.wasm; cat out/cc.wasm c/libc.c c/cc.c; } | w out/wasm.wasm > out/t
  check out/t out/cc.wasm "interpreter in interpreter runs the C compiler on itself"
fi
exit $fail
