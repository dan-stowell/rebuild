// gc.go -- a compiler for a small subset of Go, written in that subset.
//
// Reads a program on stdin (rt.go, then the program's files, concatenated),
// writes a wasm module on stdout.  Each function body is parsed into a
// tree, typed as it is built, then turned into wasm.  See go/README.md for
// the subset.
//
// gc.go is itself C-shaped -- no structs, pointers, maps or methods, just
// slices of ints -- so that the bootstrap compiler, gc0.c, can be a
// transcription of it into cc's C.
package main

// ------------------------------------------------------------- utilities

var errline int

func die(msg string) {
	writeErr([]byte("gc: line " + itoa(errline) + ": " + msg + "\n"))
	exit(1)
}

func charstr(c int) string {
	b := make([]byte, 1)
	b[0] = byte(c)
	return string(b)
}

func itoa(n int) string {
	if n < 0 {
		return "-" + itoa(-n)
	}
	b := make([]byte, 0, 20)
	for {
		b = append(b, byte('0'+n%10))
		n = n / 10
		if n == 0 {
			break
		}
	}
	for i := 0; i < len(b)/2; i++ {
		c := b[i]
		b[i] = b[len(b)-1-i]
		b[len(b)-1-i] = c
	}
	return string(b)
}

// ----------------------------------------------------------------- lexer

const (
	T_EOF = 256 + iota
	T_NUM
	T_STR
	T_ID
	// keywords, in the order of keywords below
	T_BREAK
	T_CASE
	T_CONST
	T_CONTINUE
	T_DEFAULT
	T_ELSE
	T_FOR
	T_FUNC
	T_IF
	T_IMPORT
	T_PACKAGE
	T_RANGE
	T_RETURN
	T_SWITCH
	T_TYPE
	T_VAR
	// operators, in the order of ops2 below
	T_EQ
	T_NE
	T_LE
	T_GE
	T_ANDAND
	T_OROR
	T_INC
	T_DEC
	T_SHL
	T_SHR
	T_ANDNOT
	T_ADDA
	T_SUBA
	T_MULA
	T_DIVA
	T_MODA
	T_ANDA
	T_ORA
	T_XORA
	T_DEFINE
	T_SHLA
	T_SHRA
	T_ANDNOTA
	T_ELLIPSIS
)

var keywords = "break case const continue default else for func if import package range return switch type var "
var ops2 = "==!=<=>=&&||++--<<>>&^+=-=*=/=%=&=|=^=:="

var src []byte
var tk []int    // kind
var tv []int    // number value, or the offset of a name in src, or of a string in strs
var tl []int    // length of a name or string
var tline []int // line
var tp int      // the current token
var strs []byte // string literals, decoded

func isdigit(c int) bool  { return c >= '0' && c <= '9' }
func isletter(c int) bool { return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c == '_' }

func hexval(c int) int {
	if isdigit(c) {
		return c - '0'
	}
	if c >= 'a' && c <= 'f' {
		return c - 'a' + 10
	}
	if c >= 'A' && c <= 'F' {
		return c - 'A' + 10
	}
	return -1
}

func at(i int) int {
	if i < len(src) {
		return int(src[i])
	}
	return 0
}

func same(p int, n int, s string) bool {
	if n != len(s) {
		return false
	}
	for i := 0; i < n; i++ {
		if src[p+i] != s[i] {
			return false
		}
	}
	return true
}

func keyword(p int, n int) int {
	t := T_BREAK
	i := 0
	for i < len(keywords) {
		j := i
		for keywords[j] != ' ' {
			j++
		}
		if j-i == n {
			k := 0
			for k < n && src[p+k] == keywords[i+k] {
				k++
			}
			if k == n {
				return t
			}
		}
		i = j + 1
		t++
	}
	return T_ID
}

func addtok(kind int, v int, l int, line int) {
	tk = append(tk, kind)
	tv = append(tv, v)
	tl = append(tl, l)
	tline = append(tline, line)
}

// a newline ends a statement after these
func endsline(t int) bool {
	return t == T_ID || t == T_NUM || t == T_STR || t == T_BREAK || t == T_CONTINUE ||
		t == T_RETURN || t == T_INC || t == T_DEC || t == ')' || t == ']' || t == '}'
}

// the character at src[i] (after a backslash: an escape); sets escend
var escend int

func charat(i int) int {
	c := at(i)
	if c != '\\' {
		escend = i + 1
		return c
	}
	c = at(i + 1)
	escend = i + 2
	if c == 'n' {
		return 10
	}
	if c == 't' {
		return 9
	}
	if c == 'r' {
		return 13
	}
	if c == '0' {
		return 0
	}
	if c == 'x' {
		escend = i + 4
		return hexval(at(i+2))*16 + hexval(at(i+3))
	}
	return c
}

func lex() {
	i := 0
	line := 1
	for {
		c := at(i)
		if i >= len(src) {
			break
		}
		errline = line
		if c == '\n' {
			if len(tk) > 0 && endsline(tk[len(tk)-1]) {
				addtok(';', 0, 0, line)
			}
			line++
			i++
			continue
		}
		if c == ' ' || c == '\t' || c == '\r' {
			i++
			continue
		}
		if c == '/' && at(i+1) == '/' {
			for i < len(src) && src[i] != '\n' {
				i++
			}
			continue
		}
		if c == '/' && at(i+1) == '*' {
			i = i + 2
			for i < len(src) && !(src[i] == '*' && at(i+1) == '/') {
				if src[i] == '\n' {
					line++
				}
				i++
			}
			i = i + 2
			continue
		}
		if isdigit(c) {
			v := 0
			if c == '0' && (at(i+1) == 'x' || at(i+1) == 'X') {
				i = i + 2
				for hexval(at(i)) >= 0 || at(i) == '_' {
					if at(i) != '_' {
						v = v*16 + hexval(at(i))
					}
					i++
				}
			} else {
				for isdigit(at(i)) || at(i) == '_' {
					if at(i) != '_' {
						v = v*10 + at(i) - '0'
					}
					i++
				}
			}
			if at(i) == '.' || at(i) == 'e' {
				die("floating-point numbers are not supported")
			}
			addtok(T_NUM, v, 0, line)
			continue
		}
		if isletter(c) {
			j := i
			for isletter(at(i)) || isdigit(at(i)) {
				i++
			}
			addtok(keyword(j, i-j), j, i-j, line)
			continue
		}
		if c == '\'' {
			v := charat(i + 1)
			i = escend + 1
			addtok(T_NUM, v, 0, line)
			continue
		}
		if c == '"' {
			start := len(strs)
			i++
			for at(i) != '"' {
				if i >= len(src) || at(i) == '\n' {
					die("unterminated string")
				}
				strs = append(strs, byte(charat(i)))
				i = escend
			}
			i++
			addtok(T_STR, start, len(strs)-start, line)
			continue
		}
		if c == '`' {
			start := len(strs)
			i++
			for at(i) != '`' {
				if i >= len(src) {
					die("unterminated string")
				}
				if src[i] == '\n' {
					line++
				}
				if src[i] != '\r' {
					strs = append(strs, src[i])
				}
				i++
			}
			i++
			addtok(T_STR, start, len(strs)-start, line)
			continue
		}
		if c == '.' && at(i+1) == '.' && at(i+2) == '.' {
			addtok(T_ELLIPSIS, 0, 0, line)
			i = i + 3
			continue
		}
		if (c == '<' || c == '>' || c == '&') && at(i+2) == '=' &&
			(at(i+1) == c && c != '&' || c == '&' && at(i+1) == '^') {
			if c == '<' {
				addtok(T_SHLA, 0, 0, line)
			} else if c == '>' {
				addtok(T_SHRA, 0, 0, line)
			} else {
				addtok(T_ANDNOTA, 0, 0, line)
			}
			i = i + 3
			continue
		}
		k := 0
		for k < len(ops2) && !(int(ops2[k]) == c && int(ops2[k+1]) == at(i+1)) {
			k = k + 2
		}
		if k < len(ops2) {
			addtok(T_EQ+k/2, 0, 0, line)
			i = i + 2
			continue
		}
		addtok(c, 0, 0, line)
		i++
	}
	if len(tk) > 0 && endsline(tk[len(tk)-1]) {
		addtok(';', 0, 0, line)
	}
	addtok(T_EOF, 0, 0, line)
}

func accept(t int) bool {
	if tk[tp] == t {
		tp++
		return true
	}
	return false
}

func expect(t int) {
	errline = tline[tp]
	if !accept(t) {
		if t < 256 {
			die("syntax error: expected " + charstr(t))
		}
		die("syntax error")
	}
}

// ----------------------------------------------------------------- types

const (
	TVOID    = 0
	TINT     = 1 // int, int64
	TI32     = 2 // int32
	TBYTE    = 3 // byte, uint8
	TBOOL    = 4
	TSTR     = 5
	TUNTYPED = 6 // an untyped integer constant
	TTUPLE   = 7 // the results of a call with several
	TSLICE   = 16
)

const I32 = 0x7f
const I64 = 0x7e

var telem []int // the element type of slice type TSLICE+i

func slicetype(elem int) int {
	for i := 0; i < len(telem); i++ {
		if telem[i] == elem {
			return TSLICE + i
		}
	}
	telem = append(telem, elem)
	return TSLICE + len(telem) - 1
}
func isslice(t int) bool { return t >= TSLICE }
func elemtype(t int) int { return telem[t-TSLICE] }
func isint(t int) bool   { return t == TINT || t == TI32 || t == TBYTE || t == TUNTYPED }

// how many wasm values a value of type t takes, and their types
func words(t int) int {
	if t == TVOID {
		return 0
	}
	if t == TSTR {
		return 2
	}
	if isslice(t) {
		return 3
	}
	return 1
}
func wtype(t int) int {
	if t == TINT || t == TUNTYPED {
		return I64
	}
	return I32
}

// size in memory
func msize(t int) int {
	if t == TINT {
		return 8
	}
	if t == TI32 {
		return 4
	}
	if t == TBYTE || t == TBOOL {
		return 1
	}
	if t == TSTR {
		return 8
	}
	return 12
}

