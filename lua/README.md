# Lua, compiled ahead of time to wasm

```
lua/run.sh prog.lua        # luac.wasm: Lua -> C;  cc.wasm: C (+ lrt.c) -> wasm;  run
```

`luac.c` (written in the C subset, built by `cc`) turns a Lua program into
C; `lrt.c` is the runtime it links against (by concatenation).  There is no
interpreter and no garbage collector: programs are compiled ahead of time,
run once, and their memory is thrown away with them.

## The corpus

`lua/corpus.sh` compiles and runs a corpus of unmodified Lua programs and
compares their output with Lua 5.4's.  Multi-file programs are bundled
ahead of time (`lua/bundle.sh`: modules go into `package.preload`).

Currently **9 / 14** of the "Are We Fast Yet?" benchmarks pass.  All five
failures are about numbers: four need floats (`cd`, `nbody`, and `/` in
`deltablue` and `richards`), one needs integers wider than 31 bits
(`havlak`).

## What works

Lua 5.4 syntax except `goto`; locals, upvalues and closures, varargs and
multiple results, integer `for` and generic `for`, tables with an array
part, metatables with `__index`, `__newindex`, `__call`, `__tostring` and
`__len`; method calls on strings.  Library: `print type tostring tonumber
pairs ipairs next select rawget rawset rawequal rawlen setmetatable
getmetatable assert error pcall`, `string.{len sub upper lower rep reverse
byte char find format}`, `table.{insert remove concat sort unpack}`,
`math.{abs max min floor ceil tointeger type fmod maxinteger mininteger}`,
`io.{write read}`, `os.exit`.

## The sharp edges

- **Integers only, and 31-bit.**  A value is one 32-bit word; integers are
  tagged in the low bit.  No floats: `/` and `^` stop the program, float
  literals are rejected.  Overflow wraps at 31 bits, not 64.
- **No garbage collection.**  Memory only grows.  Building a long string
  with `s = s .. x` in a loop uses quadratic memory; use `table.concat`.
- **Errors end the program** (exit status 1).  `pcall` works for calls that
  succeed but cannot catch an error, except the common probe for an
  optional module, `pcall(require, name)`.
- `load` only of a constant string, which is compiled ahead of time.
  `require` only of bundled modules.
- No `goto`, coroutines, string patterns (only plain `find`), `os`/`io` beyond the above, or `__add`-style arithmetic
  metamethods yet.
