#!/bin/sh
# usage: lua/run.sh prog.lua [args to the host]   -- compile and run a Lua program
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
RUN=${RUN:-wasmtime}
T=${TMPDIR:-/tmp}/luarun.$$
$RUN "$D/out/luac.wasm" < "$1" > $T.c
cat "$D/c/libc.c" "$D/lua/lrt.c" $T.c | $RUN "$D/out/cc.wasm" > $T.wasm
rm -f $T.c
$RUN $T.wasm; s=$?
rm -f $T.wasm
exit $s