func typename(t int) string {
	if t == TINT {
		return "int"
	}
	if t == TI32 {
		return "int32"
	}
	if t == TBYTE {
		return "byte"
	}
	if t == TBOOL {
		return "bool"
	}
	if t == TSTR {
		return "string"
	}
	if t == TUNTYPED {
		return "untyped int"
	}
	if isslice(t) {
		return "[]" + typename(elemtype(t))
	}
	return "void"
}

// --------------------------------------------------------------- symbols

const (
	S_LOCAL = 1 + iota
	S_GLOBAL
	S_CONST
	S_FUNC
	S_TYPE
)

var sname []int
var slen []int
var skind []int
var stype []int
var sval []int // first wasm local; address; value; function
var nsym int

func lookup(p int, n int) int {
	for i := nsym - 1; i >= 0; i-- {
		if slen[i] == n && samename(sname[i], p, n) {
			return i
		}
	}
	return -1
}

func samename(a int, b int, n int) bool {
	for i := 0; i < n; i++ {
		if src[a+i] != src[b+i] {
			return false
		}
	}
	return true
}

func addsym(p int, n int, kind int, t int, v int) int {
	if nsym < len(sname) {
		sname[nsym] = p
		slen[nsym] = n
		skind[nsym] = kind
		stype[nsym] = t
		sval[nsym] = v
	} else {
		sname = append(sname, p)
		slen = append(slen, n)
		skind = append(skind, kind)
		stype = append(stype, t)
		sval = append(sval, v)
	}
	nsym++
	return nsym - 1
}

// the universe: names that src doesn't contain are added to it
func universe(name string, kind int, t int, v int) {
	p := len(src)
	src = append(src, []byte(name)...)
	addsym(p, len(name), kind, t, v)
}

// functions
var fname []int
var fnlen []int
var fpstart []int // parameter types, in ptypes
var fnp []int
var frstart []int // result types, in rtypes
var fnr []int
var fbody []int // the token of the body's {, or -1: an import
var fwasm []int // wasm function index
var ftype []int // wasm type index
var ptypes []int
var pnames []int // the token of each parameter's name
var rtypes []int

func functype(f int) int {
	if fnr[f] == 0 {
		return TVOID
	}
	if fnr[f] == 1 {
		return rtypes[frstart[f]]
	}
	return TTUPLE
}

func findfunc(name string) int {
	for f := 0; f < len(fname); f++ {
		if same(fname[f], fnlen[f], name) {
			return f
		}
	}
	die("the runtime lacks " + name)
	return -1
}

// ------------------------------------------------------------ the tree

const (
	E_CONST = 1 + iota // nv: value
	E_STR              // na: offset in strs, nb: length
	E_LOCAL            // na: the first wasm local
	E_GLOBAL           // na: the address
	E_BIN              // nv: operator, na, nb
	E_UN               // nv: operator, na
	E_CALL             // na: function, nb: arguments
	E_INDEX            // na[nb]
	E_SLICE            // na[nb:nc], -1 if absent
	E_LEN              // na
	E_CAP              // na
	E_APPEND           // na: slice, nb: elements, nc: 1 if spread (...)
	E_MAKE             // nt: type, na: length, nb: capacity or -1
	E_CONV             // nt: to, na
	E_COPY             // na, nb
	E_INTRIN           // nv: which, nb: arguments
	E_PRINT            // nv: 1 for println, nb: arguments
	E_PANIC            // na
	E_BLANK            // _, on the left of =
	L_LIST             // na: item, nb: next or -1
	S_BLOCK            // na: list
	S_EXPR             // na
	S_ASSIGN           // na: lhs list, nb: rhs list
	S_OPASSIGN         // nv: operator, na: lhs, nb: rhs
	S_IF               // na: cond, nb: then, nc: else or -1, nd: init or -1
	S_FOR              // na: init, nb: cond, nc: post, nd: body (-1 if absent)
	S_RANGE            // na: key local, nb: value local (-1 if absent), nc: expr, nd: body
	S_SWITCH           // na: init, nb: tag, nc: list of cases
	S_CASE             // na: list of exprs or -1 for default, nb: body list
	S_RETURN           // na: list
	S_BREAK
	S_CONTINUE
	S_ZERO // nt, na: set the local to zero
)

var nk []int
var nt []int
var na []int
var nb []int
var nc []int
var nd []int
var nv []int
var nline []int

func node(kind int, t int, a int, b int, c int, d int, v int) int {
	nk = append(nk, kind)
	nt = append(nt, t)
	na = append(na, a)
	nb = append(nb, b)
	nc = append(nc, c)
	nd = append(nd, d)
	nv = append(nv, v)
	nline = append(nline, errline)
	return len(nk) - 1
}

func cons(item int, next int) int { return node(L_LIST, 0, item, next, -1, -1, 0) }

func listlen(l int) int {
	n := 0
	for l >= 0 {
		n++
		l = nb[l]
	}
	return n
}

// a list, in order: append to the end with lastp
var listhead int
var listtail int

func liststart() { listhead = -1; listtail = -1 }
func listadd(item int) {
	c := cons(item, -1)
	if listtail < 0 {
		listhead = c
	} else {
		nb[listtail] = c
	}
	listtail = c
}

func constnode(v int, t int) int { return node(E_CONST, t, -1, -1, -1, -1, v) }

// give an untyped constant type t (or check that n has type t)
func convert(n int, t int) {
	errline = nline[n]
	if nt[n] == t {
		return
	}
	if nt[n] == TUNTYPED && isint(t) {
		v := nv[n]
		if t == TBYTE && (v < 0 || v > 255) || t == TI32 && (v < -2147483648 || v > 2147483647) {
			die("constant " + itoa(v) + " overflows " + typename(t))
		}
		nt[n] = t
		return
	}
	die("type mismatch: " + typename(nt[n]) + " used as " + typename(t))
}

// an untyped constant used where no type is implied is an int
func settle(n int) {
	if nt[n] == TUNTYPED {
		nt[n] = TINT
	}
}

// ------------------------------------------------------------ expressions

var nloc int     // wasm locals so far in this function
var ltypes []int // their types

func newlocal(t int) int {
	ltypes = append(ltypes, t)
	nloc++
	return nloc - 1
}

// a local variable of type t, named src[p..p+n)
func declare(p int, n int, t int) int {
	s := addsym(p, n, S_LOCAL, t, nloc)
	for i := 0; i < words(t); i++ {
		if i == 0 {
			newlocal(wtype(t))
		} else {
			newlocal(I32)
		}
	}
	return s
}

func parsetype() int {
	errline = tline[tp]
	if accept('[') {
		expect(']')
		return slicetype(parsetype())
	}
	if tk[tp] == T_ID {
		s := lookup(tv[tp], tl[tp])
		if s >= 0 && skind[s] == S_TYPE {
			tp++
			return stype[s]
		}
	}
	die("expected a type")
	return 0
}

func istypestart() bool {
	if tk[tp] == '[' && tk[tp+1] == ']' {
		return true
	}
	if tk[tp] == T_ID {
		s := lookup(tv[tp], tl[tp])
		return s >= 0 && skind[s] == S_TYPE
	}
	return false
}

func parseargs() int {
	liststart()
	head := -1
	tail := -1
	expect('(')
	for tk[tp] != ')' {
		e := parseexpr()
		c := cons(e, -1)
		if tail < 0 {
			head = c
		} else {
			nb[tail] = c
		}
		tail = c
		if tk[tp] == T_ELLIPSIS {
			break
		}
		if !accept(',') {
			break
		}
	}
	return head
}

func builtin(p int, n int) int {
	errline = tline[tp]
	if same(p, n, "len") || same(p, n, "cap") {
		a := parseargs()
		expect(')')
		if listlen(a) != 1 {
			die("len and cap take one argument")
		}
		x := na[a]
		if !isslice(nt[x]) && !(nt[x] == TSTR && same(p, n, "len")) {
			die("invalid argument to len or cap")
		}
		if same(p, n, "len") {
			return node(E_LEN, TINT, x, -1, -1, -1, 0)
		}
		return node(E_CAP, TINT, x, -1, -1, -1, 0)
	}
	if same(p, n, "append") {
		a := parseargs()
		spread := 0
		if accept(T_ELLIPSIS) {
			spread = 1
		}
		accept(',')
		expect(')')
		if a < 0 || !isslice(nt[na[a]]) {
			die("first argument to append must be a slice")
		}
		t := nt[na[a]]
		for l := nb[a]; l >= 0; l = nb[l] {
			if spread == 1 {
				if !(nt[na[l]] == t || nt[na[l]] == TSTR && elemtype(t) == TBYTE) {
					die("bad argument to append")
				}
			} else {
				convert(na[l], elemtype(t))
			}
		}
		return node(E_APPEND, t, na[a], nb[a], spread, -1, 0)
	}
	if same(p, n, "make") {
		expect('(')
		t := parsetype()
		if !isslice(t) {
			die("make of a non-slice")
		}
		expect(',')
		l := parseexpr()
		convert(l, TINT)
		c := -1
		if accept(',') {
			c = parseexpr()
			convert(c, TINT)
		}
		expect(')')
		return node(E_MAKE, t, l, c, -1, -1, 0)
	}
	if same(p, n, "copy") {
		a := parseargs()
		expect(')')
		if listlen(a) != 2 || !isslice(nt[na[a]]) {
			die("bad arguments to copy")
		}
		return node(E_COPY, TINT, na[a], na[nb[a]], -1, -1, 0)
	}
	if same(p, n, "print") || same(p, n, "println") {
		a := parseargs()
		expect(')')
		for l := a; l >= 0; l = nb[l] {
			settle(na[l])
		}
		v := 0
		if same(p, n, "println") {
			v = 1
		}
		return node(E_PRINT, TVOID, -1, a, -1, -1, v)
	}
	if same(p, n, "panic") {
		a := parseargs()
		expect(')')
		if listlen(a) != 1 || nt[na[a]] != TSTR {
			die("panic takes a string")
		}
		return node(E_PANIC, TVOID, na[a], -1, -1, -1, 0)
	}
	// intrinsics, for rt.go
	w := -1
	t := TI32
	if same(p, n, "__heap_base") {
		w = 1
	}
	if same(p, n, "__memory_size") {
		w = 2
	}
	if same(p, n, "__memory_grow") {
		w = 3
	}
	if same(p, n, "__load32") {
		w = 4
	}
	if same(p, n, "__store32") {
		w = 5
		t = TVOID
	}
	if same(p, n, "__memcopy") {
		w = 6
		t = TVOID
	}
	if same(p, n, "__sptr") || same(p, n, "__bptr") {
		w = 7
	}
	if same(p, n, "__mkstring") {
		w = 8
		t = TSTR
	}
	if w < 0 {
		return -1
	}
	a := parseargs()
	expect(')')
	for l := a; l >= 0; l = nb[l] {
		if w != 7 {
			convert(na[l], TI32)
		}
	}
	return node(E_INTRIN, t, -1, a, -1, -1, w)
}

