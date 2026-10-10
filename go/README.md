# gc — a compiler for a subset of Go, written in that subset

`gc.go` reads a program on stdin and writes a wasm module on stdout,
directly: no C, no assembler.  Put `rt.go`, the runtime, in front:

```
cat go/rt.go prog.go | wasmtime out/gc.wasm > prog.wasm
go/run.sh prog.go            # the same, and run it
```

It compiles itself to a fixed point in a few milliseconds, also on our
own interpreter.  Its first build comes from `gc0.c`, the same compiler
transcribed into cc's C, so the chain from the seed needs no Go toolchain:

```
cc.wasm ─ compiles go/gc0.c ─▶ gc0.wasm ─ compiles go/gc.go ─▶ gc1.wasm ─ compiles go/gc.go ─▶ gc.wasm
```

and `gc0` and `gc` produce the same bytes.  The programs are also valid Go:
`go run prog.go sys.go` builds them with the real toolchain (`sys.go` maps
the four system calls below onto `os`), which is how the tests' expected
output is made.

## The subset

- Types: `int` (64 bits; also `int64`, `uint`), `int32`, `byte`
  (`uint8`), `bool`, `string`, and slices of any of these, slices included.
- `package`, `import` (ignored), `func` (with several results),
  `const` (groups, `iota`, typed and untyped, strings), `var` (globals, with
  initializers, run in order before `main`).
- Statements: `:=` and `=` (also several at once: `a, b = b, a`;
  `x, ok := f()`), `op=`, `++`/`--`, `if` (with an init statement), `for`
  (all three forms, `range` over slices and ints), `switch` (with or
  without a tag; `default` anywhere), `break`, `continue`, `return`,
  blocks and scopes, `_`.
- Expressions: Go's operators and precedence, untyped integer constants,
  `s[i]` and `s[a:b]` (bounds-checked: out of range panics), `len`, `cap`,
  `append` (also `append(a, b...)`), `make`, `copy`, `[]T{...}`,
  conversions between integer types, `string(b)`, `[]byte(s)`,
  `string(r)`, string `+` and comparison, `print`, `println`, `panic`.
- I/O, the same for gc and for the real toolchain (`sys.go`):
  `readAll() []byte` (all of stdin), `writeOut(b)`, `writeErr(b)`,
  `exit(code)`.

Not (yet): structs, pointers, maps, methods, interfaces, closures and
function values, `goto`, labels, `defer`, goroutines, floats, runes beyond
`string(r)`.  Shifts are wasm's (counts mod 64).  Memory is never freed.

## How it works

The source is tokenized up front (with Go's semicolon rule).  A first pass
records every function's signature and the constants, then the globals;
then each function body is parsed into a tree, typed as it is built, and
turned into wasm.  A string is two wasm values (pointer, length), a slice
three (pointer, length, capacity), so they live in locals and pass through
calls and multi-value returns like any other value.  `rt.go` is the
runtime: the allocator, string operations and WASI I/O, in the same Go
plus a few intrinsics (`__load32`, `__memcopy`, ...) for raw memory.

`gc.go` is deliberately C-shaped -- slices of ints instead of structs --
so that `gc0.c` could be a line-by-line transcription.
