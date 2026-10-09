#!/bin/sh
# Run the Lua corpus: compile each program ahead of time, run it, and
# compare its output with the reference Lua's (lua/corpus/**/*.expected,
# made with --update, which needs lua5.4).
#   lua/corpus.sh [--update] [-v]
cd "$(dirname "$0")/.."
RUN=${RUN:-wasmtime}
update=0; verbose=0
for a in "$@"; do [ "$a" = --update ] && update=1; [ "$a" = -v ] && verbose=1; done
T=${TMPDIR:-/tmp}/corpus.$$
pass=0; total=0

# case NAME BUNDLE-ARGS...: the bundled program is lua/corpus/NAME's input
runcase() {
  name=$1; shift
  lua/bundle.sh "$@" > $T.lua
  exp=lua/corpus/$name.expected
  if [ $update = 1 ]; then lua5.4 $T.lua > $exp 2>/dev/null; fi
  total=$((total + 1))
  why=""
  if ! $RUN out/luac.wasm < $T.lua > $T.c 2> $T.err; then why=$(grep -m1 . $T.err)
  elif ! cat c/libc.c rt/num.c lua/lrt.c $T.c | $RUN out/cc.wasm > $T.wasm 2> $T.err; then why="cc: $(grep -m1 . $T.err)"
  else
    timeout 60 $RUN $T.wasm > $T.out 2> $T.err
    if cmp -s $T.out $exp; then pass=$((pass + 1)); echo "pass  $name"; return
    elif [ -s $T.err ]; then why=$(grep -m1 . $T.err)
    else why="wrong output"
    fi
  fi
  echo "FAIL  $name: $why"
  [ $verbose = 1 ] && [ -f $T.out ] && diff $T.out $exp | head -5
}

AW=lua/corpus/awfy
for b in bounce cd deltablue havlak json list mandelbrot nbody permute queens richards sieve storage towers; do
  printf 'print(require("%s"):inner_benchmark_loop(1))\n' $b > $T.main.lua
  runcase awfy/$b $T.main.lua $AW/benchmark.lua $AW/som.lua $AW/$b.lua
done
for f in lua/corpus/misc/*.lua; do
  [ -f "$f" ] || continue
  runcase misc/$(basename $f .lua) $f
done
rm -f $T $T.*
echo "$pass / $total pass"
