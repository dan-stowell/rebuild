#!/bin/sh
# usage: scheme/run.sh prog.scm   -- compile and run a Scheme program
#   scc.wasm: Scheme -> C;  cc.wasm: C + the runtime -> wasm;  then run it
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
RUN=${RUN:-wasmtime}
T=${TMPDIR:-/tmp}/scmrun.$$
$RUN "$D/out/scc.wasm" < "$1" > $T.c
cat "$D/c/libc.c" "$D/rt/num.c" "$D/rt/rt.c" "$D/scheme/read.c" "$D/scheme/srt.c" $T.c | $RUN "$D/out/cc.wasm" > $T.wasm
rm -f $T.c
shift
$RUN $T.wasm "$@"; s=$?
rm -f $T.wasm
exit $s