func parsecall(f int) int {
	a := parseargs()
	expect(')')
	errline = tline[tp]
	if listlen(a) != fnp[f] {
		die("wrong number of arguments")
	}
	i := 0
	for l := a; l >= 0; l = nb[l] {
		convert(na[l], ptypes[fpstart[f]+i])
		i++
	}
	return node(E_CALL, functype(f), f, a, -1, -1, 0)
}

func primary() int {
	errline = tline[tp]
	t := tk[tp]
	if t == T_NUM {
		tp++
		return constnode(tv[tp-1], TUNTYPED)
	}
	if t == T_STR {
		tp++
		return node(E_STR, TSTR, tv[tp-1], tl[tp-1], -1, -1, 0)
	}
	if t == '(' {
		tp++
		e := parseexpr()
		expect(')')
		return e
	}
	if istypestart() { // a conversion
		to := parsetype()
		expect('(')
		e := parseexpr()
		expect(')')
		return conversion(to, e)
	}
	if t != T_ID {
		die("expected an expression")
	}
	p := tv[tp]
	n := tl[tp]
	tp++
	if same(p, n, "_") {
		return node(E_BLANK, TVOID, -1, -1, -1, -1, 0)
	}
	s := lookup(p, n)
	if s < 0 {
		if tk[tp] == '(' {
			b := builtin(p, n)
			if b >= 0 {
				return b
			}
		}
		die("undefined: " + string(src[p:p+n]))
	}
	k := skind[s]
	if k == S_FUNC {
		if tk[tp] != '(' {
			die("functions as values are not supported")
		}
		return parsecall(sval[s])
	}
	if k == S_CONST {
		return constnode(sval[s], stype[s])
	}
	if k == S_LOCAL {
		return node(E_LOCAL, stype[s], sval[s], -1, -1, -1, 0)
	}
	if k == S_GLOBAL {
		return node(E_GLOBAL, stype[s], sval[s], -1, -1, -1, 0)
	}
	die("unexpected type")
	return -1
}

func conversion(to int, e int) int {
	f := nt[e]
	if isint(to) && f == TUNTYPED {
		convert(e, to)
		return e
	}
	if isint(to) && isint(f) {
		if nk[e] == E_CONST {
			v := nv[e]
			if to == TBYTE {
				v = v & 255
			} else if to == TI32 {
				v = (v & 4294967295) ^ 2147483648 - 2147483648
			}
			return constnode(v, to)
		}
		return node(E_CONV, to, e, -1, -1, -1, 0)
	}
	if to == TSTR && isslice(f) && elemtype(f) == TBYTE || to == slicetype(TBYTE) && f == TSTR {
		return node(E_CONV, to, e, -1, -1, -1, 0)
	}
	if to == f {
		return e
	}
	die("cannot convert " + typename(f) + " to " + typename(to))
	return -1
}

func postfix() int {
	e := primary()
	for {
		errline = tline[tp]
		if accept('[') {
			t := nt[e]
			if !isslice(t) && t != TSTR {
				die("index of a non-slice")
			}
			var lo int
			lo = -1
			if tk[tp] != ':' {
				lo = parseexpr()
				settle(lo)
				if !isint(nt[lo]) {
					die("non-integer index")
				}
			}
			if accept(':') {
				hi := -1
				if tk[tp] != ']' {
					hi = parseexpr()
					convert(hi, TINT)
				}
				if lo >= 0 {
					convert(lo, TINT)
				}
				expect(']')
				e = node(E_SLICE, t, e, lo, hi, -1, 0)
				continue
			}
			expect(']')
			et := TBYTE
			if isslice(t) {
				et = elemtype(t)
			}
			e = node(E_INDEX, et, e, lo, -1, -1, 0)
			continue
		}
		return e
	}
}

func unary() int {
	errline = tline[tp]
	t := tk[tp]
	if t == '-' || t == '+' || t == '!' || t == '^' {
		tp++
		e := unary()
		if t == '!' {
			if nt[e] != TBOOL {
				die("! of a non-bool")
			}
		} else if !isint(nt[e]) {
			die("arithmetic on a non-integer")
		}
		if nk[e] == E_CONST {
			v := nv[e]
			if t == '-' {
				v = -v
			} else if t == '^' {
				v = ^v
			} else if t == '!' {
				v = 1 - v
			}
			if nt[e] == TBYTE {
				v = v & 255
			}
			return constnode(v, nt[e])
		}
		if t == '+' {
			return e
		}
		return node(E_UN, nt[e], e, -1, -1, -1, t)
	}
	return postfix()
}

func prec(t int) int {
	if t == '*' || t == '/' || t == '%' || t == T_SHL || t == T_SHR || t == '&' || t == T_ANDNOT {
		return 5
	}
	if t == '+' || t == '-' || t == '|' || t == '^' {
		return 4
	}
	if t == T_EQ || t == T_NE || t == '<' || t == T_LE || t == '>' || t == T_GE {
		return 3
	}
	if t == T_ANDAND {
		return 2
	}
	if t == T_OROR {
		return 1
	}
	return 0
}

func iscmp(op int) bool {
	return op == T_EQ || op == T_NE || op == '<' || op == T_LE || op == '>' || op == T_GE
}

func fold(op int, x int, y int) int {
	if op == '+' {
		return x + y
	}
	if op == '-' {
		return x - y
	}
	if op == '*' {
		return x * y
	}
	if op == '/' || op == '%' {
		if y == 0 {
			die("division by zero")
		}
		if op == '/' {
			return x / y
		}
		return x % y
	}
	if op == T_SHL {
		return x << y
	}
	if op == T_SHR {
		return x >> y
	}
	if op == '&' {
		return x & y
	}
	if op == '|' {
		return x | y
	}
	if op == '^' {
		return x ^ y
	}
	if op == T_ANDNOT {
		return x &^ y
	}
	r := false
	if op == T_EQ {
		r = x == y
	} else if op == T_NE {
		r = x != y
	} else if op == '<' {
		r = x < y
	} else if op == T_LE {
		r = x <= y
	} else if op == '>' {
		r = x > y
	} else if op == T_GE {
		r = x >= y
	} else if op == T_ANDAND {
		r = x != 0 && y != 0
	} else if op == T_OROR {
		r = x != 0 || y != 0
	}
	if r {
		return 1
	}
	return 0
}

// the binary operator op applied to l and r
func binary(op int, l int, r int) int {
	errline = nline[l]
	if op == T_ANDAND || op == T_OROR {
		if nt[l] != TBOOL || nt[r] != TBOOL {
			die("&& or || of a non-bool")
		}
		if nk[l] == E_CONST && nk[r] == E_CONST {
			return constnode(fold(op, nv[l], nv[r]), TBOOL)
		}
		return node(E_BIN, TBOOL, l, r, -1, -1, op)
	}
	if op == T_SHL || op == T_SHR {
		if !isint(nt[l]) || !isint(nt[r]) {
			die("shift of a non-integer")
		}
		if nk[l] == E_CONST && nk[r] == E_CONST {
			return constnode(fold(op, nv[l], nv[r]), nt[l])
		}
		settle(l)
		if nt[r] == TUNTYPED {
			convert(r, nt[l])
		}
		return node(E_BIN, nt[l], l, r, -1, -1, op)
	}
	if nt[l] == TUNTYPED && nt[r] != TUNTYPED {
		convert(l, nt[r])
	}
	if nt[r] == TUNTYPED && nt[l] != TUNTYPED {
		convert(r, nt[l])
	}
	t := nt[l]
	if nt[r] != t {
		die("mismatched types " + typename(t) + " and " + typename(nt[r]))
	}
	rt := t
	if iscmp(op) {
		rt = TBOOL
		if isslice(t) {
			die("slices can't be compared")
		}
		if t == TBOOL && op != T_EQ && op != T_NE {
			die("bools are only == or !=")
		}
	} else if t == TSTR {
		if op != '+' {
			die("invalid string operation")
		}
	} else if !isint(t) {
		die("arithmetic on " + typename(t))
	}
	if nk[l] == E_CONST && nk[r] == E_CONST {
		v := fold(op, nv[l], nv[r])
		if t == TBYTE && !iscmp(op) {
			v = v & 255
		}
		return constnode(v, rt)
	}
	return node(E_BIN, rt, l, r, -1, -1, op)
}

func binexpr(minprec int) int {
	l := unary()
	for {
		op := tk[tp]
		p := prec(op)
		if p == 0 || p < minprec {
			return l
		}
		tp++
		r := binexpr(p + 1)
		l = binary(op, l, r)
	}
}

func parseexpr() int { return binexpr(1) }

// ------------------------------------------------------------ statements

func isaddressable(e int) bool {
	k := nk[e]
	return k == E_LOCAL || k == E_GLOBAL || k == E_INDEX && nt[na[e]] != TSTR
}

// are the next tokens  name {, name} :=  ?
func isdefine() bool {
	i := tp
	for tk[i] == T_ID {
		i++
		if tk[i] == T_DEFINE {
			return true
		}
		if tk[i] != ',' {
			return false
		}
		i++
	}
	return false
}

func parseexprlist() int {
	liststart()
	head := -1
	tail := -1
	for {
		e := parseexpr()
		c := cons(e, -1)
		if tail < 0 {
			head = c
		} else {
			nb[tail] = c
		}
		tail = c
		if !accept(',') {
			return head
		}
	}
}

var scopestart int // where the innermost block's symbols start

