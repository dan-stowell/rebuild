#!/bin/sh
# usage: go/run.sh prog.go [more.go ...]  -- compile (with gc) and run a Go program
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
RUN=${RUN:-wasmtime}
T=${TMPDIR:-/tmp}/gorun.$$
cat "$D/go/rt.go" "$@" | $RUN "$D/out/gc.wasm" > $T.wasm
$RUN $T.wasm; s=$?
rm -f $T.wasm
exit $s
