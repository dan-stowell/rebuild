# cc — a C subset compiler, targeting WebAssembly only

`cc.c` is written in the subset it compiles, and compiles itself to a fixed
point.  Programs are a single translation unit on stdin; put `libc.c` in
front:

```
cat c/libc.c prog.c | wasmtime out/cc.wasm > prog.wasm
```

## The subset

- Types: `char` (signed) and `unsigned char`, `int` and `unsigned` (32-bit),
  `long` and `unsigned long` (64-bit, as are `long long`), `double`, `void`,
  and pointers (32-bit).  The usual arithmetic conversions apply.  `static`,
  `const` and `signed` are accepted and ignored.
- Integer literals of any size (`1L`, `4000000000U`, ...).  No
  floating-point literals yet: write `(double)3`, or
  `__f64_from_bits(0x400921fb54442d18L)`.
- Globals, including arrays (`int a[N]`) and scalar initializers (a constant
  expression, or a string literal for a `char *`).  Locals are scalars.
- `enum { A, B = 5 }` constants; constant expressions in array sizes.
- Functions with up to 16 parameters; prototypes.
- Statements: blocks, `if`/`else`, `while`, `do`/`while`, `for`, `break`,
  `continue`, `return`, expression statements.
- Expressions: all of C's arithmetic, comparison, logical (short-circuit),
  bitwise and shift operators, `?:`, assignment and compound assignment,
  `++`/`--`, `*`, `&` (of memory, not of locals), `[]`, calls, casts,
  `sizeof(type)`, character and string literals.
- Function pointers, wasm style: a function's name used as a value is an
  `int` (its table index), and calling an `int` variable, `f(a, b)`, is an
  indirect call with int parameters and an int result.  (Not valid C.)
- String escapes: `\n \t \r \0 \xHH`, and any other character as itself.
- Builtins: `__builtin_trap()`, `__memory_size()`, `__memory_grow(n)`,
  `__heap_base()`, `__f64_bits(d)` and `__f64_from_bits(l)` (reinterpret),
  and `__builtin_{sqrt,floor,ceil,trunc,fabs}`.
- A function that is declared but never defined, and is called, becomes a
  WASI import.
- Ignored: `#` lines (so `#include` is harmless), comments.

Not (yet): structs, unions, local arrays, `&local`, `switch`, `goto`,
`float`, `short`, floating-point literals, initializer lists, the
preprocessor.

There is no garbage collector and none is planned: `malloc` bumps a pointer
and `free` does nothing, because these programs are short-lived.

## How it works

One pass, no AST.  The source is tokenized up front; the recursive-descent
parser emits wasm while it parses.  An expression leaves its value on the
wasm stack, in a wasm local, or in memory with the address on the stack, and
is loaded only once its use is known — that is all it takes to handle
assignment.  A `for` loop's step is compiled after its body by re-reading
its tokens.  Function indices and the addresses of zero-initialized globals
are written as padded LEB128s and patched at the end.  `main` is called by
a generated `_start`; a nonzero result traps.

`native.c` lets gcc build `cc.c` too — a dev convenience only.