// name {, name} := exprs
func define() int {
	errline = tline[tp]
	start := tp
	nnames := 0
	for tk[tp] == T_ID {
		nnames++
		tp++
		if !accept(',') {
			break
		}
	}
	expect(T_DEFINE)
	rhs := parseexprlist()
	return definenames(start, nnames, rhs)
}

// declare the names at tokens start, start+2, ... as new variables, from rhs
func definenames(start int, nnames int, rhs int) int {
	var types []int
	if listlen(rhs) == 1 && nt[na[rhs]] == TTUPLE {
		f := na[na[rhs]]
		if fnr[f] != nnames {
			die("assignment mismatch")
		}
		for i := 0; i < nnames; i++ {
			types = append(types, rtypes[frstart[f]+i])
		}
	} else {
		if listlen(rhs) != nnames {
			die("assignment mismatch")
		}
		for l := rhs; l >= 0; l = nb[l] {
			settle(na[l])
			if nt[na[l]] == TVOID || nt[na[l]] == TTUPLE {
				die("no value")
			}
			types = append(types, nt[na[l]])
		}
	}
	liststart()
	head := -1
	tail := -1
	anynew := false
	for i := 0; i < nnames; i++ {
		t := start + 2*i
		s := lookup(tv[t], tl[t])
		if same(tv[t], tl[t], "_") {
			s = declare(tv[t], tl[t], types[i])
		} else if s < scopestart || skind[s] != S_LOCAL {
			s = declare(tv[t], tl[t], types[i])
			anynew = true
		} else if stype[s] != types[i] {
			die("type mismatch in :=")
		}
		c := cons(node(E_LOCAL, stype[s], sval[s], -1, -1, -1, 0), -1)
		if tail < 0 {
			head = c
		} else {
			nb[tail] = c
		}
		tail = c
	}
	if !anynew && nnames > 0 && !same(tv[start], tl[start], "_") {
		die("no new variables on left side of :=")
	}
	return node(S_ASSIGN, 0, head, rhs, -1, -1, 0)
}

func assignop(t int) int {
	if t >= T_ADDA && t <= T_XORA {
		ops := "+-*/%&|^"
		return int(ops[t-T_ADDA])
	}
	if t == T_SHLA {
		return T_SHL
	}
	if t == T_SHRA {
		return T_SHR
	}
	if t == T_ANDNOTA {
		return T_ANDNOT
	}
	return 0
}

func simplestmt() int {
	errline = tline[tp]
	if isdefine() {
		return define()
	}
	lhs := parseexprlist()
	t := tk[tp]
	if t == '=' {
		tp++
		rhs := parseexprlist()
		return checkassign(lhs, rhs)
	}
	if assignop(t) != 0 {
		tp++
		if listlen(lhs) != 1 {
			die("bad assignment")
		}
		r := parseexpr()
		l := na[lhs]
		if !isaddressable(l) {
			die("cannot assign")
		}
		b := binary(assignop(t), l, r) // for its type checks
		if nk[b] != E_BIN {
			die("cannot assign")
		}
		return node(S_OPASSIGN, 0, l, r, -1, -1, assignop(t))
	}
	if t == T_INC || t == T_DEC {
		tp++
		l := na[lhs]
		if !isaddressable(l) || !isint(nt[l]) {
			die("bad ++ or --")
		}
		op := int('+')
		if t == T_DEC {
			op = '-'
		}
		return node(S_OPASSIGN, 0, l, constnode(1, nt[l]), -1, -1, op)
	}
	if listlen(lhs) != 1 {
		die("syntax error")
	}
	e := na[lhs]
	k := nk[e]
	if k != E_CALL && k != E_PRINT && k != E_PANIC && k != E_INTRIN && k != E_COPY {
		die("expression is not used")
	}
	return node(S_EXPR, 0, e, -1, -1, -1, 0)
}

func checkassign(lhs int, rhs int) int {
	n := listlen(lhs)
	if listlen(rhs) == 1 && nt[na[rhs]] == TTUPLE {
		f := na[na[rhs]]
		if fnr[f] != n {
			die("assignment mismatch")
		}
		i := 0
		for l := lhs; l >= 0; l = nb[l] {
			if !isblank(na[l]) && (!isaddressable(na[l]) || nt[na[l]] != rtypes[frstart[f]+i]) {
				die("bad assignment")
			}
			i++
		}
		return node(S_ASSIGN, 0, lhs, rhs, -1, -1, 0)
	}
	if listlen(rhs) != n {
		die("assignment mismatch")
	}
	r := rhs
	for l := lhs; l >= 0; l = nb[l] {
		if isblank(na[l]) {
			settle(na[r])
			nt[na[l]] = nt[na[r]]
		} else if !isaddressable(na[l]) {
			die("cannot assign")
		} else {
			convert(na[r], nt[na[l]])
		}
		r = nb[r]
	}
	return node(S_ASSIGN, 0, lhs, rhs, -1, -1, 0)
}

func isblank(e int) bool { return nk[e] == E_BLANK }

var curfunc int

func block() int {
	expect('{')
	save := scopestart
	mark := nsym
	scopestart = nsym
	liststart()
	head := -1
	tail := -1
	for !accept('}') {
		s := stmt()
		if s >= 0 {
			c := cons(s, -1)
			if tail < 0 {
				head = c
			} else {
				nb[tail] = c
			}
			tail = c
		}
	}
	nsym = mark
	scopestart = save
	return node(S_BLOCK, 0, head, -1, -1, -1, 0)
}

func endstmt() {
	if tk[tp] != '}' {
		expect(';')
	}
}

func stmt() int {
	errline = tline[tp]
	t := tk[tp]
	if accept(';') {
		return -1
	}
	if t == '{' {
		b := block()
		endstmt()
		return b
	}
	if accept(T_VAR) {
		s := vardecl()
		endstmt()
		return s
	}
	if accept(T_IF) {
		s := ifstmt()
		endstmt()
		return s
	}
	if accept(T_FOR) {
		s := forstmt()
		endstmt()
		return s
	}
	if accept(T_SWITCH) {
		s := switchstmt()
		endstmt()
		return s
	}
	if accept(T_RETURN) {
		r := -1
		if tk[tp] != ';' && tk[tp] != '}' {
			r = parseexprlist()
		}
		f := curfunc
		if listlen(r) == 1 && nt[na[r]] == TTUPLE {
			g := na[na[r]]
			if fnr[g] != fnr[f] {
				die("wrong number of return values")
			}
			for i := 0; i < fnr[f]; i++ {
				if rtypes[frstart[g]+i] != rtypes[frstart[f]+i] {
					die("wrong return types")
				}
			}
		} else {
			if listlen(r) != fnr[f] {
				die("wrong number of return values")
			}
			i := 0
			for l := r; l >= 0; l = nb[l] {
				convert(na[l], rtypes[frstart[f]+i])
				i++
			}
		}
		endstmt()
		return node(S_RETURN, 0, r, -1, -1, -1, 0)
	}
	if accept(T_BREAK) {
		endstmt()
		return node(S_BREAK, 0, -1, -1, -1, -1, 0)
	}
	if accept(T_CONTINUE) {
		endstmt()
		return node(S_CONTINUE, 0, -1, -1, -1, -1, 0)
	}
	if t == T_CONST {
		die("local constants are not supported")
	}
	s := simplestmt()
	endstmt()
	return s
}

// var name type [= expr]  or  var name = expr
func vardecl() int {
	errline = tline[tp]
	if tk[tp] != T_ID {
		die("expected a name")
	}
	p := tv[tp]
	n := tl[tp]
	tp++
	t := -1
	if tk[tp] != '=' {
		t = parsetype()
	}
	if accept('=') {
		e := parseexpr()
		if t >= 0 {
			convert(e, t)
		}
		settle(e)
		s := declare(p, n, nt[e])
		return node(S_ASSIGN, 0, cons(node(E_LOCAL, nt[e], sval[s], -1, -1, -1, 0), -1), cons(e, -1), -1, -1, 0)
	}
	s := declare(p, n, t)
	return node(S_ZERO, t, sval[s], -1, -1, -1, 0)
}

func ifstmt() int {
	save := scopestart
	mark := nsym
	scopestart = nsym
	init := -1
	if !istokbefore(';', '{') {
		init = -1
	} else {
		init = simplestmt()
		expect(';')
	}
	c := parseexpr()
	if nt[c] != TBOOL {
		die("non-bool condition")
	}
	then := block()
	els := -1
	if accept(T_ELSE) {
		if accept(T_IF) {
			els = ifstmt()
		} else {
			els = block()
		}
	}
	nsym = mark
	scopestart = save
	return node(S_IF, 0, c, then, els, init, 0)
}

// is there a token t before the next u, at this nesting level?
func istokbefore(t int, u int) bool {
	depth := 0
	for i := tp; tk[i] != T_EOF; i++ {
		if tk[i] == '(' || tk[i] == '[' {
			depth++
		}
		if tk[i] == ')' || tk[i] == ']' {
			depth--
		}
		if depth == 0 && tk[i] == u {
			return false
		}
		if depth == 0 && tk[i] == t {
			return true
		}
	}
	return false
}

func forstmt() int {
	save := scopestart
	mark := nsym
	scopestart = nsym
	var s int
	if tk[tp] == '{' {
		s = node(S_FOR, 0, -1, -1, -1, block(), 0)
	} else if istokbefore(T_RANGE, '{') {
		s = rangeloop()
	} else if !istokbefore(';', '{') {
		c := parseexpr()
		if nt[c] != TBOOL {
			die("non-bool condition")
		}
		s = node(S_FOR, 0, -1, c, -1, block(), 0)
	} else {
		init := -1
		if tk[tp] != ';' {
			init = simplestmt()
		}
		expect(';')
		c := -1
		if tk[tp] != ';' {
			c = parseexpr()
			if nt[c] != TBOOL {
				die("non-bool condition")
			}
		}
		expect(';')
		post := -1
		if tk[tp] != '{' {
			post = simplestmt()
		}
		s = node(S_FOR, 0, init, c, post, block(), 0)
	}
	nsym = mark
	scopestart = save
	return s
}

