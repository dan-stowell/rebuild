# rebuild

Bootstrapping a WebAssembly runtime from a tiny hand-written wasm seed.

```
seed/seed.wasm   (353 bytes, hand-written hex assembler)
   │  assembles
   ▼
compiler.wasm    (3.2 KB, from stage1/compiler.hex: a compiler for "sx", a tiny s-expression language)
   │  compiles
   ▼
runtime.wasm     (7.5 KB, from stage2/runtime.sx: a WebAssembly interpreter)
   │  runs
   ▼
any (supported) .wasm program — including seed.wasm, compiler.wasm and runtime.wasm itself
```

The only binary you have to trust (besides the host runtime that runs the
first step) is `seed/seed.wasm`, which is exactly the bytes written out in
`seed/seed.hex` — and assembling `seed.hex` with `seed.wasm` reproduces
`seed.wasm`.

| stage | source | binary |
|---|---|---|
| seed (hex assembler) | `seed/seed.hex` (hand-written wasm) | **353 bytes** |
| compiler (sx → wasm) | `stage1/compiler.hex`, 593 lines of annotated hex | 3197 bytes |
| runtime (wasm interpreter) | `stage2/runtime.sx`, 580 lines of sx | 7455 bytes |

Of the seed's 353 bytes, 130 are module structure (the WASI import
section alone is 70); the code section is 223.

`tools/` holds dev-only helpers; nothing in the bootstrap chain uses them.

## Running it

Needs a WASI host runtime for the first step (default `wasmtime`; set `RUN=`
to use another).

```
./build.sh                  # seed -> out/compiler.wasm -> out/runtime.wasm
./run.sh guest.wasm < input # run a guest on out/runtime.wasm
./test.sh [--slow]          # the runtime reproduces every stage byte-for-byte
                            # (--slow: also runtime-in-runtime)
```

The runtime reads the guest module from stdin, prefixed by its length in
decimal and a newline; the rest of stdin is the guest's stdin.  `run.sh`
does that framing.

## The pieces

- `seed/seed.hex` — the seed, annotated.  Hex pairs become bytes, `;` starts
  a comment, and `{ ... }` prefixes a region with its length (a 5-byte
  LEB128), which is what makes writing wasm by hand practical.
- `stage1/compiler.hex` — compiler for sx, written as raw wasm in the
  seed's format.  sx: every value is an i32; `(fn name (params) (locals)
  body...)`, `(global name n)`, `(const name n)`, `if`, `while`, `do`, `set`,
  `return`, calls, strings, and wasm's i32 ops by short names (`+`, `<u`,
  `load8`, `store`, `memgrow`, ...).  See the header of the file.
- `stage2/runtime.sx` — the interpreter.  It supports the i32 subset of
  wasm 1.0: all control flow, calls, `call_indirect`/tables, globals,
  memory, i32 loads/stores/arithmetic, plus `memory.copy`/`memory.fill`,
  and WASI `fd_read`, `fd_write`, `proc_exit`.
