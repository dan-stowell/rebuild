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

All **14 / 14** of the "Are We Fast Yet?" benchmarks pass.

## What works

Lua 5.4 syntax except `goto`; locals, upvalues and closures, varargs and
multiple results, integer `for` and generic `for`, tables with an array
part, metatables with `__index`, `__newindex`, `__call`, `__tostring`, `__len`,
`__eq`, `__lt`, `__le`, `__concat` and the arithmetic metamethods; method calls on strings.  Library: `print type tostring tonumber
pairs ipairs next select rawget rawset rawequal rawlen setmetatable
getmetatable assert error pcall`, `string.{len sub upper lower rep reverse
byte char find format}`, `table.{insert remove concat sort unpack}`,
`math.{abs max min floor ceil sqrt exp log tointeger type fmod modf ult
pi huge maxinteger mininteger}`,
`io.{write read}`, `os.exit`.

## The sharp edges

- **Numbers are Lua 5.4's**: 64-bit integers and doubles.  A value is one
  64-bit word: doubles are NaN-boxed (offset by 2^48), integers within 48
  bits are stored inline and wider ones are interned boxes.  Conversions
  between doubles and decimal are exact (`num.c`), so `print` and
  `string.format` match C's and Lua's output digit for digit.  `exp` and
  `log` (and so `x ^ y` for fractional `y`) are accurate to about an ulp
  but not correctly rounded.
- **No garbage collection.**  Memory only grows.  Building a long string
  with `s = s .. x` in a loop uses quadratic memory; use `table.concat`.
- **Errors end the program** (exit status 1).  `pcall` works for calls that
  succeed but cannot catch an error, except the common probe for an
  optional module, `pcall(require, name)`.
- `load` only of a constant string, which is compiled ahead of time.
  `require` only of bundled modules.
- No `goto`, coroutines, string patterns (only plain `find`), trigonometry,
  `math.random`, `os`/`io` beyond the above, yet.