// for [k [, v] :=] range x
func rangeloop() int {
	kp := -1
	vp := -1
	if tk[tp] != T_RANGE {
		kp = tp
		tp++
		if accept(',') {
			vp = tp
			tp++
		}
		expect(T_DEFINE)
	}
	expect(T_RANGE)
	x := parseexpr()
	settle(x)
	t := nt[x]
	if !isslice(t) && t != TINT {
		die("range over " + typename(t) + " is not supported")
	}
	if vp >= 0 && t == TINT {
		die("range over an int has one variable")
	}
	ks := -1
	vs := -1
	if kp >= 0 {
		ks = sval[declare(tv[kp], tl[kp], TINT)]
	}
	if vp >= 0 {
		vs = sval[declare(tv[vp], tl[vp], elemtype(t))]
	}
	return node(S_RANGE, 0, ks, vs, x, block(), 0)
}

func switchstmt() int {
	save := scopestart
	mark := nsym
	scopestart = nsym
	init := -1
	if istokbefore(';', '{') {
		init = simplestmt()
		expect(';')
	}
	tag := -1
	if tk[tp] != '{' {
		tag = parseexpr()
		settle(tag)
	}
	expect('{')
	liststart()
	head := -1
	tail := -1
	for !accept('}') {
		errline = tline[tp]
		exprs := -1
		if accept(T_DEFAULT) {
			exprs = -1
		} else {
			expect(T_CASE)
			exprs = parseexprlist()
			for l := exprs; l >= 0; l = nb[l] {
				if tag >= 0 {
					convert(na[l], nt[tag])
				} else if nt[na[l]] != TBOOL {
					die("non-bool case")
				}
			}
		}
		expect(':')
		bmark := nsym
		bsave := scopestart
		scopestart = nsym
		bhead := -1
		btail := -1
		for tk[tp] != T_CASE && tk[tp] != T_DEFAULT && tk[tp] != '}' {
			s := stmt()
			if s >= 0 {
				c := cons(s, -1)
				if btail < 0 {
					bhead = c
				} else {
					nb[btail] = c
				}
				btail = c
			}
		}
		nsym = bmark
		scopestart = bsave
		c := cons(node(S_CASE, 0, exprs, bhead, -1, -1, 0), -1)
		if tail < 0 {
			head = c
		} else {
			nb[tail] = c
		}
		tail = c
	}
	nsym = mark
	scopestart = save
	return node(S_SWITCH, 0, init, tag, head, -1, 0)
}

// ---------------------------------------------------------------- output

var fb []byte // the current function's body

func emit(b int) { fb = append(fb, byte(b)) }

func uleb(v int) {
	for v >= 128 {
		emit(v&127 | 128)
		v = v >> 7
	}
	emit(v)
}

func sleb(v int) {
	for {
		b := v & 127
		v = v >> 7
		if v == 0 && b&64 == 0 || v == -1 && b&64 != 0 {
			emit(b)
			return
		}
		emit(b | 128)
	}
}

func iconst(v int) {
	emit(0x41)
	sleb(v)
}
func lconst(v int) {
	emit(0x42)
	sleb(v)
}
func lget(i int) {
	emit(0x20)
	uleb(i)
}
func lset(i int) {
	emit(0x21)
	uleb(i)
}
func ltee(i int) {
	emit(0x22)
	uleb(i)
}
func callf(f int) {
	emit(0x10)
	uleb(fwasm[f])
}

var dataseg []byte // string literals, at database
var database int
var globalsize int

// the address of string literal bytes strs[p..p+n)
func strdata(p int, n int) int {
	a := database + len(dataseg)
	for i := 0; i < n; i++ {
		dataseg = append(dataseg, strs[p+i])
	}
	return a
}

// load a value of type t from the address in local a, plus off
func loadmem(t int, a int, off int) {
	if t == TINT {
		lget(a)
		emit(0x29)
		emit(3)
		uleb(off)
		return
	}
	if t == TBYTE || t == TBOOL {
		lget(a)
		emit(0x2d)
		emit(0)
		uleb(off)
		return
	}
	for i := 0; i < words(t); i++ {
		lget(a)
		emit(0x28)
		emit(2)
		uleb(off + 4*i)
	}
}

// store the value of type t in locals v.. at the address in local a, plus off
func storemem(t int, a int, off int, v int) {
	if t == TINT {
		lget(a)
		lget(v)
		emit(0x37)
		emit(3)
		uleb(off)
		return
	}
	if t == TBYTE || t == TBOOL {
		lget(a)
		lget(v)
		emit(0x3a)
		emit(0)
		uleb(off)
		return
	}
	for i := 0; i < words(t); i++ {
		lget(a)
		lget(v + i)
		emit(0x36)
		emit(2)
		uleb(off + 4*i)
	}
}

// new locals for a value of type t: the first's index
func templocals(t int) int {
	first := nloc
	for i := 0; i < words(t); i++ {
		if i == 0 {
			newlocal(wtype(t))
		} else {
			newlocal(I32)
		}
	}
	return first
}

// pop a value of type t from the stack into new locals
func spill(t int) int {
	v := templocals(t)
	for i := words(t) - 1; i >= 0; i-- {
		lset(v + i)
	}
	return v
}

func pushlocals(t int, v int) {
	for i := 0; i < words(t); i++ {
		lget(v + i)
	}
}

func drop(n int) {
	for i := 0; i < n; i++ {
		emit(0x1a)
	}
}

// --------------------------------------------------------- code generation

func opcode(op int, t int) int {
	w := wtype(t) == I64
	if op == '+' {
		if w {
			return 0x7c
		}
		return 0x6a
	}
	if op == '-' {
		if w {
			return 0x7d
		}
		return 0x6b
	}
	if op == '*' {
		if w {
			return 0x7e
		}
		return 0x6c
	}
	if op == '/' {
		if w {
			return 0x7f
		}
		if t == TBYTE {
			return 0x6e
		}
		return 0x6d
	}
	if op == '%' {
		if w {
			return 0x81
		}
		if t == TBYTE {
			return 0x70
		}
		return 0x6f
	}
	if op == '&' {
		if w {
			return 0x83
		}
		return 0x71
	}
	if op == '|' {
		if w {
			return 0x84
		}
		return 0x72
	}
	if op == '^' {
		if w {
			return 0x85
		}
		return 0x73
	}
	if op == T_SHL {
		if w {
			return 0x86
		}
		return 0x74
	}
	if op == T_SHR {
		if w {
			return 0x87
		}
		if t == TBYTE {
			return 0x76
		}
		return 0x75
	}
	if op == T_EQ {
		if w {
			return 0x51
		}
		return 0x46
	}
	if op == T_NE {
		if w {
			return 0x52
		}
		return 0x47
	}
	u := t == TBYTE
	if op == '<' {
		if w {
			return 0x53
		}
		if u {
			return 0x49
		}
		return 0x48
	}
	if op == '>' {
		if w {
			return 0x55
		}
		if u {
			return 0x4b
		}
		return 0x4a
	}
	if op == T_LE {
		if w {
			return 0x57
		}
		if u {
			return 0x4d
		}
		return 0x4c
	}
	if w {
		return 0x59
	}
	if u {
		return 0x4f
	}
	return 0x4e
}

// an integer on the stack, as an int
func widen(t int) {
	if wtype(t) == I32 {
		if t == TBYTE {
			emit(0xad)
		} else {
			emit(0xac)
		}
	}
}

// an int on the stack, as an i32 (for addresses and lengths)
func toi32(t int) {
	if wtype(t) == I64 {
		emit(0xa7)
	}
}

// the operator op on the values on the stack, of type t
func arith(op int, t int) {
	if op == T_ANDNOT {
		if wtype(t) == I64 {
			lconst(-1)
			emit(0x85)
		} else {
			iconst(-1)
			emit(0x73)
		}
		emit(opcode('&', t))
		return
	}
	emit(opcode(op, t))
	if t == TBYTE && (op == '+' || op == '-' || op == '*' || op == T_SHL) {
		iconst(255)
		emit(0x71)
	}
}

func gencall(e int) {
	for l := nb[e]; l >= 0; l = nb[l] {
		gen(na[l])
	}
	callf(na[e])
}

// a slice or string value on the stack: keep its pointer and length
func ptrlen(t int) {
	if isslice(t) {
		drop(1)
	}
}

