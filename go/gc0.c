/* gc0.c -- the bootstrap Go compiler: gc.go, transcribed into cc's C.
 *
 * It exists only to compile gc.go the first time, so that the chain from
 * the seed needs no outside Go toolchain: cc compiles gc0.c, gc0 compiles
 * gc.go, and from then on gc compiles itself.  Kept as close to gc.go as
 * C allows (same functions, same order, same output bytes); Go's slices
 * become fixed arrays with counts, its strings NUL-terminated chars, and
 * its 64-bit int long where values can be large.
 */

enum {
  MAXSRC = 4194304, MAXTOK = 600000, MAXSTRS = 1048576, MAXSYM = 100000,
  MAXFN = 20000, MAXP = 100000, MAXNODE = 1500000, MAXLOC = 200000,
  MAXFB = 4194304, MAXCODE = 8388608, MAXOUT = 16777216, MAXDATA = 1048576,
  MAXVAR = 20000
};

void proc_exit(int code);

/* ------------------------------------------------------------- utilities */

int errline;

int cstrlen(char *s) { int n = 0; while (s[n]) n++; return n; }
void eputs(char *s) { sys_write(2, s, cstrlen(s)); }

char *itoa(long n) {
  char *b = malloc(24); int i = 23; int neg = n < 0;
  if (neg) n = -n;
  b[i] = 0;
  do { b[--i] = '0' + (int)(n % 10); n = n / 10; } while (n);
  if (neg) b[--i] = '-';
  return b + i;
}
char *cat(char *a, char *b) {
  int n = cstrlen(a); int m = cstrlen(b); char *r = malloc(n + m + 1); int i;
  for (i = 0; i < n; i++) r[i] = a[i];
  for (i = 0; i < m; i++) r[n + i] = b[i];
  r[n + m] = 0;
  return r;
}
void die(char *msg) {
  eputs("gc: line "); eputs(itoa(errline)); eputs(": "); eputs(msg); eputs("\n");
  proc_exit(1);
}
char *charstr(int c) { char *b = malloc(2); b[0] = c; b[1] = 0; return b; }

/* ----------------------------------------------------------------- lexer */

enum {
  T_EOF = 256, T_NUM, T_STR, T_ID,
  T_BREAK, T_CASE, T_CONST, T_CONTINUE, T_DEFAULT, T_ELSE, T_FOR, T_FUNC,
  T_IF, T_IMPORT, T_PACKAGE, T_RANGE, T_RETURN, T_SWITCH, T_TYPE, T_VAR,
  T_EQ, T_NE, T_LE, T_GE, T_ANDAND, T_OROR, T_INC, T_DEC, T_SHL, T_SHR,
  T_ANDNOT, T_ADDA, T_SUBA, T_MULA, T_DIVA, T_MODA, T_ANDA, T_ORA, T_XORA,
  T_DEFINE, T_SHLA, T_SHRA, T_ANDNOTA, T_ELLIPSIS
};

char *keywords = "break case const continue default else for func if import package range return switch type var ";
char *ops2 = "==!=<=>=&&||++--<<>>&^+=-=*=/=%=&=|=^=:=";

char src[MAXSRC]; int srclen;
int tk[MAXTOK]; long tv[MAXTOK]; int tl[MAXTOK]; int tline[MAXTOK]; int ntok;
int tp;
char strs[MAXSTRS]; int nstrs;

int isdigit_(int c) { return c >= '0' && c <= '9'; }
int isletter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

