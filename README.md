# rebuild

Tools that target only WebAssembly, grown from a tiny hand-written seed.

The bet: one target (wasm), one usage pattern (invoke, run, throw away),
and ahead-of-time compilation all the way down.  Those sharp edges keep the
tools small enough to read, and quick to build from nothing: the whole
chain below builds from the seed in well under a second.

```
seed/seed.wasm   353 bytes: a hex assembler, hand-written as raw wasm
   │  assembles stage1/compiler.hex
   ▼
compiler.wasm    3 KB: a compiler for "sx", a tiny s-expression language
   │  compiles c/cc.sx
   ▼
cc0.wasm         11 KB: a bootstrap C compiler (cc.c transcribed into sx)
   │  compiles c/cc.c, which then compiles itself to a fixed point
   ▼
cc.wasm          29 KB: a compiler for a subset of C          (c/README.md)
   ├─ compiles c/wasm.c  →  wasm.wasm   27 KB: a WebAssembly interpreter
   ├─ compiles go/gc0.c  →  gc0.wasm    46 KB: a bootstrap Go compiler
   │     compiles go/gc.go, which then compiles itself to a fixed point
   │     ▼
   │  gc.wasm         80 KB: a compiler for a subset of Go, in Go (go/README.md)
   ├─ compiles scheme/scc.c → scc.wasm  71 KB: Scheme → C, ahead of time (scheme/)
   └─ compiles lua/luac.c → luac.wasm   34 KB: Lua → C, ahead of time (lua/README.md)
```

The only binary to trust, besides the host that runs the first step, is
`seed/seed.wasm`: it is exactly the bytes written out in `seed/seed.hex`,
and assembling `seed.hex` with it reproduces it.

The interpreter is a check that the chain is complete, not the way to run
things fast: it runs every stage, including itself, and the results must
be byte-identical.  Production runs use a real engine (wasmtime, a
browser), which compiles our naive wasm well.

## Running it

Needs a WASI runtime for the first step (default `wasmtime`; set `RUN=`).

```
./build.sh                   # seed -> ... -> out/cc.wasm, gc.wasm, wasm.wasm, ...
./test.sh [--slow]           # every stage reproduces itself, also interpreted
go/run.sh prog.go            # compile a Go program and run it
scheme/run.sh prog.scm       # compile a Scheme program ahead of time and run it
lua/run.sh prog.lua          # compile a Lua program ahead of time and run it
lua/corpus.sh                # the Lua corpus: 14/14 match Lua 5.4's output
./run.sh guest.wasm < input  # run a module on our interpreter
```

`run.sh` feeds the interpreter the guest module (its length, a newline, the
bytes) and then the guest's stdin.

## The pieces

- `seed/seed.hex` — hex pairs become bytes, `;` starts a comment, and
  `{ ... }` prefixes a region with its length (a 5-byte LEB128).
- `stage1/compiler.hex` — the sx compiler, as annotated raw wasm.  sx: every
  value is an i32; functions, globals, constants, `if`, `while`, `set`,
  calls, strings, and wasm's i32 operators by short names.
- `c/cc.sx` — the C compiler transcribed into sx, only to compile `cc.c`.
- `c/cc.c` — one pass, no AST, emitting wasm while parsing.  `char`, `int`,
  `long`, `double`, `unsigned`, pointers, `switch`, function pointers.
  `cc.c` uses only int, char and pointers itself, so that `cc.sx` can
  compile it.  `c/libc.c` is the whole library: WASI I/O and a bump
  allocator that never frees.
- `c/wasm.c` — the interpreter: wasm 1.0 without f32, translated first so
  branches need no label stack.
- `go/` — Go: `gc.go` compiles a Go subset straight to wasm, and itself;
  `gc0.c` is its transcription into C, for the first build.
- `scheme/` — Scheme (R7RS's core) compiled ahead of time to C, on `rt/`,
  a runtime meant to be shared by the dynamically typed languages.
- `lua/` — Lua 5.4 compiled ahead of time to C.

`tools/` and `c/native.c` are dev-only helpers; nothing in the chain
uses them.
