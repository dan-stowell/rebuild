# rebuild

Bootstrapping a WebAssembly runtime from a tiny hand-written wasm seed.

```
seed/seed.wasm   (419 bytes, hand-written hex assembler)
   │  assembles
   ▼
compiler.wasm    (from stage1/compiler.hex: a compiler for a small s-expression language)
   │  compiles
   ▼
runtime.wasm     (from stage2/runtime.sx: a WebAssembly interpreter)
   │  runs
   ▼
any (supported) .wasm program — including seed.wasm, compiler.wasm and runtime.wasm itself
```

The only binary you have to trust (besides the host runtime that runs the
first step) is `seed/seed.wasm`, which is exactly the bytes written out in
`seed/seed.hex` — and assembling `seed.hex` with `seed.wasm` reproduces
`seed.wasm`.

`tools/` holds dev-only helpers; nothing in the bootstrap chain uses them.