int hexval(int c) {
  if (isdigit_(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int at(int i) { if (i < srclen) return src[i] & 255; return 0; }

int same(long p, int n, char *s) {
  int i;
  if (n != cstrlen(s)) return 0;
  for (i = 0; i < n; i++) if (src[p + i] != s[i]) return 0;
  return 1;
}

int keyword(int p, int n) {
  int t = T_BREAK; int i = 0; int j; int k; int len = cstrlen(keywords);
  while (i < len) {
    j = i;
    while (keywords[j] != ' ') j++;
    if (j - i == n) {
      k = 0;
      while (k < n && src[p + k] == keywords[i + k]) k++;
      if (k == n) return t;
    }
    i = j + 1;
    t++;
  }
  return T_ID;
}

void addtok(int kind, long v, int l, int line) {
  if (ntok >= MAXTOK) die("too many tokens");
  tk[ntok] = kind; tv[ntok] = v; tl[ntok] = l; tline[ntok] = line; ntok++;
}

int endsline(int t) {
  return t == T_ID || t == T_NUM || t == T_STR || t == T_BREAK || t == T_CONTINUE ||
    t == T_RETURN || t == T_INC || t == T_DEC || t == ')' || t == ']' || t == '}';
}

int escend;
int charat(int i) {
  int c = at(i);
  if (c != '\\') { escend = i + 1; return c; }
  c = at(i + 1);
  escend = i + 2;
  if (c == 'n') return 10;
  if (c == 't') return 9;
  if (c == 'r') return 13;
  if (c == '0') return 0;
  if (c == 'x') { escend = i + 4; return hexval(at(i + 2)) * 16 + hexval(at(i + 3)); }
  return c;
}

void addstr(int c) { if (nstrs >= MAXSTRS) die("too many strings"); strs[nstrs++] = c; }

void lex() {
  int i = 0; int line = 1; int c; long v; int j; int start; int k; int len2 = cstrlen(ops2);
  while (1) {
    c = at(i);
    if (i >= srclen) break;
    errline = line;
    if (c == '\n') {
      if (ntok > 0 && endsline(tk[ntok - 1])) addtok(';', 0, 0, line);
      line++; i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
    if (c == '/' && at(i + 1) == '/') { while (i < srclen && src[i] != '\n') i++; continue; }
    if (c == '/' && at(i + 1) == '*') {
      i = i + 2;
      while (i < srclen && !(src[i] == '*' && at(i + 1) == '/')) { if (src[i] == '\n') line++; i++; }
      i = i + 2;
      continue;
    }
    if (isdigit_(c)) {
      v = 0;
      if (c == '0' && (at(i + 1) == 'x' || at(i + 1) == 'X')) {
        i = i + 2;
        while (hexval(at(i)) >= 0 || at(i) == '_') { if (at(i) != '_') v = v * 16 + hexval(at(i)); i++; }
      } else {
        while (isdigit_(at(i)) || at(i) == '_') { if (at(i) != '_') v = v * 10 + at(i) - '0'; i++; }
      }
      if (at(i) == '.' || at(i) == 'e') die("floating-point numbers are not supported");
      addtok(T_NUM, v, 0, line);
      continue;
    }
    if (isletter(c)) {
      j = i;
      while (isletter(at(i)) || isdigit_(at(i))) i++;
      addtok(keyword(j, i - j), j, i - j, line);
      continue;
    }
    if (c == '\'') {
      v = charat(i + 1);
      i = escend + 1;
      addtok(T_NUM, v, 0, line);
      continue;
    }
    if (c == '"') {
      start = nstrs;
      i++;
      while (at(i) != '"') {
        if (i >= srclen || at(i) == '\n') die("unterminated string");
        addstr(charat(i));
        i = escend;
      }
      i++;
      addtok(T_STR, start, nstrs - start, line);
      continue;
    }
    if (c == '`') {
      start = nstrs;
      i++;
      while (at(i) != '`') {
        if (i >= srclen) die("unterminated string");
        if (src[i] == '\n') line++;
        if (src[i] != '\r') addstr(src[i]);
        i++;
      }
      i++;
      addtok(T_STR, start, nstrs - start, line);
      continue;
    }
    if (c == '.' && at(i + 1) == '.' && at(i + 2) == '.') { addtok(T_ELLIPSIS, 0, 0, line); i = i + 3; continue; }
    if ((c == '<' || c == '>' || c == '&') && at(i + 2) == '=' &&
        ((at(i + 1) == c && c != '&') || (c == '&' && at(i + 1) == '^'))) {
      if (c == '<') addtok(T_SHLA, 0, 0, line);
      else if (c == '>') addtok(T_SHRA, 0, 0, line);
      else addtok(T_ANDNOTA, 0, 0, line);
      i = i + 3;
      continue;
    }
    k = 0;
    while (k < len2 && !(ops2[k] == c && ops2[k + 1] == at(i + 1))) k = k + 2;
    if (k < len2) { addtok(T_EQ + k / 2, 0, 0, line); i = i + 2; continue; }
    addtok(c, 0, 0, line);
    i++;
  }
  if (ntok > 0 && endsline(tk[ntok - 1])) addtok(';', 0, 0, line);
  addtok(T_EOF, 0, 0, line);
}

int accept(int t) { if (tk[tp] == t) { tp++; return 1; } return 0; }
void expect(int t) {
  errline = tline[tp];
  if (!accept(t)) {
    if (t < 256) die(cat("syntax error: expected ", charstr(t)));
    die("syntax error");
  }
}

/* ----------------------------------------------------------------- types */

enum { TVOID = 0, TINT = 1, TI32 = 2, TBYTE = 3, TBOOL = 4, TSTR = 5, TUNTYPED = 6, TTUPLE = 7, TSLICE = 16 };
enum { I32 = 127, I64 = 126 };

int telem[1000]; int ntelem;
int slicetype(int elem) {
  int i;
  for (i = 0; i < ntelem; i++) if (telem[i] == elem) return TSLICE + i;
  telem[ntelem++] = elem;
  return TSLICE + ntelem - 1;
}
int isslice(int t) { return t >= TSLICE; }
int elemtype(int t) { return telem[t - TSLICE]; }
int isint(int t) { return t == TINT || t == TI32 || t == TBYTE || t == TUNTYPED; }
int words(int t) {
  if (t == TVOID) return 0;
  if (t == TSTR) return 2;
  if (isslice(t)) return 3;
  return 1;
}
int wtype(int t) { if (t == TINT || t == TUNTYPED) return I64; return I32; }
int msize(int t) {
  if (t == TINT) return 8;
  if (t == TI32) return 4;
  if (t == TBYTE || t == TBOOL) return 1;
  if (t == TSTR) return 8;
  return 12;
}
char *typename(int t) {
  if (t == TINT) return "int";
  if (t == TI32) return "int32";
  if (t == TBYTE) return "byte";
  if (t == TBOOL) return "bool";
  if (t == TSTR) return "string";
  if (t == TUNTYPED) return "untyped int";
  if (isslice(t)) return cat("[]", typename(elemtype(t)));
  return "void";
}

/* --------------------------------------------------------------- symbols */

enum { S_LOCAL = 1, S_GLOBAL, S_CONST, S_FUNC, S_TYPE };

int sname[MAXSYM]; int slen[MAXSYM]; int skind[MAXSYM]; int stype[MAXSYM]; long sval[MAXSYM];
int nsym;

int samename(int a, int b, int n) {
  int i;
  for (i = 0; i < n; i++) if (src[a + i] != src[b + i]) return 0;
  return 1;
}
int lookup(int p, int n) {
  int i;
  for (i = nsym - 1; i >= 0; i--) if (slen[i] == n && samename(sname[i], p, n)) return i;
  return -1;
}
int addsym(int p, int n, int kind, int t, long v) {
  if (nsym >= MAXSYM) die("too many symbols");
  sname[nsym] = p; slen[nsym] = n; skind[nsym] = kind; stype[nsym] = t; sval[nsym] = v;
  nsym++;
  return nsym - 1;
}
void universe(char *name, int kind, int t, long v) {
  int p = srclen; int i; int n = cstrlen(name);
  for (i = 0; i < n; i++) src[srclen++] = name[i];
  addsym(p, n, kind, t, v);
}

int fname[MAXFN]; int fnlen[MAXFN]; int fpstart[MAXFN]; int fnp[MAXFN];
int frstart[MAXFN]; int fnr[MAXFN]; int fbody[MAXFN]; int fwasm[MAXFN]; int nfn;
int ptypes[MAXP]; int pnames[MAXP]; int nptypes;
int rtypes[MAXP]; int nrtypes;

int functype(int f) {
  if (fnr[f] == 0) return TVOID;
  if (fnr[f] == 1) return rtypes[frstart[f]];
  return TTUPLE;
}
int findfunc(char *name) {
  int f;
  for (f = 0; f < nfn; f++) if (same(fname[f], fnlen[f], name)) return f;
  die(cat("the runtime lacks ", name));
  return -1;
}

/* ------------------------------------------------------------ the tree */

enum {
  E_CONST = 1, E_STR, E_LOCAL, E_GLOBAL, E_BIN, E_UN, E_CALL, E_INDEX, E_SLICE,
  E_LEN, E_CAP, E_APPEND, E_MAKE, E_CONV, E_COPY, E_INTRIN, E_PRINT, E_PANIC,
  E_BLANK, L_LIST, S_BLOCK, S_EXPR, S_ASSIGN, S_OPASSIGN, S_IF, S_FOR, S_RANGE,
  S_SWITCH, S_CASE, S_RETURN, S_BREAK, S_CONTINUE, S_ZERO
};

int nk[MAXNODE]; int nt[MAXNODE]; int na[MAXNODE]; int nb[MAXNODE]; int nc[MAXNODE];
int nd[MAXNODE]; long nv[MAXNODE]; int nline[MAXNODE]; int nnode;

int node(int kind, int t, int a, int b, int c, int d, long v) {
  if (nnode >= MAXNODE) die("too many nodes");
  nk[nnode] = kind; nt[nnode] = t; na[nnode] = a; nb[nnode] = b; nc[nnode] = c;
  nd[nnode] = d; nv[nnode] = v; nline[nnode] = errline;
  nnode++;
  return nnode - 1;
}
int cons(int item, int next) { return node(L_LIST, 0, item, next, -1, -1, 0); }
int listlen(int l) { int n = 0; while (l >= 0) { n++; l = nb[l]; } return n; }
int constnode(long v, int t) { return node(E_CONST, t, -1, -1, -1, -1, v); }

void convert(int n, int t) {
  long v;
  errline = nline[n];
  if (nt[n] == t) return;
  if (nt[n] == TUNTYPED && isint(t)) {
    v = nv[n];
    if ((t == TBYTE && (v < 0 || v > 255)) || (t == TI32 && (v < -2147483648L || v > 2147483647L)))
      die(cat(cat(cat("constant ", itoa(v)), " overflows "), typename(t)));
    nt[n] = t;
    return;
  }
  die(cat(cat(cat("type mismatch: ", typename(nt[n])), " used as "), typename(t)));
}
void settle(int n) { if (nt[n] == TUNTYPED) nt[n] = TINT; }

/* ------------------------------------------------------------ expressions */

int nloc; int ltypes[MAXLOC];
int newlocal(int t) { if (nloc >= MAXLOC) die("too many locals"); ltypes[nloc] = t; nloc++; return nloc - 1; }

int declare(int p, int n, int t) {
  int s = addsym(p, n, S_LOCAL, t, nloc); int i;
  for (i = 0; i < words(t); i++) { if (i == 0) newlocal(wtype(t)); else newlocal(I32); }
  return s;
}

int parseexpr();

int parsetype() {
  int s;
  errline = tline[tp];
  if (accept('[')) { expect(']'); return slicetype(parsetype()); }
  if (tk[tp] == T_ID) {
    s = lookup(tv[tp], tl[tp]);
    if (s >= 0 && skind[s] == S_TYPE) { tp++; return stype[s]; }
  }
  die("expected a type");
  return 0;
}
int istypestart() {
  int s;
  if (tk[tp] == '[' && tk[tp + 1] == ']') return 1;
  if (tk[tp] == T_ID) { s = lookup(tv[tp], tl[tp]); return s >= 0 && skind[s] == S_TYPE; }
  return 0;
}

int parseargs() {
  int head = -1; int tail = -1; int e; int c;
  expect('(');
  while (tk[tp] != ')') {
    e = parseexpr();
    c = cons(e, -1);
    if (tail < 0) head = c; else nb[tail] = c;
    tail = c;
    if (tk[tp] == T_ELLIPSIS) break;
    if (!accept(',')) break;
  }
  return head;
}

int builtin(int p, int n) {
  int a; int x; int spread; int t; int l; int c; int v; int w;
  errline = tline[tp];
  if (same(p, n, "len") || same(p, n, "cap")) {
    a = parseargs(); expect(')');
    if (listlen(a) != 1) die("len and cap take one argument");
    x = na[a];
    if (!isslice(nt[x]) && !(nt[x] == TSTR && same(p, n, "len"))) die("invalid argument to len or cap");
    if (same(p, n, "len")) return node(E_LEN, TINT, x, -1, -1, -1, 0);
    return node(E_CAP, TINT, x, -1, -1, -1, 0);
  }
  if (same(p, n, "append")) {
    a = parseargs();
    spread = 0;
    if (accept(T_ELLIPSIS)) spread = 1;
    accept(',');
    expect(')');
    if (a < 0 || !isslice(nt[na[a]])) die("first argument to append must be a slice");
    t = nt[na[a]];
    for (l = nb[a]; l >= 0; l = nb[l]) {
      if (spread == 1) {
        if (!(nt[na[l]] == t || (nt[na[l]] == TSTR && elemtype(t) == TBYTE))) die("bad argument to append");
      } else convert(na[l], elemtype(t));
    }
    return node(E_APPEND, t, na[a], nb[a], spread, -1, 0);
  }
  if (same(p, n, "make")) {
    expect('(');
    t = parsetype();
    if (!isslice(t)) die("make of a non-slice");
    expect(',');
    l = parseexpr(); convert(l, TINT);
    c = -1;
    if (accept(',')) { c = parseexpr(); convert(c, TINT); }
    expect(')');
    return node(E_MAKE, t, l, c, -1, -1, 0);
  }
  if (same(p, n, "copy")) {
    a = parseargs(); expect(')');
    if (listlen(a) != 2 || !isslice(nt[na[a]])) die("bad arguments to copy");
    return node(E_COPY, TINT, na[a], na[nb[a]], -1, -1, 0);
  }
  if (same(p, n, "print") || same(p, n, "println")) {
    a = parseargs(); expect(')');
    for (l = a; l >= 0; l = nb[l]) settle(na[l]);
    v = 0;
    if (same(p, n, "println")) v = 1;
    return node(E_PRINT, TVOID, -1, a, -1, -1, v);
  }
  if (same(p, n, "panic")) {
    a = parseargs(); expect(')');
    if (listlen(a) != 1 || nt[na[a]] != TSTR) die("panic takes a string");
    return node(E_PANIC, TVOID, na[a], -1, -1, -1, 0);
  }
  w = -1; t = TI32;
  if (same(p, n, "__heap_base")) w = 1;
  if (same(p, n, "__memory_size")) w = 2;
  if (same(p, n, "__memory_grow")) w = 3;
  if (same(p, n, "__load32")) w = 4;
  if (same(p, n, "__store32")) { w = 5; t = TVOID; }
  if (same(p, n, "__memcopy")) { w = 6; t = TVOID; }
  if (same(p, n, "__sptr") || same(p, n, "__bptr")) w = 7;
  if (same(p, n, "__mkstring")) { w = 8; t = TSTR; }
  if (w < 0) return -1;
  a = parseargs(); expect(')');
  for (l = a; l >= 0; l = nb[l]) if (w != 7) convert(na[l], TI32);
  return node(E_INTRIN, t, -1, a, -1, -1, w);
}

int parsecall(int f) {
  int a = parseargs(); int i; int l;
  expect(')');
  errline = tline[tp];
  if (listlen(a) != fnp[f]) die("wrong number of arguments");
  i = 0;
  for (l = a; l >= 0; l = nb[l]) { convert(na[l], ptypes[fpstart[f] + i]); i++; }
  return node(E_CALL, functype(f), f, a, -1, -1, 0);
}

int conversion(int to, int e);

char *srcstr(int p, int n) {
  char *b = malloc(n + 1); int i;
  for (i = 0; i < n; i++) b[i] = src[p + i];
  b[n] = 0;
  return b;
}

int primary() {
  int t; int e; int to; int p; int n; int s; int b; int k;
  errline = tline[tp];
  t = tk[tp];
  if (t == T_NUM) { tp++; return constnode(tv[tp - 1], TUNTYPED); }
  if (t == T_STR) { tp++; return node(E_STR, TSTR, tv[tp - 1], tl[tp - 1], -1, -1, 0); }
  if (t == '(') { tp++; e = parseexpr(); expect(')'); return e; }
  if (istypestart()) {
    to = parsetype();
    expect('(');
    e = parseexpr();
    expect(')');
    return conversion(to, e);
  }
  if (t != T_ID) die("expected an expression");
  p = tv[tp]; n = tl[tp];
  tp++;
  if (same(p, n, "_")) return node(E_BLANK, TVOID, -1, -1, -1, -1, 0);
  s = lookup(p, n);
  if (s < 0) {
    if (tk[tp] == '(') {
      b = builtin(p, n);
      if (b >= 0) return b;
    }
    die(cat("undefined: ", srcstr(p, n)));
  }
  k = skind[s];
  if (k == S_FUNC) {
    if (tk[tp] != '(') die("functions as values are not supported");
    return parsecall(sval[s]);
  }
  if (k == S_CONST) {
    if (stype[s] == TSTR) return node(E_STR, TSTR, (int)(sval[s] & 4294967295L), (int)(sval[s] >> 32), -1, -1, 0);
    return constnode(sval[s], stype[s]);
  }
  if (k == S_LOCAL) return node(E_LOCAL, stype[s], sval[s], -1, -1, -1, 0);
  if (k == S_GLOBAL) return node(E_GLOBAL, stype[s], sval[s], -1, -1, -1, 0);
  die("unexpected type");
  return -1;
}

int conversion(int to, int e) {
  int f = nt[e]; long v;
  if (isint(to) && f == TUNTYPED) { convert(e, to); return e; }
  if (isint(to) && isint(f)) {
    if (nk[e] == E_CONST) {
      v = nv[e];
      if (to == TBYTE) v = v & 255;
      else if (to == TI32) v = ((v & 4294967295L) ^ 2147483648L) - 2147483648L;
      return constnode(v, to);
    }
    return node(E_CONV, to, e, -1, -1, -1, 0);
  }
  if ((to == TSTR && isslice(f) && elemtype(f) == TBYTE) || (to == slicetype(TBYTE) && f == TSTR))
    return node(E_CONV, to, e, -1, -1, -1, 0);
  if (to == f) return e;
  die(cat(cat(cat("cannot convert ", typename(f)), " to "), typename(to)));
  return -1;
}

int postfix() {
  int e = primary(); int t; int lo; int hi; int et;
  while (1) {
    errline = tline[tp];
    if (accept('[')) {
      t = nt[e];
      if (!isslice(t) && t != TSTR) die("index of a non-slice");
      lo = -1;
      if (tk[tp] != ':') {
        lo = parseexpr();
        settle(lo);
        if (!isint(nt[lo])) die("non-integer index");
      }
      if (accept(':')) {
        hi = -1;
        if (tk[tp] != ']') { hi = parseexpr(); convert(hi, TINT); }
        if (lo >= 0) convert(lo, TINT);
        expect(']');
        e = node(E_SLICE, t, e, lo, hi, -1, 0);
        continue;
      }
      expect(']');
      et = TBYTE;
      if (isslice(t)) et = elemtype(t);
      e = node(E_INDEX, et, e, lo, -1, -1, 0);
      continue;
    }
    return e;
  }
  return e;
}

int unary() {
  int t; int e; long v;
  errline = tline[tp];
  t = tk[tp];
  if (t == '-' || t == '+' || t == '!' || t == '^') {
    tp++;
    e = unary();
    if (t == '!') { if (nt[e] != TBOOL) die("! of a non-bool"); }
    else if (!isint(nt[e])) die("arithmetic on a non-integer");
    if (nk[e] == E_CONST) {
      v = nv[e];
      if (t == '-') v = -v;
      else if (t == '^') v = ~v;
      else if (t == '!') v = 1 - v;
      if (nt[e] == TBYTE) v = v & 255;
      return constnode(v, nt[e]);
    }
    if (t == '+') return e;
    return node(E_UN, nt[e], e, -1, -1, -1, t);
  }
  return postfix();
}

int prec(int t) {
  if (t == '*' || t == '/' || t == '%' || t == T_SHL || t == T_SHR || t == '&' || t == T_ANDNOT) return 5;
  if (t == '+' || t == '-' || t == '|' || t == '^') return 4;
  if (t == T_EQ || t == T_NE || t == '<' || t == T_LE || t == '>' || t == T_GE) return 3;
  if (t == T_ANDAND) return 2;
  if (t == T_OROR) return 1;
  return 0;
}
int iscmp(int op) { return op == T_EQ || op == T_NE || op == '<' || op == T_LE || op == '>' || op == T_GE; }

long fold(int op, long x, long y) {
  int r = 0;
  if (op == '+') return x + y;
  if (op == '-') return x - y;
  if (op == '*') return x * y;
  if (op == '/' || op == '%') {
    if (y == 0) die("division by zero");
    if (op == '/') return x / y;
    return x % y;
  }
  if (op == T_SHL) return x << y;
  if (op == T_SHR) return x >> y;
  if (op == '&') return x & y;
  if (op == '|') return x | y;
  if (op == '^') return x ^ y;
  if (op == T_ANDNOT) return x & ~y;
  if (op == T_EQ) r = x == y;
  else if (op == T_NE) r = x != y;
  else if (op == '<') r = x < y;
  else if (op == T_LE) r = x <= y;
  else if (op == '>') r = x > y;
  else if (op == T_GE) r = x >= y;
  else if (op == T_ANDAND) r = x != 0 && y != 0;
  else if (op == T_OROR) r = x != 0 || y != 0;
  if (r) return 1;
  return 0;
}

int binary(int op, int l, int r) {
  int t; int rt; long v;
  errline = nline[l];
  if (op == T_ANDAND || op == T_OROR) {
    if (nt[l] != TBOOL || nt[r] != TBOOL) die("&& or || of a non-bool");
    if (nk[l] == E_CONST && nk[r] == E_CONST) return constnode(fold(op, nv[l], nv[r]), TBOOL);
    return node(E_BIN, TBOOL, l, r, -1, -1, op);
  }
  if (op == T_SHL || op == T_SHR) {
    if (!isint(nt[l]) || !isint(nt[r])) die("shift of a non-integer");
    if (nk[l] == E_CONST && nk[r] == E_CONST) return constnode(fold(op, nv[l], nv[r]), nt[l]);
    settle(l);
    if (nt[r] == TUNTYPED) convert(r, nt[l]);
    return node(E_BIN, nt[l], l, r, -1, -1, op);
  }
  if (nt[l] == TUNTYPED && nt[r] != TUNTYPED) convert(l, nt[r]);
  if (nt[r] == TUNTYPED && nt[l] != TUNTYPED) convert(r, nt[l]);
  t = nt[l];
  if (nt[r] != t) die(cat(cat(cat("mismatched types ", typename(t)), " and "), typename(nt[r])));
  rt = t;
  if (iscmp(op)) {
    rt = TBOOL;
    if (isslice(t)) die("slices can't be compared");
    if (t == TBOOL && op != T_EQ && op != T_NE) die("bools are only == or !=");
  } else if (t == TSTR) {
    if (op != '+') die("invalid string operation");
  } else if (!isint(t)) die(cat("arithmetic on ", typename(t)));
  if (nk[l] == E_CONST && nk[r] == E_CONST) {
    v = fold(op, nv[l], nv[r]);
    if (t == TBYTE && !iscmp(op)) v = v & 255;
    return constnode(v, rt);
  }
  return node(E_BIN, rt, l, r, -1, -1, op);
}

int binexpr(int minprec) {
  int l = unary(); int op; int p; int r;
  while (1) {
    op = tk[tp];
    p = prec(op);
    if (p == 0 || p < minprec) return l;
    tp++;
    r = binexpr(p + 1);
    l = binary(op, l, r);
  }
  return l;
}
int parseexpr() { return binexpr(1); }

/* ------------------------------------------------------------ statements */

int isaddressable(int e) {
  int k = nk[e];
  return k == E_LOCAL || k == E_GLOBAL || (k == E_INDEX && nt[na[e]] != TSTR);
}

int isdefine() {
  int i = tp;
  while (tk[i] == T_ID) {
    i++;
    if (tk[i] == T_DEFINE) return 1;
    if (tk[i] != ',') return 0;
    i++;
  }
  return 0;
}

int parseexprlist() {
  int head = -1; int tail = -1; int e; int c;
  while (1) {
    e = parseexpr();
    c = cons(e, -1);
    if (tail < 0) head = c; else nb[tail] = c;
    tail = c;
    if (!accept(',')) return head;
  }
  return head;
}

int scopestart;

int definenames(int start, int nnames, int rhs);
int define() {
  int start; int nnames = 0; int rhs;
  errline = tline[tp];
  start = tp;
  while (tk[tp] == T_ID) {
    nnames++;
    tp++;
    if (!accept(',')) break;
  }
  expect(T_DEFINE);
  rhs = parseexprlist();
  return definenames(start, nnames, rhs);
}

int definenames(int start, int nnames, int rhs) {
  int *types = (int *)malloc(4 * nnames + 4); int ntypes = 0; int f; int i; int l;
  int head = -1; int tail = -1; int anynew = 0; int t; int s; int c;
  if (listlen(rhs) == 1 && nt[na[rhs]] == TTUPLE) {
    f = na[na[rhs]];
    if (fnr[f] != nnames) die("assignment mismatch");
    for (i = 0; i < nnames; i++) types[ntypes++] = rtypes[frstart[f] + i];
  } else {
    if (listlen(rhs) != nnames) die("assignment mismatch");
    for (l = rhs; l >= 0; l = nb[l]) {
      settle(na[l]);
      if (nt[na[l]] == TVOID || nt[na[l]] == TTUPLE) die("no value");
      types[ntypes++] = nt[na[l]];
    }
  }
  for (i = 0; i < nnames; i++) {
    t = start + 2 * i;
    s = lookup(tv[t], tl[t]);
    if (same(tv[t], tl[t], "_")) s = declare(tv[t], tl[t], types[i]);
    else if (s < scopestart || skind[s] != S_LOCAL) { s = declare(tv[t], tl[t], types[i]); anynew = 1; }
    else if (stype[s] != types[i]) die("type mismatch in :=");
    c = cons(node(E_LOCAL, stype[s], sval[s], -1, -1, -1, 0), -1);
    if (tail < 0) head = c; else nb[tail] = c;
    tail = c;
  }
  if (!anynew && nnames > 0 && !same(tv[start], tl[start], "_")) die("no new variables on left side of :=");
  return node(S_ASSIGN, 0, head, rhs, -1, -1, 0);
}

int assignop(int t) {
  char *ops = "+-*/%&|^";
  if (t >= T_ADDA && t <= T_XORA) return ops[t - T_ADDA];
  if (t == T_SHLA) return T_SHL;
  if (t == T_SHRA) return T_SHR;
  if (t == T_ANDNOTA) return T_ANDNOT;
  return 0;
}

int checkassign(int lhs, int rhs);
int simplestmt() {
  int lhs; int t; int rhs; int r; int l; int b; int op; int e; int k;
  errline = tline[tp];
  if (isdefine()) return define();
  lhs = parseexprlist();
  t = tk[tp];
  if (t == '=') { tp++; rhs = parseexprlist(); return checkassign(lhs, rhs); }
  if (assignop(t) != 0) {
    tp++;
    if (listlen(lhs) != 1) die("bad assignment");
    r = parseexpr();
    l = na[lhs];
    if (!isaddressable(l)) die("cannot assign");
    b = binary(assignop(t), l, r);
    if (nk[b] != E_BIN) die("cannot assign");
    return node(S_OPASSIGN, 0, l, r, -1, -1, assignop(t));
  }
  if (t == T_INC || t == T_DEC) {
    tp++;
    l = na[lhs];
    if (!isaddressable(l) || !isint(nt[l])) die("bad ++ or --");
    op = '+';
    if (t == T_DEC) op = '-';
    return node(S_OPASSIGN, 0, l, constnode(1, nt[l]), -1, -1, op);
  }
  if (listlen(lhs) != 1) die("syntax error");
  e = na[lhs];
  k = nk[e];
  if (k != E_CALL && k != E_PRINT && k != E_PANIC && k != E_INTRIN && k != E_COPY) die("expression is not used");
  return node(S_EXPR, 0, e, -1, -1, -1, 0);
}

int isblank(int e) { return nk[e] == E_BLANK; }

int checkassign(int lhs, int rhs) {
  int n = listlen(lhs); int f; int i; int l; int r;
  if (listlen(rhs) == 1 && nt[na[rhs]] == TTUPLE) {
    f = na[na[rhs]];
    if (fnr[f] != n) die("assignment mismatch");
    i = 0;
    for (l = lhs; l >= 0; l = nb[l]) {
      if (!isblank(na[l]) && (!isaddressable(na[l]) || nt[na[l]] != rtypes[frstart[f] + i])) die("bad assignment");
      i++;
    }
    return node(S_ASSIGN, 0, lhs, rhs, -1, -1, 0);
  }
  if (listlen(rhs) != n) die("assignment mismatch");
  r = rhs;
  for (l = lhs; l >= 0; l = nb[l]) {
    if (isblank(na[l])) { settle(na[r]); nt[na[l]] = nt[na[r]]; }
    else if (!isaddressable(na[l])) die("cannot assign");
    else convert(na[r], nt[na[l]]);
    r = nb[r];
  }
  return node(S_ASSIGN, 0, lhs, rhs, -1, -1, 0);
}

int curfunc;

int stmt();
int block() {
  int save; int mark; int head = -1; int tail = -1; int s; int c;
  expect('{');
  save = scopestart; mark = nsym; scopestart = nsym;
  while (!accept('}')) {
    s = stmt();
    if (s >= 0) {
      c = cons(s, -1);
      if (tail < 0) head = c; else nb[tail] = c;
      tail = c;
    }
  }
  nsym = mark; scopestart = save;
  return node(S_BLOCK, 0, head, -1, -1, -1, 0);
}

void endstmt() { if (tk[tp] != '}') expect(';'); }

int vardecl(); int ifstmt(); int forstmt(); int switchstmt();

int stmt() {
  int t; int b; int s; int r; int f; int g; int i; int l;
  errline = tline[tp];
  t = tk[tp];
  if (accept(';')) return -1;
  if (t == '{') { b = block(); endstmt(); return b; }
  if (accept(T_VAR)) { s = vardecl(); endstmt(); return s; }
  if (accept(T_IF)) { s = ifstmt(); endstmt(); return s; }
  if (accept(T_FOR)) { s = forstmt(); endstmt(); return s; }
  if (accept(T_SWITCH)) { s = switchstmt(); endstmt(); return s; }
  if (accept(T_RETURN)) {
    r = -1;
    if (tk[tp] != ';' && tk[tp] != '}') r = parseexprlist();
    f = curfunc;
    if (listlen(r) == 1 && nt[na[r]] == TTUPLE) {
      g = na[na[r]];
      if (fnr[g] != fnr[f]) die("wrong number of return values");
      for (i = 0; i < fnr[f]; i++)
        if (rtypes[frstart[g] + i] != rtypes[frstart[f] + i]) die("wrong return types");
    } else {
      if (listlen(r) != fnr[f]) die("wrong number of return values");
      i = 0;
      for (l = r; l >= 0; l = nb[l]) { convert(na[l], rtypes[frstart[f] + i]); i++; }
    }
    endstmt();
    return node(S_RETURN, 0, r, -1, -1, -1, 0);
  }
  if (accept(T_BREAK)) { endstmt(); return node(S_BREAK, 0, -1, -1, -1, -1, 0); }
  if (accept(T_CONTINUE)) { endstmt(); return node(S_CONTINUE, 0, -1, -1, -1, -1, 0); }
  if (t == T_CONST) die("local constants are not supported");
  s = simplestmt();
  endstmt();
  return s;
}

int vardecl() {
  int p; int n; int t; int e; int s;
  errline = tline[tp];
  if (tk[tp] != T_ID) die("expected a name");
  p = tv[tp]; n = tl[tp];
  tp++;
  t = -1;
  if (tk[tp] != '=') t = parsetype();
  if (accept('=')) {
    e = parseexpr();
    if (t >= 0) convert(e, t);
    settle(e);
    s = declare(p, n, nt[e]);
    return node(S_ASSIGN, 0, cons(node(E_LOCAL, nt[e], sval[s], -1, -1, -1, 0), -1), cons(e, -1), -1, -1, 0);
  }
  s = declare(p, n, t);
  return node(S_ZERO, t, sval[s], -1, -1, -1, 0);
}

int istokbefore(int t, int u) {
  int depth = 0; int i;
  for (i = tp; tk[i] != T_EOF; i++) {
    if (tk[i] == '(' || tk[i] == '[') depth++;
    if (tk[i] == ')' || tk[i] == ']') depth--;
    if (depth == 0 && tk[i] == u) return 0;
    if (depth == 0 && tk[i] == t) return 1;
  }
  return 0;
}

int ifstmt() {
  int save = scopestart; int mark = nsym; int init; int c; int then; int els;
  scopestart = nsym;
  init = -1;
  if (!istokbefore(';', '{')) init = -1;
  else { init = simplestmt(); expect(';'); }
  c = parseexpr();
  if (nt[c] != TBOOL) die("non-bool condition");
  then = block();
  els = -1;
  if (accept(T_ELSE)) {
    if (accept(T_IF)) els = ifstmt();
    else els = block();
  }
  nsym = mark; scopestart = save;
  return node(S_IF, 0, c, then, els, init, 0);
}

int rangeloop();
int forstmt() {
  int save = scopestart; int mark = nsym; int s; int c; int init; int post; int b;
  scopestart = nsym;
  if (tk[tp] == '{') { b = block(); s = node(S_FOR, 0, -1, -1, -1, b, 0); }
  else if (istokbefore(T_RANGE, '{')) s = rangeloop();
  else if (!istokbefore(';', '{')) {
    c = parseexpr();
    if (nt[c] != TBOOL) die("non-bool condition");
    b = block();
    s = node(S_FOR, 0, -1, c, -1, b, 0);
  } else {
    init = -1;
    if (tk[tp] != ';') init = simplestmt();
    expect(';');
    c = -1;
    if (tk[tp] != ';') {
      c = parseexpr();
      if (nt[c] != TBOOL) die("non-bool condition");
    }
    expect(';');
    post = -1;
    if (tk[tp] != '{') post = simplestmt();
    b = block();
    s = node(S_FOR, 0, init, c, post, b, 0);
  }
  nsym = mark; scopestart = save;
  return s;
}

int rangeloop() {
  int kp = -1; int vp = -1; int x; int t; int ks; int vs; int b;
  if (tk[tp] != T_RANGE) {
    kp = tp;
    tp++;
    if (accept(',')) { vp = tp; tp++; }
    expect(T_DEFINE);
  }
  expect(T_RANGE);
  x = parseexpr();
  settle(x);
  t = nt[x];
  if (!isslice(t) && t != TINT) die(cat(cat("range over ", typename(t)), " is not supported"));
  if (vp >= 0 && t == TINT) die("range over an int has one variable");
  ks = -1; vs = -1;
  if (kp >= 0) ks = sval[declare(tv[kp], tl[kp], TINT)];
  if (vp >= 0) vs = sval[declare(tv[vp], tl[vp], elemtype(t))];
  b = block();
  return node(S_RANGE, 0, ks, vs, x, b, 0);
}

int switchstmt() {
  int save = scopestart; int mark = nsym; int init; int tag; int head = -1; int tail = -1;
  int exprs; int l; int bmark; int bsave; int bhead; int btail; int s; int c;
  scopestart = nsym;
  init = -1;
  if (istokbefore(';', '{')) { init = simplestmt(); expect(';'); }
  tag = -1;
  if (tk[tp] != '{') { tag = parseexpr(); settle(tag); }
  expect('{');
  while (!accept('}')) {
    errline = tline[tp];
    exprs = -1;
    if (accept(T_DEFAULT)) exprs = -1;
    else {
      expect(T_CASE);
      exprs = parseexprlist();
      for (l = exprs; l >= 0; l = nb[l]) {
        if (tag >= 0) convert(na[l], nt[tag]);
        else if (nt[na[l]] != TBOOL) die("non-bool case");
      }
    }
    expect(':');
    bmark = nsym; bsave = scopestart; scopestart = nsym;
    bhead = -1; btail = -1;
    while (tk[tp] != T_CASE && tk[tp] != T_DEFAULT && tk[tp] != '}') {
      s = stmt();
      if (s >= 0) {
        c = cons(s, -1);
        if (btail < 0) bhead = c; else nb[btail] = c;
        btail = c;
      }
    }
    nsym = bmark; scopestart = bsave;
    c = cons(node(S_CASE, 0, exprs, bhead, -1, -1, 0), -1);
    if (tail < 0) head = c; else nb[tail] = c;
    tail = c;
  }
  nsym = mark; scopestart = save;
  return node(S_SWITCH, 0, init, tag, head, -1, 0);
}

/* ---------------------------------------------------------------- output */

char fb[MAXFB]; int nfb;

void emit(int b) { if (nfb >= MAXFB) die("function too big"); fb[nfb++] = b; }
void uleb(long v) { while (v >= 128) { emit((int)(v & 127) | 128); v = v >> 7; } emit((int)v); }
void sleb(long v) {
  int b;
  while (1) {
    b = (int)(v & 127);
    v = v >> 7;
    if ((v == 0 && (b & 64) == 0) || (v == -1 && (b & 64) != 0)) { emit(b); return; }
    emit(b | 128);
  }
}
void iconst(long v) { emit(0x41); sleb(v); }
void lconst(long v) { emit(0x42); sleb(v); }
void lget(int i) { emit(0x20); uleb(i); }
void lset(int i) { emit(0x21); uleb(i); }
void ltee(int i) { emit(0x22); uleb(i); }
void callf(int f) { emit(0x10); uleb(fwasm[f]); }

char dataseg[MAXDATA]; int ndata;
int database; int globalsize;

int strdata(int p, int n) {
  int a = database + ndata; int i;
  for (i = 0; i < n; i++) { if (ndata >= MAXDATA) die("too much data"); dataseg[ndata++] = strs[p + i]; }
  return a;
}

void loadmem(int t, int a, int off) {
  int i;
  if (t == TINT) { lget(a); emit(0x29); emit(3); uleb(off); return; }
  if (t == TBYTE || t == TBOOL) { lget(a); emit(0x2d); emit(0); uleb(off); return; }
  for (i = 0; i < words(t); i++) { lget(a); emit(0x28); emit(2); uleb(off + 4 * i); }
}
void storemem(int t, int a, int off, int v) {
  int i;
  if (t == TINT) { lget(a); lget(v); emit(0x37); emit(3); uleb(off); return; }
  if (t == TBYTE || t == TBOOL) { lget(a); lget(v); emit(0x3a); emit(0); uleb(off); return; }
  for (i = 0; i < words(t); i++) { lget(a); lget(v + i); emit(0x36); emit(2); uleb(off + 4 * i); }
}
int templocals(int t) {
  int first = nloc; int i;
  for (i = 0; i < words(t); i++) { if (i == 0) newlocal(wtype(t)); else newlocal(I32); }
  return first;
}
int spill(int t) {
  int v = templocals(t); int i;
  for (i = words(t) - 1; i >= 0; i--) lset(v + i);
  return v;
}
void pushlocals(int t, int v) { int i; for (i = 0; i < words(t); i++) lget(v + i); }
void drop(int n) { int i; for (i = 0; i < n; i++) emit(0x1a); }

/* --------------------------------------------------------- code generation */

int opcode(int op, int t) {
  int w = wtype(t) == I64; int u;
  if (op == '+') { if (w) return 0x7c; return 0x6a; }
  if (op == '-') { if (w) return 0x7d; return 0x6b; }
  if (op == '*') { if (w) return 0x7e; return 0x6c; }
  if (op == '/') { if (w) return 0x7f; if (t == TBYTE) return 0x6e; return 0x6d; }
  if (op == '%') { if (w) return 0x81; if (t == TBYTE) return 0x70; return 0x6f; }
  if (op == '&') { if (w) return 0x83; return 0x71; }
  if (op == '|') { if (w) return 0x84; return 0x72; }
  if (op == '^') { if (w) return 0x85; return 0x73; }
  if (op == T_SHL) { if (w) return 0x86; return 0x74; }
  if (op == T_SHR) { if (w) return 0x87; if (t == TBYTE) return 0x76; return 0x75; }
  if (op == T_EQ) { if (w) return 0x51; return 0x46; }
  if (op == T_NE) { if (w) return 0x52; return 0x47; }
  u = t == TBYTE;
  if (op == '<') { if (w) return 0x53; if (u) return 0x49; return 0x48; }
  if (op == '>') { if (w) return 0x55; if (u) return 0x4b; return 0x4a; }
  if (op == T_LE) { if (w) return 0x57; if (u) return 0x4d; return 0x4c; }
  if (w) return 0x59;
  if (u) return 0x4f;
  return 0x4e;
}

void widen(int t) { if (wtype(t) == I32) { if (t == TBYTE) emit(0xad); else emit(0xac); } }
void toi32(int t) { if (wtype(t) == I64) emit(0xa7); }

void arith(int op, int t) {
  if (op == T_ANDNOT) {
    if (wtype(t) == I64) { lconst(-1); emit(0x85); } else { iconst(-1); emit(0x73); }
    emit(opcode('&', t));
    return;
  }
  emit(opcode(op, t));
  if (t == TBYTE && (op == '+' || op == '-' || op == '*' || op == T_SHL)) { iconst(255); emit(0x71); }
}

void gen(int e);
void gencall(int e) {
  int l;
  for (l = nb[e]; l >= 0; l = nb[l]) gen(na[l]);
  callf(na[e]);
}
void ptrlen(int t) { if (isslice(t)) drop(1); }

int indexaddr(int e); void genslice(int e); void genappend(int e); void genintrinsic(int e);

void printlit(char *lit) {
  int p = nstrs; int n = cstrlen(lit); int i;
  for (i = 0; i < n; i++) addstr(lit[i]);
  iconst(strdata(p, n));
  iconst(n);
  callf(findfunc("rt_printstr"));
}

void gen(int e) {
  int k; int t; int a; int op; int l; int r; int lt; int x; int c; int size; int d; int s; int n;
  int f; int p; int first; int xt;
  errline = nline[e];
  k = nk[e]; t = nt[e];
  if (k == E_CONST) { if (t == TUNTYPED || t == TINT) lconst(nv[e]); else iconst(nv[e]); return; }
  if (k == E_STR) { iconst(strdata(na[e], nb[e])); iconst(nb[e]); return; }
  if (k == E_LOCAL) { pushlocals(t, na[e]); return; }
  if (k == E_GLOBAL) { a = newlocal(I32); iconst(na[e]); lset(a); loadmem(t, a, 0); return; }
  if (k == E_BIN) {
    op = nv[e]; l = na[e]; r = nb[e];
    if (op == T_ANDAND || op == T_OROR) {
      gen(l);
      emit(0x04); emit(I32);
      if (op == T_ANDAND) { gen(r); emit(0x05); iconst(0); }
      else { iconst(1); emit(0x05); gen(r); }
      emit(0x0b);
      return;
    }
    lt = nt[l];
    gen(l);
    gen(r);
    if (lt == TSTR) {
      if (op == '+') { callf(findfunc("rt_concat")); return; }
      callf(findfunc("rt_strcmp"));
      lconst(0);
      emit(opcode(op, TINT));
      return;
    }
    if (op == T_SHL || op == T_SHR) {
      if (wtype(nt[r]) != wtype(lt)) { if (wtype(lt) == I64) emit(0xad); else emit(0xa7); }
    }
    if (iscmp(op)) { emit(opcode(op, lt)); return; }
    arith(op, lt);
    return;
  }
  if (k == E_UN) {
    op = nv[e];
    if (op == '!') { gen(na[e]); emit(0x45); return; }
    if (op == '-') {
      if (wtype(t) == I64) lconst(0); else iconst(0);
      gen(na[e]);
      arith('-', t);
      return;
    }
    gen(na[e]);
    if (wtype(t) == I64) lconst(-1); else iconst(-1);
    arith('^', t);
    if (t == TBYTE) { iconst(255); emit(0x71); }
    return;
  }
  if (k == E_CALL) { gencall(e); return; }
  if (k == E_INDEX) { a = indexaddr(e); loadmem(t, a, 0); return; }
  if (k == E_SLICE) { genslice(e); return; }
  if (k == E_LEN || k == E_CAP) {
    x = na[e];
    gen(x);
    c = newlocal(I32);
    if (isslice(nt[x]) && k == E_CAP) { lset(c); drop(2); }
    else { if (isslice(nt[x])) drop(1); lset(c); drop(1); }
    lget(c);
    emit(0xad);
    return;
  }
  if (k == E_APPEND) { genappend(e); return; }
  if (k == E_MAKE) {
    size = msize(elemtype(t));
    gen(na[e]);
    toi32(TINT);
    l = newlocal(I32);
    lset(l);
    c = l;
    if (nb[e] >= 0) { gen(nb[e]); toi32(TINT); c = newlocal(I32); lset(c); }
    lget(c); iconst(size); emit(0x6c);
    callf(findfunc("rt_alloc"));
    lget(l); lget(c);
    return;
  }
  if (k == E_COPY) {
    size = msize(elemtype(nt[na[e]]));
    gen(na[e]); ptrlen(nt[na[e]]);
    d = spill(TSTR);
    gen(nb[e]); ptrlen(nt[nb[e]]);
    s = spill(TSTR);
    n = newlocal(I32);
    lget(d + 1); lget(s + 1); lget(d + 1); lget(s + 1);
    emit(0x49); emit(0x1b);
    lset(n);
    lget(d); lget(s); lget(n); iconst(size); emit(0x6c);
    emit(0xfc); emit(10); emit(0); emit(0);
    lget(n); emit(0xad);
    return;
  }
  if (k == E_CONV) {
    x = na[e]; f = nt[x];
    gen(x);
    if (isint(t)) {
      if (wtype(f) == I64 && wtype(t) == I32) emit(0xa7);
      else if (wtype(f) == I32 && wtype(t) == I64) { if (f == TBYTE) emit(0xad); else emit(0xac); }
      if (t == TBYTE && f != TBYTE) { iconst(255); emit(0x71); }
      return;
    }
    ptrlen(f);
    s = spill(TSTR);
    lget(s + 1);
    callf(findfunc("rt_alloc"));
    p = newlocal(I32);
    ltee(p);
    lget(s); lget(s + 1);
    emit(0xfc); emit(10); emit(0); emit(0);
    lget(p); lget(s + 1);
    if (isslice(t)) lget(s + 1);
    return;
  }
  if (k == E_INTRIN) { genintrinsic(e); return; }
  if (k == E_PRINT) {
    first = 1;
    for (l = nb[e]; l >= 0; l = nb[l]) {
      x = na[l];
      if (!first && nv[e] == 1) printlit(" ");
      first = 0;
      gen(x);
      xt = nt[x];
      if (xt == TSTR) callf(findfunc("rt_printstr"));
      else if (xt == TBOOL) callf(findfunc("rt_printbool"));
      else if (isint(xt)) {
        if (wtype(xt) == I32) { if (xt == TBYTE) emit(0xad); else emit(0xac); }
        callf(findfunc("rt_printint"));
      } else die(cat("can't print ", typename(xt)));
    }
    if (nv[e] == 1) printlit("\n");
    return;
  }
  if (k == E_PANIC) { gen(na[e]); callf(findfunc("rt_fail")); return; }
  die("internal error: gen");
}

int indexaddr(int e) {
  int s = na[e]; int st = nt[s]; int size = 1; int v; int i; int a;
  if (isslice(st)) size = msize(elemtype(st));
  gen(s);
  ptrlen(st);
  v = spill(TSTR);
  gen(nb[e]);
  widen(nt[nb[e]]);
  i = newlocal(I64);
  ltee(i);
  lget(v + 1); emit(0xad);
  emit(0x5a);
  emit(0x04); emit(0x40);
  callf(findfunc("rt_bounds"));
  emit(0x0b);
  a = newlocal(I32);
  lget(v); lget(i); emit(0xa7);
  if (size != 1) { iconst(size); emit(0x6c); }
  emit(0x6a);
  lset(a);
  return a;
}

void genslice(int e) {
  int s = na[e]; int st = nt[s]; int size = 1; int v; int lo; int hi;
  if (isslice(st)) size = msize(elemtype(st));
  gen(s);
  v = templocals(slicetype(TBYTE));
  if (isslice(st)) lset(v + 2);
  lset(v + 1);
  lset(v);
  if (!isslice(st)) { lget(v + 1); lset(v + 2); }
  lo = newlocal(I32);
  hi = newlocal(I32);
  if (nb[e] >= 0) { gen(nb[e]); toi32(TINT); } else iconst(0);
  lset(lo);
  if (nc[e] >= 0) { gen(nc[e]); toi32(TINT); } else lget(v + 1);
  lset(hi);
  lget(hi); lget(v + 2); emit(0x4b);
  lget(lo); lget(hi); emit(0x4b);
  emit(0x72);
  emit(0x04); emit(0x40);
  callf(findfunc("rt_bounds"));
  emit(0x0b);
  lget(v); lget(lo);
  if (size != 1) { iconst(size); emit(0x6c); }
  emit(0x6a);
  lget(hi); lget(lo); emit(0x6b);
  if (isslice(st)) { lget(v + 2); lget(lo); emit(0x6b); }
}

void growto(int v, int need, int size) {
  lget(v); lget(v + 1); lget(v + 2); lget(need); iconst(size);
  callf(findfunc("rt_grow"));
  lset(v + 2);
  lset(v);
}

void genappend(int e) {
  int t = nt[e]; int et = elemtype(t); int size = msize(et); int v; int x; int w; int need;
  int n; int *vals; int nvals = 0; int l; int a; int i;
  gen(na[e]);
  v = spill(t);
  if (nc[e] == 1) {
    x = na[nb[e]];
    gen(x);
    ptrlen(nt[x]);
    w = spill(TSTR);
    need = newlocal(I32);
    lget(v + 1); lget(w + 1); emit(0x6a);
    lset(need);
    growto(v, need, size);
    lget(v); lget(v + 1); iconst(size); emit(0x6c); emit(0x6a);
    lget(w); lget(w + 1); iconst(size); emit(0x6c);
    emit(0xfc); emit(10); emit(0); emit(0);
    lget(v); lget(need); lget(v + 2);
    return;
  }
  n = listlen(nb[e]);
  vals = (int *)malloc(4 * n + 4);
  for (l = nb[e]; l >= 0; l = nb[l]) { gen(na[l]); vals[nvals++] = spill(et); }
  need = newlocal(I32);
  lget(v + 1); iconst(n); emit(0x6a);
  lset(need);
  growto(v, need, size);
  a = newlocal(I32);
  lget(v); lget(v + 1); iconst(size); emit(0x6c); emit(0x6a);
  lset(a);
  for (i = 0; i < n; i++) storemem(et, a, i * size, vals[i]);
  lget(v); lget(need); lget(v + 2);
}

void genintrinsic(int e) {
  int w = nv[e]; int l;
  for (l = nb[e]; l >= 0; l = nb[l]) {
    gen(na[l]);
    if (w == 7) { ptrlen(nt[na[l]]); drop(1); }
  }
  if (w == 1) { emit(0x23); emit(0); }
  else if (w == 2) { emit(0x3f); emit(0); }
  else if (w == 3) { emit(0x40); emit(0); }
  else if (w == 4) { emit(0x28); emit(2); emit(0); }
  else if (w == 5) { emit(0x36); emit(2); emit(0); }
  else if (w == 6) { emit(0xfc); emit(10); emit(0); emit(0); }
}

void store(int e) {
  int t = nt[e]; int k = nk[e]; int v; int i; int a;
  if (k == E_BLANK) { drop(words(t)); return; }
  if (k == E_LOCAL) {
    v = na[e];
    for (i = words(t) - 1; i >= 0; i--) lset(v + i);
    return;
  }
  v = spill(t);
  a = -1;
  if (k == E_GLOBAL) { a = newlocal(I32); iconst(na[e]); lset(a); }
  else a = indexaddr(e);
  storemem(t, a, 0, v);
}

/* --------------------------------------------------------------- statements */

int depth; int brk; int cont;

void shiftfix(int op, int t, int ct) {
  if ((op == T_SHL || op == T_SHR) && wtype(ct) != wtype(t)) { if (wtype(t) == I64) emit(0xad); else emit(0xa7); }
}

void genstmt(int s) {
  int k; int l; int e; int f; int i; int t; int v; int lhs; int rhs; int *vals; int *types; int nvals; int n; int x;
  int a; int ob; int oc; int top; int xt; int nn; int et; int tag; int tagv; int def; int c; int first; int b;
  errline = nline[s];
  k = nk[s];
  if (k == S_BLOCK) { for (l = na[s]; l >= 0; l = nb[l]) genstmt(na[l]); return; }
  if (k == S_EXPR) {
    e = na[s];
    gen(e);
    if (nt[e] == TTUPLE) {
      f = na[e];
      for (i = 0; i < fnr[f]; i++) drop(words(rtypes[frstart[f] + i]));
    } else drop(words(nt[e]));
    return;
  }
  if (k == S_ZERO) {
    t = nt[s]; v = na[s];
    for (i = 0; i < words(t); i++) {
      if (i == 0 && wtype(t) == I64) lconst(0); else iconst(0);
      lset(v + i);
    }
    return;
  }
  if (k == S_ASSIGN) {
    lhs = na[s]; rhs = nb[s];
    if (listlen(lhs) == 1 && nt[na[rhs]] != TTUPLE) { gen(na[rhs]); store(na[lhs]); return; }
    vals = (int *)malloc(4 * listlen(lhs) + 64);
    types = (int *)malloc(4 * listlen(lhs) + 64);
    nvals = 0;
    if (nt[na[rhs]] == TTUPLE) {
      f = na[na[rhs]];
      gen(na[rhs]);
      for (i = fnr[f] - 1; i >= 0; i--) {
        t = rtypes[frstart[f] + i];
        types[nvals] = t;
        vals[nvals] = spill(t);
        nvals++;
      }
      n = nvals;
      for (i = 0; i < n / 2; i++) {
        x = vals[i]; vals[i] = vals[n - 1 - i]; vals[n - 1 - i] = x;
        x = types[i]; types[i] = types[n - 1 - i]; types[n - 1 - i] = x;
      }
    } else {
      for (l = rhs; l >= 0; l = nb[l]) {
        gen(na[l]);
        types[nvals] = nt[na[l]];
        vals[nvals] = spill(nt[na[l]]);
        nvals++;
      }
    }
    i = 0;
    for (l = lhs; l >= 0; l = nb[l]) {
      if (!isblank(na[l])) { pushlocals(types[i], vals[i]); store(na[l]); }
      i++;
    }
    return;
  }
  if (k == S_OPASSIGN) {
    l = na[s]; t = nt[l];
    if (nk[l] == E_INDEX) {
      a = indexaddr(l);
      loadmem(t, a, 0);
      gen(nb[s]);
      if (t == TSTR) callf(findfunc("rt_concat"));
      else { shiftfix(nv[s], t, nt[nb[s]]); arith(nv[s], t); }
      v = spill(t);
      storemem(t, a, 0, v);
      return;
    }
    gen(l);
    gen(nb[s]);
    if (t == TSTR) callf(findfunc("rt_concat"));
    else { shiftfix(nv[s], t, nt[nb[s]]); arith(nv[s], t); }
    store(l);
    return;
  }
  if (k == S_IF) {
    if (nd[s] >= 0) genstmt(nd[s]);
    gen(na[s]);
    emit(0x04); emit(0x40);
    depth++;
    genstmt(nb[s]);
    if (nc[s] >= 0) { emit(0x05); genstmt(nc[s]); }
    emit(0x0b);
    depth--;
    return;
  }
  if (k == S_FOR) {
    if (na[s] >= 0) genstmt(na[s]);
    ob = brk; oc = cont;
    emit(0x02); emit(0x40); depth++; brk = depth;
    emit(0x03); emit(0x40); depth++; top = depth;
    if (nb[s] >= 0) { gen(nb[s]); emit(0x45); emit(0x0d); uleb(depth - brk); }
    emit(0x02); emit(0x40); depth++; cont = depth;
    genstmt(nd[s]);
    emit(0x0b); depth--;
    if (nc[s] >= 0) genstmt(nc[s]);
    emit(0x0c); uleb(depth - top);
    emit(0x0b); emit(0x0b);
    depth = depth - 2;
    brk = ob; cont = oc;
    return;
  }
  if (k == S_RANGE) {
    x = nc[s]; xt = nt[x];
    gen(x);
    nn = -1; v = -1;
    if (xt == TINT) { nn = newlocal(I64); lset(nn); } else v = spill(xt);
    i = newlocal(I64);
    lconst(0); lset(i);
    ob = brk; oc = cont;
    emit(0x02); emit(0x40); depth++; brk = depth;
    emit(0x03); emit(0x40); depth++; top = depth;
    lget(i);
    if (xt == TINT) lget(nn); else { lget(v + 1); emit(0xad); }
    emit(0x59);
    emit(0x0d); uleb(depth - brk);
    if (na[s] >= 0) { lget(i); lset(na[s]); }
    if (nb[s] >= 0) {
      et = elemtype(xt);
      a = newlocal(I32);
      lget(v); lget(i); emit(0xa7); iconst(msize(et)); emit(0x6c); emit(0x6a);
      lset(a);
      loadmem(et, a, 0);
      store(node(E_LOCAL, et, nb[s], -1, -1, -1, 0));
    }
    emit(0x02); emit(0x40); depth++; cont = depth;
    genstmt(nd[s]);
    emit(0x0b); depth--;
    lget(i); lconst(1); emit(0x7c); lset(i);
    emit(0x0c); uleb(depth - top);
    emit(0x0b); emit(0x0b);
    depth = depth - 2;
    brk = ob; cont = oc;
    return;
  }
  if (k == S_SWITCH) {
    if (na[s] >= 0) genstmt(na[s]);
    tag = nb[s];
    tagv = -1;
    if (tag >= 0) { gen(tag); tagv = spill(nt[tag]); }
    ob = brk;
    emit(0x02); emit(0x40); depth++; brk = depth;
    def = -1;
    for (l = nc[s]; l >= 0; l = nb[l]) {
      c = na[l];
      if (na[c] < 0) { def = c; continue; }
      first = 1;
      for (x = na[c]; x >= 0; x = nb[x]) {
        if (tag >= 0) {
          pushlocals(nt[tag], tagv);
          gen(na[x]);
          if (nt[tag] == TSTR) { callf(findfunc("rt_strcmp")); emit(0x50); }
          else emit(opcode(T_EQ, nt[tag]));
        } else gen(na[x]);
        if (!first) emit(0x72);
        first = 0;
      }
      emit(0x04); emit(0x40); depth++;
      for (b = nb[c]; b >= 0; b = nb[b]) genstmt(na[b]);
      emit(0x0c); uleb(depth - brk);
      emit(0x0b); depth--;
    }
    if (def >= 0) for (b = nb[def]; b >= 0; b = nb[b]) genstmt(na[b]);
    emit(0x0b); depth--;
    brk = ob;
    return;
  }
  if (k == S_RETURN) {
    for (l = na[s]; l >= 0; l = nb[l]) gen(na[l]);
    emit(0x0f);
    return;
  }
  if (k == S_BREAK) {
    if (brk == 0) die("break outside a loop or switch");
    emit(0x0c); uleb(depth - brk);
    return;
  }
  if (k == S_CONTINUE) {
    if (cont == 0) die("continue outside a loop");
    emit(0x0c); uleb(depth - cont);
    return;
  }
  die("internal error: genstmt");
}

/* ------------------------------------------------------------- top level */

char code[MAXCODE]; int ncode;
int ndefined;

void skipblock() {
  int d = 0;
  while (1) {
    if (tk[tp] == T_EOF) die("unexpected end of file");
    if (tk[tp] == '{') d++;
    if (tk[tp] == '}') { d--; if (d == 0) { tp++; return; } }
    tp++;
  }
}

int istypeat(int i) { int save = tp; int r; tp = i; r = istypestart(); tp = save; return r; }

void funcsig() {
  int f; int np; int first; int t; int i; int nr;
  errline = tline[tp];
  if (tk[tp] != T_ID) die("expected a function name");
  f = nfn;
  if (nfn >= MAXFN) die("too many functions");
  fname[nfn] = tv[tp]; fnlen[nfn] = tl[tp]; nfn++;
  if (lookup(tv[tp], tl[tp]) >= 0) die(cat(srcstr(tv[tp], tl[tp]), " redeclared"));
  addsym(tv[tp], tl[tp], S_FUNC, 0, f);
  tp++;
  expect('(');
  fpstart[f] = nptypes;
  np = 0;
  while (!accept(')')) {
    first = nptypes;
    while (tk[tp] == T_ID) {
      pnames[nptypes] = tp; ptypes[nptypes] = 0; nptypes++;
      np++;
      tp++;
      if (!accept(',')) break;
    }
    if (nptypes == first) die("parameters must be named");
    t = parsetype();
    for (i = first; i < nptypes; i++) ptypes[i] = t;
    if (!accept(',')) { expect(')'); break; }
  }
  fnp[f] = np;
  frstart[f] = nrtypes;
  nr = 0;
  if (accept('(')) {
    while (!accept(')')) {
      if (tk[tp] == T_ID && !istypeat(tp)) die("named results are not supported");
      rtypes[nrtypes++] = parsetype();
      nr++;
      accept(',');
    }
  } else if (tk[tp] != '{' && tk[tp] != ';') {
    rtypes[nrtypes++] = parsetype();
    nr++;
  }
  fnr[f] = nr;
  if (tk[tp] == '{') { fbody[f] = tp; skipblock(); } else fbody[f] = -1;
  fwasm[f] = 0;
}

int iotav;

int constspec(int prevexpr) {
  int p; int n; int t; int exprtok; int save; int mark; int e; int end;
  errline = tline[tp];
  p = tv[tp]; n = tl[tp];
  expect(T_ID);
  t = -1;
  if (tk[tp] != '=' && tk[tp] != ';' && tk[tp] != ')') t = parsetype();
  exprtok = prevexpr;
  if (accept('=')) exprtok = tp;
  if (exprtok < 0) die("missing constant value");
  save = tp;
  tp = exprtok;
  mark = nsym;
  universe("iota", S_CONST, TUNTYPED, iotav);
  e = parseexpr();
  end = tp;
  nsym = mark;
  srclen = srclen - 4;
  if (exprtok == prevexpr) tp = save; else tp = end;
  if (nk[e] == E_STR && t < 0) { addsym(p, n, S_CONST, TSTR, na[e] + ((long)nb[e] << 32)); return exprtok; }
  if (nk[e] != E_CONST) die("not a constant");
  if (t >= 0) convert(e, t);
  addsym(p, n, S_CONST, nt[e], nv[e]);
  return exprtok;
}

void constdecl() {
  int prev;
  if (accept('(')) {
    prev = -1;
    iotav = 0;
    while (!accept(')')) {
      if (accept(';')) continue;
      prev = constspec(prev);
      iotav++;
    }
  } else {
    iotav = 0;
    constspec(-1);
  }
}

int vartoks[MAXVAR]; int nvartoks;
int initlist[MAXVAR]; int initexpr[MAXVAR]; int ninit;

void globalspec() {
  int p; int n; int t; int e; int s;
  errline = tline[tp];
  p = tv[tp]; n = tl[tp];
  expect(T_ID);
  t = -1;
  if (tk[tp] != '=') t = parsetype();
  e = -1;
  if (accept('=')) {
    e = parseexpr();
    if (t >= 0) convert(e, t);
    settle(e);
    t = nt[e];
  }
  globalsize = (globalsize + 7) & ~7;
  s = addsym(p, n, S_GLOBAL, t, 1024 + globalsize);
  globalsize = globalsize + msize(t);
  if (e >= 0) { initlist[ninit] = s; initexpr[ninit] = e; ninit++; }
}

void vardecl1() {
  if (accept('(')) {
    while (!accept(')')) { if (accept(';')) continue; globalspec(); }
  } else globalspec();
}

void skipdecl() {
  int d = 0; int t;
  while (1) {
    t = tk[tp];
    if (t == T_EOF) return;
    if (t == '(' || t == '{' || t == '[') d++;
    if (t == ')' || t == '}' || t == ']') d--;
    tp++;
    if (d == 0 && t == ';') return;
  }
}

/* --------------------------------------------------------------- the module */

char out[MAXOUT]; int nout;

void oemit(int b) { if (nout >= MAXOUT) die("module too big"); out[nout++] = b; }
void ouleb(long v) { while (v >= 128) { oemit((int)(v & 127) | 128); v = v >> 7; } oemit((int)v); }
void oname(char *s) { int n = cstrlen(s); int i; ouleb(n); for (i = 0; i < n; i++) oemit(s[i]); }
void onamesrc(int p, int n) { int i; ouleb(n); for (i = 0; i < n; i++) oemit(src[p + i]); }

int secstart;
void section(int id) { oemit(id); secstart = nout; }

char sizebuf[8];
void endsection() {
  int n = nout - secstart; int ns = 0; int v = n; int i;
  while (v >= 128) { sizebuf[ns++] = (v & 127) | 128; v = v >> 7; }
  sizebuf[ns++] = v;
  for (i = 0; i < ns; i++) oemit(0);
  for (i = n - 1; i >= 0; i--) out[secstart + ns + i] = out[secstart + i];
  for (i = 0; i < ns; i++) out[secstart + i] = sizebuf[i];
}

void emitvaltypes(int start, int n, int *types) {
  int cnt = 0; int i; int t; int w;
  for (i = 0; i < n; i++) cnt = cnt + words(types[start + i]);
  ouleb(cnt);
  for (i = 0; i < n; i++) {
    t = types[start + i];
    for (w = 0; w < words(t); w++) { if (w == 0) oemit(wtype(t)); else oemit(I32); }
  }
}

void finishfunc(int nparams);
void compilefunc(int f) {
  int mark; int i; int q; int nparamlocals; int body;
  curfunc = f;
  tp = fbody[f];
  nloc = 0;
  nfb = 0;
  depth = 0; brk = 0; cont = 0;
  mark = nsym;
  scopestart = nsym;
  for (i = 0; i < fnp[f]; i++) {
    q = pnames[fpstart[f] + i];
    declare(tv[q], tl[q], ptypes[fpstart[f] + i]);
  }
  nparamlocals = nloc;
  body = block();
  genstmt(body);
  if (fnr[f] > 0) emit(0);
  emit(0x0b);
  nsym = mark;
  finishfunc(nparamlocals);
}

/* the locals' declarations: built in out, after the module so far, then moved */
void addcode(int b) { if (ncode >= MAXCODE) die("too much code"); code[ncode++] = b; }
void finishfunc(int nparams) {
  int start = nout; int i; int elen; int n; int v;
  ouleb(nloc - nparams);
  for (i = nparams; i < nloc; i++) { ouleb(1); oemit(ltypes[i]); }
  elen = nout - start;
  n = elen + nfb;
  v = n;
  while (v >= 128) { addcode((v & 127) | 128); v = v >> 7; }
  addcode(v);
  for (i = 0; i < elen; i++) addcode(out[start + i]);
  for (i = 0; i < nfb; i++) addcode(fb[i]);
  nout = start;
  ndefined++;
}

void writeout(char *p, int n) {
  int w;
  while (n > 0) { w = sys_write(1, p, n); if (w <= 0) proc_exit(1); p = p + w; n = n - w; }
}

int main() {
  int n; int i; int t; int nimports; int nfuncs; int f; int mainf; int heap; int b; int sub; int outer;
  while ((n = sys_read(src + srclen, MAXSRC - 64 - srclen)) > 0) srclen = srclen + n;
  lex();
  universe("int", S_TYPE, TINT, 0);
  universe("int64", S_TYPE, TINT, 0);
  universe("uint", S_TYPE, TINT, 0);
  universe("int32", S_TYPE, TI32, 0);
  universe("byte", S_TYPE, TBYTE, 0);
  universe("uint8", S_TYPE, TBYTE, 0);
  universe("bool", S_TYPE, TBOOL, 0);
  universe("string", S_TYPE, TSTR, 0);
  universe("true", S_CONST, TBOOL, 1);
  universe("false", S_CONST, TBOOL, 0);

  tp = 0;
  while (tk[tp] != T_EOF) {
    errline = tline[tp];
    if (accept(';')) continue;
    if (accept(T_PACKAGE) || accept(T_IMPORT)) skipdecl();
    else if (accept(T_FUNC)) { funcsig(); accept(';'); }
    else if (accept(T_CONST)) constdecl();
    else if (accept(T_VAR)) { vartoks[nvartoks++] = tp; skipdecl(); }
    else if (accept(T_TYPE)) die("type declarations are not supported");
    else die("syntax error at top level");
  }
  for (i = 0; i < nvartoks; i++) { tp = vartoks[i]; vardecl1(); }
  database = 1024 + ((globalsize + 7) & ~7);

  nimports = 0;
  for (f = 0; f < nfn; f++) if (fbody[f] < 0) { fwasm[f] = nimports; nimports++; }
  nfuncs = nimports;
  for (f = 0; f < nfn; f++) if (fbody[f] >= 0) { fwasm[f] = nfuncs; nfuncs++; }
  mainf = -1;
  for (f = 0; f < nfn; f++) if (same(fname[f], fnlen[f], "main")) mainf = f;
  if (mainf < 0) die("no main function");

  for (f = 0; f < nfn; f++) if (fbody[f] >= 0) compilefunc(f);
  nloc = 0;
  nfb = 0;
  for (i = 0; i < ninit; i++) {
    gen(initexpr[i]);
    store(node(E_GLOBAL, stype[initlist[i]], sval[initlist[i]], -1, -1, -1, 0));
  }
  callf(mainf);
  emit(0x0b);
  finishfunc(0);

  heap = (database + ndata + 15) & ~15;
  oemit(0); oemit(0x61); oemit(0x73); oemit(0x6d); oemit(1); oemit(0); oemit(0); oemit(0);
  section(1);
  ouleb(nfn + 1);
  for (f = 0; f < nfn; f++) {
    oemit(0x60);
    emitvaltypes(fpstart[f], fnp[f], ptypes);
    emitvaltypes(frstart[f], fnr[f], rtypes);
  }
  oemit(0x60); oemit(0); oemit(0);
  endsection();
  section(2);
  ouleb(nimports);
  for (f = 0; f < nfn; f++) if (fbody[f] < 0) {
    oname("wasi_snapshot_preview1");
    onamesrc(fname[f], fnlen[f]);
    oemit(0);
    ouleb(f);
  }
  endsection();
  section(3);
  ouleb(ndefined);
  for (f = 0; f < nfn; f++) if (fbody[f] >= 0) ouleb(f);
  ouleb(nfn);
  endsection();
  section(5);
  ouleb(1); oemit(0); ouleb(heap / 65536 + 1);
  endsection();
  section(6);
  ouleb(1); oemit(I32); oemit(0); oemit(0x41);
  n = heap;
  while (1) {
    b = n & 127;
    n = n >> 7;
    if (n == 0 && (b & 64) == 0) { oemit(b); break; }
    oemit(b | 128);
  }
  oemit(0x0b);
  endsection();
  section(7);
  ouleb(2);
  oname("memory"); oemit(2); oemit(0);
  oname("_start"); oemit(0); ouleb(nfuncs);
  endsection();
  section(10);
  ouleb(ndefined);
  for (i = 0; i < ncode; i++) oemit(code[i]);
  endsection();
  section(11);
  ouleb(1); oemit(0); oemit(0x41);
  n = database;
  while (1) {
    b = n & 127;
    n = n >> 7;
    if (n == 0 && (b & 64) == 0) { oemit(b); break; }
    oemit(b | 128);
  }
  oemit(0x0b);
  ouleb(ndata);
  for (i = 0; i < ndata; i++) oemit(dataseg[i]);
  endsection();
  section(0);
  oname("name");
  oemit(1);
  sub = nout;
  ouleb(nfn + 1);
  for (f = 0; f < nfn; f++) if (fbody[f] < 0) { ouleb(fwasm[f]); onamesrc(fname[f], fnlen[f]); }
  for (f = 0; f < nfn; f++) if (fbody[f] >= 0) { ouleb(fwasm[f]); onamesrc(fname[f], fnlen[f]); }
  ouleb(nfuncs);
  oname("_start");
  outer = secstart;
  secstart = sub;
  endsection();
  secstart = outer;
  endsection();
  writeout(out, nout);
  return 0;
}