func gen(e int) {
	errline = nline[e]
	k := nk[e]
	t := nt[e]
	if k == E_CONST {
		if t == TUNTYPED || t == TINT {
			lconst(nv[e])
		} else {
			iconst(nv[e])
		}
		return
	}
	if k == E_STR {
		iconst(strdata(na[e], nb[e]))
		iconst(nb[e])
		return
	}
	if k == E_LOCAL {
		pushlocals(t, na[e])
		return
	}
	if k == E_GLOBAL {
		a := newlocal(I32)
		iconst(na[e])
		lset(a)
		loadmem(t, a, 0)
		return
	}
	if k == E_BIN {
		op := nv[e]
		l := na[e]
		r := nb[e]
		if op == T_ANDAND || op == T_OROR {
			gen(l)
			emit(0x04)
			emit(I32)
			if op == T_ANDAND {
				gen(r)
				emit(0x05)
				iconst(0)
			} else {
				iconst(1)
				emit(0x05)
				gen(r)
			}
			emit(0x0b)
			return
		}
		lt := nt[l]
		gen(l)
		gen(r)
		if lt == TSTR {
			if op == '+' {
				callf(findfunc("rt_concat"))
				return
			}
			callf(findfunc("rt_strcmp"))
			lconst(0)
			emit(opcode(op, TINT))
			return
		}
		if op == T_SHL || op == T_SHR {
			if wtype(nt[r]) != wtype(lt) {
				if wtype(lt) == I64 {
					emit(0xad) // extend_u
				} else {
					emit(0xa7)
				}
			}
		}
		if iscmp(op) {
			emit(opcode(op, lt))
			return
		}
		arith(op, lt)
		return
	}
	if k == E_UN {
		op := nv[e]
		if op == '!' {
			gen(na[e])
			emit(0x45)
			return
		}
		if op == '-' {
			if wtype(t) == I64 {
				lconst(0)
			} else {
				iconst(0)
			}
			gen(na[e])
			arith('-', t)
			return
		}
		gen(na[e]) // ^
		if wtype(t) == I64 {
			lconst(-1)
		} else {
			iconst(-1)
		}
		arith('^', t)
		if t == TBYTE {
			iconst(255)
			emit(0x71)
		}
		return
	}
	if k == E_CALL {
		gencall(e)
		return
	}
	if k == E_INDEX {
		a := indexaddr(e)
		loadmem(t, a, 0)
		return
	}
	if k == E_SLICE {
		genslice(e)
		return
	}
	if k == E_LEN || k == E_CAP {
		x := na[e]
		gen(x)
		c := newlocal(I32)
		if isslice(nt[x]) && k == E_CAP {
			lset(c)
			drop(2)
		} else {
			if isslice(nt[x]) {
				drop(1)
			}
			lset(c)
			drop(1)
		}
		lget(c)
		emit(0xad) // i64.extend_i32_u
		return
	}
	if k == E_APPEND {
		genappend(e)
		return
	}
	if k == E_MAKE {
		size := msize(elemtype(t))
		gen(na[e])
		toi32(TINT)
		l := newlocal(I32)
		lset(l)
		c := l
		if nb[e] >= 0 {
			gen(nb[e])
			toi32(TINT)
			c = newlocal(I32)
			lset(c)
		}
		lget(c)
		iconst(size)
		emit(0x6c)
		callf(findfunc("rt_alloc"))
		lget(l)
		lget(c)
		return
	}
	if k == E_COPY {
		size := msize(elemtype(nt[na[e]]))
		gen(na[e])
		ptrlen(nt[na[e]])
		d := spill(TSTR)
		gen(nb[e])
		ptrlen(nt[nb[e]])
		s := spill(TSTR)
		n := newlocal(I32)
		lget(d + 1)
		lget(s + 1)
		lget(d + 1)
		lget(s + 1)
		emit(0x49) // lt_u
		emit(0x1b) // select: the smaller
		lset(n)
		lget(d)
		lget(s)
		lget(n)
		iconst(size)
		emit(0x6c)
		emit(0xfc)
		emit(10)
		emit(0)
		emit(0)
		lget(n)
		emit(0xad)
		return
	}
	if k == E_CONV {
		x := na[e]
		f := nt[x]
		gen(x)
		if isint(t) {
			if wtype(f) == I64 && wtype(t) == I32 {
				emit(0xa7)
			} else if wtype(f) == I32 && wtype(t) == I64 {
				if f == TBYTE {
					emit(0xad)
				} else {
					emit(0xac)
				}
			}
			if t == TBYTE && f != TBYTE {
				iconst(255)
				emit(0x71)
			}
			return
		}
		// string <-> []byte: copy
		ptrlen(f)
		s := spill(TSTR)
		lget(s + 1)
		callf(findfunc("rt_alloc"))
		p := newlocal(I32)
		ltee(p)
		lget(s)
		lget(s + 1)
		emit(0xfc)
		emit(10)
		emit(0)
		emit(0)
		lget(p)
		lget(s + 1)
		if isslice(t) {
			lget(s + 1)
		}
		return
	}
	if k == E_INTRIN {
		genintrinsic(e)
		return
	}
	if k == E_PRINT {
		first := true
		for l := nb[e]; l >= 0; l = nb[l] {
			x := na[l]
			if !first && nv[e] == 1 {
				printlit(" ")
			}
			first = false
			gen(x)
			xt := nt[x]
			if xt == TSTR {
				callf(findfunc("rt_printstr"))
			} else if xt == TBOOL {
				callf(findfunc("rt_printbool"))
			} else if isint(xt) {
				if wtype(xt) == I32 {
					if xt == TBYTE {
						emit(0xad)
					} else {
						emit(0xac)
					}
				}
				callf(findfunc("rt_printint"))
			} else {
				die("can't print " + typename(xt))
			}
		}
		if nv[e] == 1 {
			printlit("\n")
		}
		return
	}
	if k == E_PANIC {
		gen(na[e])
		callf(findfunc("rt_fail"))
		return
	}
	die("internal error: gen")
}

func printlit(lit string) {
	p := len(strs)
	strs = append(strs, lit...)
	iconst(strdata(p, len(lit)))
	iconst(len(lit))
	callf(findfunc("rt_printstr"))
}

// the address of element e = s[i] (checked), in a new local
func indexaddr(e int) int {
	s := na[e]
	st := nt[s]
	size := 1
	if isslice(st) {
		size = msize(elemtype(st))
	}
	gen(s)
	ptrlen(st)
	v := spill(TSTR)
	gen(nb[e])
	widen(nt[nb[e]])
	i := newlocal(I64)
	ltee(i)
	lget(v + 1)
	emit(0xad)
	emit(0x5a) // i64.ge_u: out of range
	emit(0x04)
	emit(0x40)
	callf(findfunc("rt_bounds"))
	emit(0x0b)
	a := newlocal(I32)
	lget(v)
	lget(i)
	emit(0xa7)
	if size != 1 {
		iconst(size)
		emit(0x6c)
	}
	emit(0x6a)
	lset(a)
	return a
}

// s[lo:hi]
func genslice(e int) {
	s := na[e]
	st := nt[s]
	size := 1
	if isslice(st) {
		size = msize(elemtype(st))
	}
	gen(s)
	v := templocals(slicetype(TBYTE)) // p, len, cap
	if isslice(st) {
		lset(v + 2)
	}
	lset(v + 1)
	lset(v)
	if !isslice(st) {
		lget(v + 1)
		lset(v + 2)
	}
	lo := newlocal(I32)
	hi := newlocal(I32)
	if nb[e] >= 0 {
		gen(nb[e])
		toi32(TINT)
	} else {
		iconst(0)
	}
	lset(lo)
	if nc[e] >= 0 {
		gen(nc[e])
		toi32(TINT)
	} else {
		lget(v + 1)
	}
	lset(hi)
	// 0 <= lo <= hi <= cap (the length, for strings)
	lget(hi)
	lget(v + 2)
	emit(0x4b) // gt_u
	lget(lo)
	lget(hi)
	emit(0x4b)
	emit(0x72)
	emit(0x04)
	emit(0x40)
	callf(findfunc("rt_bounds"))
	emit(0x0b)
	lget(v)
	lget(lo)
	if size != 1 {
		iconst(size)
		emit(0x6c)
	}
	emit(0x6a)
	lget(hi)
	lget(lo)
	emit(0x6b)
	if isslice(st) {
		lget(v + 2)
		lget(lo)
		emit(0x6b)
	}
}

func genappend(e int) {
	t := nt[e]
	et := elemtype(t)
	size := msize(et)
	gen(na[e])
	v := spill(t) // p, len, cap
	if nc[e] == 1 { // append(s, x...)
		x := na[nb[e]]
		gen(x)
		ptrlen(nt[x])
		w := spill(TSTR)
		need := newlocal(I32)
		lget(v + 1)
		lget(w + 1)
		emit(0x6a)
		lset(need)
		growto(v, need, size)
		lget(v)
		lget(v + 1)
		iconst(size)
		emit(0x6c)
		emit(0x6a)
		lget(w)
		lget(w + 1)
		iconst(size)
		emit(0x6c)
		emit(0xfc)
		emit(10)
		emit(0)
		emit(0)
		lget(v)
		lget(need)
		lget(v + 2)
		return
	}
	// the elements first, then room for them
	n := listlen(nb[e])
	var vals []int
	for l := nb[e]; l >= 0; l = nb[l] {
		gen(na[l])
		vals = append(vals, spill(et))
	}
	need := newlocal(I32)
	lget(v + 1)
	iconst(n)
	emit(0x6a)
	lset(need)
	growto(v, need, size)
	a := newlocal(I32)
	lget(v)
	lget(v + 1)
	iconst(size)
	emit(0x6c)
	emit(0x6a)
	lset(a)
	for i := 0; i < n; i++ {
		storemem(et, a, i*size, vals[i])
	}
	lget(v)
	lget(need)
	lget(v + 2)
}

// make the slice in locals v (p, len, cap) have room for need elements
func growto(v int, need int, size int) {
	lget(v)
	lget(v + 1)
	lget(v + 2)
	lget(need)
	iconst(size)
	callf(findfunc("rt_grow"))
	lset(v + 2)
	lset(v)
}

func genintrinsic(e int) {
	w := nv[e]
	for l := nb[e]; l >= 0; l = nb[l] {
		gen(na[l])
		if w == 7 {
			ptrlen(nt[na[l]])
			drop(1)
		}
	}
	if w == 1 {
		emit(0x23)
		emit(0)
	} else if w == 2 {
		emit(0x3f)
		emit(0)
	} else if w == 3 {
		emit(0x40)
		emit(0)
	} else if w == 4 {
		emit(0x28)
		emit(2)
		emit(0)
	} else if w == 5 {
		emit(0x36)
		emit(2)
		emit(0)
	} else if w == 6 {
		emit(0xfc)
		emit(10)
		emit(0)
		emit(0)
	}
}

// store the value on the stack (of the lvalue's type) into lvalue e
func store(e int) {
	t := nt[e]
	k := nk[e]
	if k == E_BLANK {
		drop(words(t))
		return
	}
	if k == E_LOCAL {
		v := na[e]
		for i := words(t) - 1; i >= 0; i-- {
			lset(v + i)
		}
		return
	}
	v := spill(t)
	a := -1
	if k == E_GLOBAL {
		a = newlocal(I32)
		iconst(na[e])
		lset(a)
	} else {
		a = indexaddr(e)
	}
	storemem(t, a, 0, v)
}

// --------------------------------------------------------------- statements

var depth int // nested wasm blocks
var brk int   // the depth that break leaves, and that continue repeats
var cont int

func genstmt(s int) {
	errline = nline[s]
	k := nk[s]
	if k == S_BLOCK {
		for l := na[s]; l >= 0; l = nb[l] {
			genstmt(na[l])
		}
		return
	}
	if k == S_EXPR {
		e := na[s]
		gen(e)
		if nt[e] == TTUPLE {
			f := na[e]
			for i := 0; i < fnr[f]; i++ {
				drop(words(rtypes[frstart[f]+i]))
			}
		} else {
			drop(words(nt[e]))
		}
		return
	}
	if k == S_ZERO {
		t := nt[s]
		v := na[s]
		for i := 0; i < words(t); i++ {
			if i == 0 && wtype(t) == I64 {
				lconst(0)
			} else {
				iconst(0)
			}
			lset(v + i)
		}
		return
	}
	if k == S_ASSIGN {
		lhs := na[s]
		rhs := nb[s]
		if listlen(lhs) == 1 && nt[na[rhs]] != TTUPLE {
			gen(na[rhs])
			store(na[lhs])
			return
		}
		// several: all the values first, then the stores
		var vals []int
		var types []int
		if nt[na[rhs]] == TTUPLE {
			f := na[na[rhs]]
			gen(na[rhs])
			for i := fnr[f] - 1; i >= 0; i-- {
				t := rtypes[frstart[f]+i]
				types = append(types, t)
				vals = append(vals, spill(t))
			}
			n := len(vals)
			for i := 0; i < n/2; i++ { // spilled last first: reverse
				x := vals[i]
				vals[i] = vals[n-1-i]
				vals[n-1-i] = x
				x = types[i]
				types[i] = types[n-1-i]
				types[n-1-i] = x
			}
		} else {
			for l := rhs; l >= 0; l = nb[l] {
				gen(na[l])
				types = append(types, nt[na[l]])
				vals = append(vals, spill(nt[na[l]]))
			}
		}
		i := 0
		for l := lhs; l >= 0; l = nb[l] {
			pushlocals(types[i], vals[i])
			store(na[l])
			i++
		}
		return
	}
	if k == S_OPASSIGN {
		l := na[s]
		t := nt[l]
		if nk[l] == E_INDEX {
			// evaluate the address once
			a := indexaddr(l)
			loadmem(t, a, 0)
			gen(nb[s])
			if t == TSTR {
				callf(findfunc("rt_concat"))
			} else {
				shiftfix(nv[s], t, nt[nb[s]])
				arith(nv[s], t)
			}
			v := spill(t)
			storemem(t, a, 0, v)
			return
		}
		gen(l)
		gen(nb[s])
		if t == TSTR {
			callf(findfunc("rt_concat"))
		} else {
			shiftfix(nv[s], t, nt[nb[s]])
			arith(nv[s], t)
		}
		store(l)
		return
	}
	if k == S_IF {
		if nd[s] >= 0 {
			genstmt(nd[s])
		}
		gen(na[s])
		emit(0x04)
		emit(0x40)
		depth++
		genstmt(nb[s])
		if nc[s] >= 0 {
			emit(0x05)
			genstmt(nc[s])
		}
		emit(0x0b)
		depth--
		return
	}
	if k == S_FOR {
		if na[s] >= 0 {
			genstmt(na[s])
		}
		ob := brk
		oc := cont
		emit(0x02)
		emit(0x40)
		depth++
		brk = depth
		emit(0x03)
		emit(0x40)
		depth++
		top := depth
		if nb[s] >= 0 {
			gen(nb[s])
			emit(0x45)
			emit(0x0d)
			uleb(depth - brk)
		}
		emit(0x02)
		emit(0x40)
		depth++
		cont = depth
		genstmt(nd[s])
		emit(0x0b)
		depth--
		if nc[s] >= 0 {
			genstmt(nc[s])
		}
		emit(0x0c)
		uleb(depth - top)
		emit(0x0b)
		emit(0x0b)
		depth = depth - 2
		brk = ob
		cont = oc
		return
	}
	if k == S_RANGE {
		x := nc[s]
		xt := nt[x]
		gen(x)
		n := -1
		v := -1
		if xt == TINT {
			n = newlocal(I64)
			lset(n)
		} else {
			v = spill(xt)
		}
		i := newlocal(I64)
		lconst(0)
		lset(i)
		ob := brk
		oc := cont
		emit(0x02)
		emit(0x40)
		depth++
		brk = depth
		emit(0x03)
		emit(0x40)
		depth++
		top := depth
		lget(i)
		if xt == TINT {
			lget(n)
		} else {
			lget(v + 1)
			emit(0xad)
		}
		emit(0x59) // i64.ge_s
		emit(0x0d)
		uleb(depth - brk)
		if na[s] >= 0 {
			lget(i)
			lset(na[s])
		}
		if nb[s] >= 0 {
			et := elemtype(xt)
			a := newlocal(I32)
			lget(v)
			lget(i)
			emit(0xa7)
			iconst(msize(et))
			emit(0x6c)
			emit(0x6a)
			lset(a)
			loadmem(et, a, 0)
			store(node(E_LOCAL, et, nb[s], -1, -1, -1, 0))
		}
		emit(0x02)
		emit(0x40)
		depth++
		cont = depth
		genstmt(nd[s])
		emit(0x0b)
		depth--
		lget(i)
		lconst(1)
		emit(0x7c)
		lset(i)
		emit(0x0c)
		uleb(depth - top)
		emit(0x0b)
		emit(0x0b)
		depth = depth - 2
		brk = ob
		cont = oc
		return
	}
	if k == S_SWITCH {
		if na[s] >= 0 {
			genstmt(na[s])
		}
		tag := nb[s]
		tagv := -1
		if tag >= 0 {
			gen(tag)
			tagv = spill(nt[tag])
		}
		ob := brk
		emit(0x02)
		emit(0x40)
		depth++
		brk = depth
		def := -1
		for l := nc[s]; l >= 0; l = nb[l] {
			c := na[l]
			if na[c] < 0 {
				def = c
				continue
			}
			first := true
			for x := na[c]; x >= 0; x = nb[x] {
				if tag >= 0 {
					pushlocals(nt[tag], tagv)
					gen(na[x])
					if nt[tag] == TSTR {
						callf(findfunc("rt_strcmp"))
						emit(0x50) // i64.eqz
					} else {
						emit(opcode(T_EQ, nt[tag]))
					}
				} else {
					gen(na[x])
				}
				if !first {
					emit(0x72)
				}
				first = false
			}
			emit(0x04)
			emit(0x40)
			depth++
			for b := nb[c]; b >= 0; b = nb[b] {
				genstmt(na[b])
			}
			emit(0x0c)
			uleb(depth - brk)
			emit(0x0b)
			depth--
		}
		if def >= 0 {
			for b := nb[def]; b >= 0; b = nb[b] {
				genstmt(na[b])
			}
		}
		emit(0x0b)
		depth--
		brk = ob
		return
	}
	if k == S_RETURN {
		for l := na[s]; l >= 0; l = nb[l] {
			gen(na[l])
		}
		emit(0x0f)
		return
	}
	if k == S_BREAK {
		if brk == 0 {
			die("break outside a loop or switch")
		}
		emit(0x0c)
		uleb(depth - brk)
		return
	}
	if k == S_CONTINUE {
		if cont == 0 {
			die("continue outside a loop")
		}
		emit(0x0c)
		uleb(depth - cont)
		return
	}
	die("internal error: genstmt")
}

// x op= y where op is a shift: the count's width to x's
func shiftfix(op int, t int, ct int) {
	if (op == T_SHL || op == T_SHR) && wtype(ct) != wtype(t) {
		if wtype(t) == I64 {
			emit(0xad)
		} else {
			emit(0xa7)
		}
	}
}

// ------------------------------------------------------------- top level

var code []byte // the code section's entries
var ndefined int
var initcode []byte // global initializers: the body of the init function
var initlocals []int

func skipblock() {
	d := 0
	for {
		if tk[tp] == T_EOF {
			die("unexpected end of file")
		}
		if tk[tp] == '{' {
			d++
		}
		if tk[tp] == '}' {
			d--
			if d == 0 {
				tp++
				return
			}
		}
		tp++
	}
}

// func name(params) results -- the signature only; the body is skipped
func funcsig() {
	errline = tline[tp]
	if tk[tp] != T_ID {
		die("expected a function name")
	}
	f := len(fname)
	fname = append(fname, tv[tp])
	fnlen = append(fnlen, tl[tp])
	if lookup(tv[tp], tl[tp]) >= 0 {
		die(string(src[tv[tp]:tv[tp]+tl[tp]]) + " redeclared")
	}
	addsym(tv[tp], tl[tp], S_FUNC, 0, f)
	tp++
	expect('(')
	fpstart = append(fpstart, len(ptypes))
	np := 0
	for !accept(')') {
		// a group of names, then their type
		first := len(ptypes)
		for tk[tp] == T_ID {
			pnames = append(pnames, tp)
			ptypes = append(ptypes, 0)
			np++
			tp++
			if !accept(',') {
				break
			}
		}
		if len(ptypes) == first {
			die("parameters must be named")
		}
		t := parsetype()
		for i := first; i < len(ptypes); i++ {
			ptypes[i] = t
		}
		if !accept(',') {
			expect(')')
			break
		}
	}
	fnp = append(fnp, np)
	frstart = append(frstart, len(rtypes))
	nr := 0
	if accept('(') {
		for !accept(')') {
			if tk[tp] == T_ID && !istypeat(tp) {
				die("named results are not supported")
			}
			rtypes = append(rtypes, parsetype())
			nr++
			accept(',')
		}
	} else if tk[tp] != '{' && tk[tp] != ';' {
		rtypes = append(rtypes, parsetype())
		nr++
	}
	fnr = append(fnr, nr)
	if tk[tp] == '{' {
		fbody = append(fbody, tp)
		skipblock()
	} else {
		fbody = append(fbody, -1)
	}
	fwasm = append(fwasm, 0)
	ftype = append(ftype, 0)
}

func istypeat(i int) bool {
	save := tp
	tp = i
	r := istypestart()
	tp = save
	return r
}

// const name [type] = expr, or a group, with iota
var iotav int

func constspec(prevexpr int) int {
	errline = tline[tp]
	p := tv[tp]
	n := tl[tp]
	expect(T_ID)
	t := -1
	if tk[tp] != '=' && tk[tp] != ';' && tk[tp] != ')' {
		t = parsetype()
	}
	exprtok := prevexpr
	if accept('=') {
		exprtok = tp
	}
	if exprtok < 0 {
		die("missing constant value")
	}
	save := tp
	tp = exprtok
	mark := nsym
	universe("iota", S_CONST, TUNTYPED, iotav)
	e := parseexpr()
	end := tp
	nsym = mark
	src = src[:len(src)-4]
	if exprtok == prevexpr {
		tp = save
	} else {
		tp = end
	}
	if nk[e] != E_CONST {
		die("not a constant")
	}
	if t >= 0 {
		convert(e, t)
	}
	addsym(p, n, S_CONST, nt[e], nv[e])
	return exprtok
}

func constdecl() {
	if accept('(') {
		prev := -1
		iotav = 0
		for !accept(')') {
			if accept(';') {
				continue
			}
			prev = constspec(prev)
			iotav++
		}
	} else {
		iotav = 0
		constspec(-1)
	}
}

// var name type [= expr]: a global
func globalspec() {
	errline = tline[tp]
	p := tv[tp]
	n := tl[tp]
	expect(T_ID)
	t := -1
	if tk[tp] != '=' {
		t = parsetype()
	}
	e := -1
	if accept('=') {
		e = parseexpr()
		if t >= 0 {
			convert(e, t)
		}
		settle(e)
		t = nt[e]
	}
	globalsize = (globalsize + 7) &^ 7
	s := addsym(p, n, S_GLOBAL, t, 1024+globalsize)
	globalsize = globalsize + msize(t)
	if e >= 0 {
		initlist = append(initlist, s)
		initexpr = append(initexpr, e)
	}
}

var vartoks []int // where the global var declarations are
var initlist []int
var initexpr []int

func vardecl1() {
	if accept('(') {
		for !accept(')') {
			if accept(';') {
				continue
			}
			globalspec()
		}
	} else {
		globalspec()
	}
}

// skip a declaration (to its end at this nesting level)
func skipdecl() {
	d := 0
	for {
		t := tk[tp]
		if t == T_EOF {
			return
		}
		if t == '(' || t == '{' || t == '[' {
			d++
		}
		if t == ')' || t == '}' || t == ']' {
			d--
		}
		tp++
		if d == 0 && t == ';' {
			return
		}
	}
}

// --------------------------------------------------------------- the module

var out []byte

func oemit(b int) { out = append(out, byte(b)) }
func ouleb(v int) {
	for v >= 128 {
		oemit(v&127 | 128)
		v = v >> 7
	}
	oemit(v)
}
func oname(s string) {
	ouleb(len(s))
	for i := 0; i < len(s); i++ {
		oemit(int(s[i]))
	}
}
func onamesrc(p int, n int) {
	ouleb(n)
	for i := 0; i < n; i++ {
		oemit(int(src[p+i]))
	}
}

var secstart int

func section(id int) {
	oemit(id)
	secstart = len(out)
}

// end the section: insert its size before its contents
func endsection() {
	n := len(out) - secstart
	var size []byte
	v := n
	for v >= 128 {
		size = append(size, byte(v&127|128))
		v = v >> 7
	}
	size = append(size, byte(v))
	out = append(out, size...)
	copy(out[secstart+len(size):], out[secstart:secstart+n])
	copy(out[secstart:], size)
}

// a function's wasm type: params then results, each type's words
func emitvaltypes(start int, n int, types []int) {
	cnt := 0
	for i := 0; i < n; i++ {
		cnt = cnt + words(types[start+i])
	}
	ouleb(cnt)
	for i := 0; i < n; i++ {
		t := types[start+i]
		for w := 0; w < words(t); w++ {
			if w == 0 {
				oemit(wtype(t))
			} else {
				oemit(I32)
			}
		}
	}
}

// compile function f's body into code
func compilefunc(f int) {
	curfunc = f
	tp = fbody[f]
	nloc = 0
	ltypes = ltypes[:0]
	fb = fb[:0]
	depth = 0
	brk = 0
	cont = 0
	mark := nsym
	scopestart = nsym
	for i := 0; i < fnp[f]; i++ {
		q := pnames[fpstart[f]+i]
		declare(tv[q], tl[q], ptypes[fpstart[f]+i])
	}
	nparamlocals := nloc
	body := block()
	genstmt(body)
	if fnr[f] > 0 {
		emit(0) // unreachable: Go requires a return
	}
	emit(0x0b)
	nsym = mark
	finishfunc(nparamlocals)
}

// append the current function (locals declared from nparams on, and fb) to code
func finishfunc(nparams int) {
	var entry []byte
	out2 := out
	out = entry
	ouleb(nloc - nparams)
	for i := nparams; i < nloc; i++ {
		ouleb(1)
		oemit(ltypes[i])
	}
	entry = out
	out = out2
	n := len(entry) + len(fb)
	v := n
	for v >= 128 {
		code = append(code, byte(v&127|128))
		v = v >> 7
	}
	code = append(code, byte(v))
	code = append(code, entry...)
	code = append(code, fb...)
	ndefined++
}

func main() {
	src = readAll()
	lex()
	universe("int", S_TYPE, TINT, 0)
	universe("int64", S_TYPE, TINT, 0)
	universe("uint", S_TYPE, TINT, 0)
	universe("int32", S_TYPE, TI32, 0)
	universe("byte", S_TYPE, TBYTE, 0)
	universe("uint8", S_TYPE, TBYTE, 0)
	universe("bool", S_TYPE, TBOOL, 0)
	universe("string", S_TYPE, TSTR, 0)
	universe("true", S_CONST, TBOOL, 1)
	universe("false", S_CONST, TBOOL, 0)

	// pass 1: function signatures and constants, then globals
	tp = 0
	for tk[tp] != T_EOF {
		errline = tline[tp]
		if accept(';') {
			continue
		}
		if accept(T_PACKAGE) || accept(T_IMPORT) {
			skipdecl()
		} else if accept(T_FUNC) {
			funcsig()
			accept(';')
		} else if accept(T_CONST) {
			constdecl()
		} else if accept(T_VAR) {
			vartoks = append(vartoks, tp)
			skipdecl()
		} else if accept(T_TYPE) {
			die("type declarations are not supported")
		} else {
			die("syntax error at top level")
		}
	}
	for _, t := range vartoks {
		tp = t
		vardecl1()
	}
	database = 1024 + (globalsize+7)&^7

	// wasm function indices: imports first
	nimports := 0
	for f := 0; f < len(fname); f++ {
		if fbody[f] < 0 {
			fwasm[f] = nimports
			nimports++
		}
	}
	nfuncs := nimports
	for f := 0; f < len(fname); f++ {
		if fbody[f] >= 0 {
			fwasm[f] = nfuncs
			nfuncs++
		}
	}
	mainf := -1
	for f := 0; f < len(fname); f++ {
		if same(fname[f], fnlen[f], "main") {
			mainf = f
		}
	}
	if mainf < 0 {
		die("no main function")
	}

	// pass 2: the bodies
	for f := 0; f < len(fname); f++ {
		if fbody[f] >= 0 {
			compilefunc(f)
		}
	}
	// the init function (globals' initializers) calls main: it is _start
	nloc = 0
	ltypes = ltypes[:0]
	fb = fb[:0]
	for i := 0; i < len(initlist); i++ {
		gen(initexpr[i])
		store(node(E_GLOBAL, stype[initlist[i]], sval[initlist[i]], -1, -1, -1, 0))
	}
	callf(mainf)
	emit(0x0b)
	finishfunc(0)

	// the module
	heap := (database + len(dataseg) + 15) &^ 15
	oemit(0)
	oemit(0x61)
	oemit(0x73)
	oemit(0x6d)
	oemit(1)
	oemit(0)
	oemit(0)
	oemit(0)
	section(1) // types: one per function, then _start's
	ouleb(len(fname) + 1)
	for f := 0; f < len(fname); f++ {
		oemit(0x60)
		emitvaltypes(fpstart[f], fnp[f], ptypes)
		emitvaltypes(frstart[f], fnr[f], rtypes)
	}
	oemit(0x60)
	oemit(0)
	oemit(0)
	endsection()
	section(2) // imports
	ouleb(nimports)
	for f := 0; f < len(fname); f++ {
		if fbody[f] < 0 {
			oname("wasi_snapshot_preview1")
			onamesrc(fname[f], fnlen[f])
			oemit(0)
			ouleb(f)
		}
	}
	endsection()
	section(3) // functions
	ouleb(ndefined)
	for f := 0; f < len(fname); f++ {
		if fbody[f] >= 0 {
			ouleb(f)
		}
	}
	ouleb(len(fname))
	endsection()
	section(5) // memory
	ouleb(1)
	oemit(0)
	ouleb(heap/65536 + 1)
	endsection()
	section(6) // globals: the heap's start
	ouleb(1)
	oemit(I32)
	oemit(0)
	oemit(0x41)
	n := heap
	for { // sleb
		b := n & 127
		n = n >> 7
		if n == 0 && b&64 == 0 {
			oemit(b)
			break
		}
		oemit(b | 128)
	}
	oemit(0x0b)
	endsection()
	section(7) // exports
	ouleb(2)
	oname("memory")
	oemit(2)
	oemit(0)
	oname("_start")
	oemit(0)
	ouleb(nfuncs)
	endsection()
	section(10) // code
	ouleb(ndefined)
	out = append(out, code...)
	endsection()
	section(11) // data
	ouleb(1)
	oemit(0)
	oemit(0x41)
	n = database
	for {
		b := n & 127
		n = n >> 7
		if n == 0 && b&64 == 0 {
			oemit(b)
			break
		}
		oemit(b | 128)
	}
	oemit(0x0b)
	ouleb(len(dataseg))
	out = append(out, dataseg...)
	endsection()
	section(0) // names, for backtraces
	oname("name")
	oemit(1)
	sub := len(out)
	ouleb(len(fname) + 1)
	for f := 0; f < len(fname); f++ {
		if fbody[f] < 0 {
			ouleb(fwasm[f])
			onamesrc(fname[f], fnlen[f])
		}
	}
	for f := 0; f < len(fname); f++ {
		if fbody[f] >= 0 {
			ouleb(fwasm[f])
			onamesrc(fname[f], fnlen[f])
		}
	}
	ouleb(nfuncs)
	oname("_start")
	outer := secstart
	secstart = sub
	endsection()
	secstart = outer
	endsection()
	writeOut(out)
}
