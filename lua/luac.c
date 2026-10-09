/* luac.c -- compile a Lua program to C, for lrt.c and cc.
 *
 *   luac < prog.lua > prog.c
 *   cat c/libc.c lua/lrt.c prog.c | cc > prog.wasm
 *
 * Every Lua function becomes a C function  int Fn(int clo, int base,
 * int nargs)  (see lrt.c for the calling convention and how values are
 * represented).  Expressions are compiled to three-address C: each value
 * lands in a temporary Tn.  Lua locals become C locals Ln, or, if a closure
 * captures them, Ln holds a cell.  Needs num.c first.  The source is parsed twice: the first pass only finds out which
 * locals are captured, since that must be known where they are declared.
 */

enum {
  MAXSRC = 1048576, MAXTOK = 300000, MAXPOOL = 1048576,
  MAXLV = 32, LBUF = 524288, MAXOUT = 8388608,
  MAXK = 20000, MAXACT = 8192, MAXUP = 256, MAXDECL = 100000, MAXSTK = 4096
};

int sys_read(char *buf, int n);
int sys_write(int fd, char *buf, int n);

int cstrlen(char *s) { int n = 0; while (s[n]) n++; return n; }
int same(char *a, char *b, int n) {
  int i = 0;
  while (i < n) { if (a[i] != b[i]) return 0; i++; }
  return 1;
}
void eputs(char *s) { sys_write(2, s, cstrlen(s)); }
char nbuf[32];
char *lnumstr(long n) {
  int i = 31; int neg = n < 0; unsigned long u;
  nbuf[i] = 0;
  u = neg ? -(unsigned long)n : (unsigned long)n;
  do { i--; nbuf[i] = '0' + (int)(u % 10); u = u / 10; } while (u);
  if (neg) { i--; nbuf[i] = '-'; }
  return nbuf + i;
}
char *numstr(int n) { return lnumstr(n); }

/* ---------------------------------------------------------------- lexer */

enum {
  X_EOF = 256, X_NAME, X_STRING, X_INT, X_FLOAT,
  X_AND, X_BREAK, X_DO, X_ELSE, X_ELSEIF, X_END, X_FALSE, X_FOR, X_FUNCTION,
  X_GOTO, X_IF, X_IN, X_LOCAL, X_NIL, X_NOT, X_OR, X_REPEAT, X_RETURN, X_THEN,
  X_TRUE, X_UNTIL, X_WHILE,
  X_DOTS, X_CONCAT, X_IDIV, X_EQ, X_NE, X_LE, X_GE, X_SHL, X_SHR, X_DBCOLON
};
char *keywords = "and break do else elseif end false for function goto if in local nil not or repeat return then true until while ";
char *puncts = "...//==~=<=>=<<>>::";   /* ... first, then 2-char ones; .. is special */

char src[MAXSRC]; int srclen;
char pool[MAXPOOL]; int npool;     /* names and string contents */
int tk[MAXTOK]; int tv[MAXTOK]; int tl[MAXTOK]; int tline[MAXTOK]; int ntok;
long tn[MAXTOK];   /* a number's value, as an encoded Lua value (see lrt.c) */
int tbig[MAXTOK];  /* an integer outside 48 bits: tn is the plain integer */
int tp;

int lexing; int line;   /* the lexer's current line */
void die2(char *a, char *b) {
  eputs("luac: line "); eputs(numstr(lexing ? line : tline[tp])); eputs(": "); eputs(a); eputs(b); eputs("\n");
  __builtin_trap();
}
void die(char *msg) { die2(msg, ""); }

int isdig(int c) { return c >= '0' && c <= '9'; }
int isalpha_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
int hexval(int c) {
  if (isdig(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
void poolc(int c) { if (npool >= MAXPOOL) die("out of string space"); pool[npool++] = c; }

int keyword(int p, int n) {
  char *k = keywords; int t = X_AND; int i;
  while (*k) {
    i = 0;
    while (k[i] != ' ') i++;
    if (i == n && same(src + p, k, n)) return t;
    k = k + i + 1; t++;
  }
  return X_NAME;
}

/* the level of a long bracket [==[ at i, or -1 */
int longlevel(int i) {
  int n = 0;
  if (src[i] != '[') return -1;
  i++;
  while (src[i] == '=') { n++; i++; }
  if (src[i] == '[') return n;
  return -1;
}
/* read a long string at i with level n into the pool; returns the new i */
int longstring(int i, int n, int keep) {
  int j;
  i = i + n + 2;
  if (src[i] == '\r') i++;
  if (src[i] == '\n') { i++; line++; }
  while (1) {
    if (!src[i]) die("unfinished long string or comment");
    if (src[i] == ']') {
      j = 1;
      while (src[i + j] == '=') j++;
      if (src[i + j] == ']' && j - 1 == n) return i + j + 1;
    }
    if (src[i] == '\n') line++;
    if (keep) poolc(src[i]);
    i++;
  }
  return i;
}

void utf8(int c) {
  if (c < 0x80) poolc(c);
  else if (c < 0x800) { poolc(0xc0 | c >> 6); poolc(0x80 | (c & 63)); }
  else if (c < 0x10000) { poolc(0xe0 | c >> 12); poolc(0x80 | (c >> 6 & 63)); poolc(0x80 | (c & 63)); }
  else { poolc(0xf0 | c >> 18); poolc(0x80 | (c >> 12 & 63)); poolc(0x80 | (c >> 6 & 63)); poolc(0x80 | (c & 63)); }
}

int quoted(int i, int q) {
  int c; int v; int k;
  i++;
  while (src[i] != q) {
    c = src[i];
    if (!c || c == '\n') die("unfinished string");
    if (c != '\\') { poolc(c); i++; continue; }
    i++; c = src[i];
    if (c == 'n') { poolc(10); i++; }
    else if (c == 't') { poolc(9); i++; }
    else if (c == 'r') { poolc(13); i++; }
    else if (c == 'a') { poolc(7); i++; }
    else if (c == 'b') { poolc(8); i++; }
    else if (c == 'f') { poolc(12); i++; }
    else if (c == 'v') { poolc(11); i++; }
    else if (c == '\n') { poolc(10); i++; line++; }
    else if (c == 'x') { poolc(hexval(src[i + 1]) * 16 + hexval(src[i + 2])); i = i + 3; }
    else if (c == 'z') {
      i++;
      while (src[i] == ' ' || src[i] == '\n' || src[i] == '\t' || src[i] == '\r') {
        if (src[i] == '\n') line++;
        i++;
      }
    } else if (c == 'u') {
      i = i + 2; v = 0;
      while (src[i] != '}') { v = v * 16 + hexval(src[i]); i++; }
      i++;
      utf8(v);
    } else if (isdig(c)) {
      v = 0; k = 0;
      while (k < 3 && isdig(src[i])) { v = v * 10 + src[i] - '0'; i++; k++; }
      poolc(v);
    } else { poolc(c); i++; }
  }
  return i + 1;
}

void numeral(int k, int i);

/* tokenize src from i; the tokens (and an X_EOF) are appended */
void lex(int i) {
  int c; int k; int v; int n; int big;
  line = 1; lexing = 1;
  while (1) {
    c = src[i];
    if (!c) break;
    if (c == '\n') { line++; i++; continue; }
    if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
    if (c == '#' && i == 0) { while (src[i] && src[i] != '\n') i++; continue; }
    if (c == '-' && src[i + 1] == '-') {
      i = i + 2;
      n = longlevel(i);
      if (n >= 0) i = longstring(i, n, 0);
      else while (src[i] && src[i] != '\n') i++;
      continue;
    }
    if (ntok >= MAXTOK - 1) die("too many tokens");
    tline[ntok] = line;
    if (isdig(c) || (c == '.' && isdig(src[i + 1]))) {
      k = i;
      while (isalpha_(src[i]) || isdig(src[i]) || src[i] == '.' ||
             ((src[i] == '-' || src[i] == '+') && (src[i - 1] == 'e' || src[i - 1] == 'E'))) i++;
      numeral(k, i);
    } else if (isalpha_(c)) {
      k = i;
      while (isalpha_(src[i]) || isdig(src[i])) i++;
      tk[ntok] = keyword(k, i - k);
      tv[ntok] = npool; tl[ntok] = i - k;
      while (k < i) poolc(src[k++]);
    } else if (c == '"' || c == '\'') {
      tk[ntok] = X_STRING; tv[ntok] = npool;
      i = quoted(i, c);
      tl[ntok] = npool - tv[ntok];
    } else if (longlevel(i) >= 0) {
      tk[ntok] = X_STRING; tv[ntok] = npool;
      i = longstring(i, longlevel(i), 1);
      tl[ntok] = npool - tv[ntok];
    } else if (c == '.' && src[i + 1] == '.' && src[i + 2] != '.') {
      tk[ntok] = X_CONCAT; i = i + 2;
    } else if (c == '.' && src[i + 1] == '.' && src[i + 2] == '.') {
      tk[ntok] = X_DOTS; i = i + 3;
    } else {
      k = 3;
      while (puncts[k] && !(puncts[k] == c && puncts[k + 1] == src[i + 1])) k = k + 2;
      if (puncts[k]) { tk[ntok] = X_IDIV + (k - 3) / 2; i = i + 2; }
      else { tk[ntok] = c; i++; }
    }
    ntok++;
  }
  tk[ntok] = X_EOF; tline[ntok] = line;
  ntok++;
  lexing = 0;
}

/* integers in 48 bits and floats are encoded in place; see lrt.c */
long encint(long n) { return (n & 281474976710655L) | -281474976710656L; }
long encdbl(double d) { return __f64_bits(d) + 281474976710656L; }
int fits48(long n) { return ((n << 16) >> 16) == n; }

/* the numeral src[k..i) */
void numeral(int k, int i) {
  unsigned long v = 0; int over = 0; int j = k; double d; int hex = 0;
  if (src[k] == '0' && (src[k + 1] == 'x' || src[k + 1] == 'X')) {
    hex = 1; j = k + 2;
    while (j < i && hexval(src[j]) >= 0) { v = v * 16 + hexval(src[j]); j++; }   /* wraps */
    if (j < i) die("hexadecimal floats are not supported");
  } else {
    while (j < i && isdig(src[j])) {
      if (v > 922337203685477580UL || (v == 922337203685477580UL && src[j] > '7')) over = 1;
      v = v * 10 + src[j] - '0'; j++;
    }
  }
  tk[ntok] = X_INT; tbig[ntok] = 0;
  if (!hex && (j < i || over)) {   /* a float (also an integer that does not fit) */
    d = str2dbl(src + k, i - k);
    if (!numok) die("malformed number");
    tk[ntok] = X_FLOAT; tn[ntok] = encdbl(d);
    return;
  }
  if (fits48((long)v)) tn[ntok] = encint((long)v);
  else { tn[ntok] = (long)v; tbig[ntok] = 1; }
}

int accept(int t) { if (tk[tp] == t) { tp++; return 1; } return 0; }
void expect(int t, char *what) {
  if (!accept(t)) die2("expected ", what);
}
int checkname() { if (tk[tp] != X_NAME) die("expected a name"); return tp++; }

/* ---------------------------------------------------- output, per level */

int gen;                 /* second pass: really write */
char lbuf[MAXLV * LBUF];
int llen[MAXLV];
int fl;                  /* the level of the function being compiled */
char fout[MAXOUT]; int fout_n;

void putc_(int c) {
  if (!gen) return;
  if (llen[fl] >= LBUF) die("function too large");
  lbuf[fl * LBUF + llen[fl]++] = c;
}
void puts_(char *s) { while (*s) putc_(*s++); }
void ef(char *f, int a, int b, int c, int d) {
  int n = 0; int v;
  while (*f) {
    if (*f != '%') { putc_(*f++); continue; }
    f++;
    v = n == 0 ? a : n == 1 ? b : n == 2 ? c : d;
    n++;
    if (*f == 't') putc_('T');
    else if (*f == 'l') putc_('L');
    puts_(numstr(v));
    f++;
  }
}
void e0(char *f) { ef(f, 0, 0, 0, 0); }
void e1(char *f, int a) { ef(f, a, 0, 0, 0); }
void e2(char *f, int a, int b) { ef(f, a, b, 0, 0); }
void e3(char *f, int a, int b, int c) { ef(f, a, b, c, 0); }

void fputs_(char *s, int n) {
  int i = 0;
  if (fout_n + n > MAXOUT) die("output too large");
  while (i < n) fout[fout_n++] = s[i++];
}
void fput(char *s) { fputs_(s, cstrlen(s)); }

/* ------------------------------------------------------- function state */

int fnum[MAXLV]; int ntemp[MAXLV]; int maxtemp[MAXLV]; int nloc[MAXLV];
int nup[MAXLV]; int np[MAXLV]; int isva[MAXLV];
int updecl[MAXLV * MAXUP]; int upkind[MAXLV * MAXUP]; int upsrc[MAXLV * MAXUP];
int nfuncs;

/* active local variables */
int aname[MAXACT]; int alen[MAXACT]; int alv[MAXACT]; int ac[MAXACT]; int adecl[MAXACT];
int nact;
char captured[MAXDECL];  /* by declaration number; filled in by pass one */
int ndecl;

int newtemp() {
  int t = ntemp[fl]++;
  if (ntemp[fl] > maxtemp[fl]) maxtemp[fl] = ntemp[fl];
  return t;
}

/* declare a local named by pool[p..p+n]; returns its C local number */
int declare(int p, int n) {
  int c = nloc[fl]++;
  if (nact >= MAXACT) die("too many local variables");
  if (ndecl >= MAXDECL) die("too many declarations");
  aname[nact] = p; alen[nact] = n; alv[nact] = fl; ac[nact] = c; adecl[nact] = ndecl++;
  nact++;
  return c;
}
int iscapt(int a) { return captured[adecl[a]]; }

/* the index of active local a as an upvalue of level lv */
int upval(int lv, int a) {
  int i; int u;
  for (i = 0; i < nup[lv]; i++) if (updecl[lv * MAXUP + i] == adecl[a]) return i;
  u = nup[lv]++;
  if (u >= MAXUP) die("too many upvalues");
  updecl[lv * MAXUP + u] = adecl[a];
  if (alv[a] == lv - 1) { upkind[lv * MAXUP + u] = 0; upsrc[lv * MAXUP + u] = ac[a]; }
  else { upkind[lv * MAXUP + u] = 1; upsrc[lv * MAXUP + u] = upval(lv - 1, a); }
  return u;
}

/* string constants: K[i] */
int kpos[MAXK]; int klen[MAXK]; int nk;
int kconst(int p, int n) {
  int i;
  for (i = 0; i < nk; i++) if (klen[i] == n && same(pool + kpos[i], pool + p, n)) return i;
  if (nk >= MAXK) die("too many constants");
  kpos[nk] = p; klen[nk] = n;
  return nk++;
}

/* ---------------------------------------------------------- expressions */

/* The expression just parsed is described by dk, da, db: */
enum {
  E_VAL = 1,   /* in temp da */
  E_LOCAL,     /* local: C local da; db = captured */
  E_UPVAL,     /* upvalue da */
  E_GLOBAL,    /* global named K[da] */
  E_INDEX,     /* temp da indexed by temp db */
  E_CALL,      /* results at stk[Tda], Tdb of them */
  E_VARARG
};
int dk; int da; int db;

void expr();
void subexpr(int limit);
void block();
int funcbody(int method, int line);

/* make the current expression a single value in a temp */
int single() {
  int t;
  if (dk == E_VAL) return da;
  t = newtemp();
  if (dk == E_LOCAL) { if (db) e2("%t = cget(%l);\n", t, da); else e2("%t = %l;\n", t, da); }
  else if (dk == E_UPVAL) e2("%t = uvget(clo, %d);\n", t, da);
  else if (dk == E_GLOBAL) e2("%t = gget(K[%d]);\n", t, da);
  else if (dk == E_INDEX) e3("%t = lindex(%t, %t);\n", t, da, db);
  else if (dk == E_CALL) e3("%t = %t ? stk[%t] : 0;\n", t, db, da);
  else if (dk == E_VARARG) e3("%t = nargs > %d ? stk[base + %d] : 0;\n", t, np[fl], np[fl]);
  dk = E_VAL; da = t;
  return t;
}

void store(int k, int a, int b, int v) {
  if (k == E_LOCAL) { if (b) e2("cset(%l, %t);\n", a, v); else e2("%l = %t;\n", a, v); }
  else if (k == E_UPVAL) e2("uvset(clo, %d, %t);\n", a, v);
  else if (k == E_GLOBAL) e2("gset(K[%d], %t);\n", a, v);
  else if (k == E_INDEX) e3("lsetindex(%t, %t, %t);\n", a, b, v);
  else die("cannot assign to this expression");
}

/* push the current expression's value(s) on the stack */
void pushexp(int last) {
  int t;
  if (last && dk == E_CALL) e2("top = %t + %t;\n", da, db);
  else if (last && dk == E_VARARG) e2("top = pushva(top, base + %d, nargs - %d);\n", np[fl], np[fl]);
  else { t = single(); e1("stk[top++] = %t;\n", t); }
}
void explist_push() {
  while (1) {
    expr();
    if (!accept(',')) { pushexp(1); return; }
    pushexp(0);
  }
}

/* evaluate an expression list into n temps vt[out ..] */
int vt[MAXSTK]; int nvt;
void explist_n(int n, int out) {
  int i = 0; int j; int t;
  while (1) {
    expr();
    if (accept(',')) { t = single(); if (i < n) vt[out + i] = t; i++; continue; }
    if (i < n && dk == E_CALL) {
      for (j = i; j < n; j++) {
        t = newtemp();
        ef("%t = %t > %d ? stk[%t + ", t, db, j - i, da);
        e1("%d] : 0;\n", j - i);
        vt[out + j] = t;
      }
    } else if (i < n && dk == E_VARARG) {
      for (j = i; j < n; j++) {
        t = newtemp();
        e3("%t = nargs > %d ? stk[base + %d] : 0;\n", t, np[fl] + j - i, np[fl] + j - i);
        vt[out + j] = t;
      }
    } else {
      if (dk != E_CALL) t = single();
      if (i < n) vt[out + i] = t;
      for (j = i + 1; j < n; j++) { t = newtemp(); e1("%t = 0;\n", t); vt[out + j] = t; }
    }
    return;
  }
}

void callargs(int f, int self) {
  int b = newtemp(); int n; int t; int k;
  e1("%t = top;\n", b);
  if (self >= 0) e1("stk[top++] = %t;\n", self);
  if (tk[tp] == X_STRING) {
    k = kconst(tv[tp], tl[tp]); tp++;
    e1("stk[top++] = K[%d];\n", k);
  } else if (tk[tp] == '{') {
    expr();
    pushexp(0);
  } else {
    expect('(', "'('");
    if (!accept(')')) { explist_push(); expect(')', "')'"); }
  }
  n = newtemp();
  ef("%t = lcall(%t, %t, top - %t);\n", n, f, b, b);
  e1("top = %t;\n", b);
  dk = E_CALL; da = b; db = n;
}

int findlocal(int p, int n) {
  int i = nact - 1;
  while (i >= 0) {
    if (alen[i] == n && same(pool + aname[i], pool + p, n)) return i;
    i--;
  }
  return -1;
}

void singlevar(int tok) {
  int p = tv[tok]; int n = tl[tok]; int i = findlocal(p, n);
  if (i < 0) { dk = E_GLOBAL; da = kconst(p, n); return; }
  if (alv[i] == fl) { dk = E_LOCAL; da = ac[i]; db = iscapt(i); return; }
  captured[adecl[i]] = 1;
  dk = E_UPVAL; da = upval(fl, i);
}

/* load "constant code" is compiled ahead of time: the string's tokens
 * are appended to the token list, once, and compiled as a function */
int sublex[MAXTOK];
int loadconst() {
  int s = -1; int next; int save; int i; int f;
  if (tl[tp] != 4 || !same(pool + tv[tp], "load", 4) || findlocal(tv[tp], 4) >= 0) return 0;
  if (tk[tp + 1] == X_STRING) { s = tp + 1; next = tp + 2; }
  else if (tk[tp + 1] == '(' && tk[tp + 2] == X_STRING && tk[tp + 3] == ')') { s = tp + 2; next = tp + 4; }
  if (s < 0) return 0;
  if (!sublex[s]) {
    if (srclen + tl[s] + 1 >= MAXSRC) die("out of source space");
    for (i = 0; i < tl[s]; i++) src[srclen + i] = pool[tv[s] + i];
    src[srclen + tl[s]] = 0;
    sublex[s] = ntok;
    lex(srclen);
    srclen = srclen + tl[s] + 1;
  }
  save = next;
  tp = sublex[s];
  f = funcbody(2, 0);
  tp = save;
  dk = E_VAL; da = f;
  return 1;
}

void primaryexp() {
  int t;
  if (tk[tp] == X_NAME && loadconst()) return;
  if (tk[tp] == X_NAME) { singlevar(tp); tp++; return; }
  if (accept('(')) {
    expr();
    t = single();
    expect(')', "')'");
    dk = E_VAL; da = t;
    return;
  }
  die("unexpected symbol");
}

void suffixedexp() {
  int o; int k; int f; int n;
  primaryexp();
  while (1) {
    if (accept('.')) {
      o = single();
      n = checkname();
      k = newtemp(); e2("%t = K[%d];\n", k, kconst(tv[n], tl[n]));
      dk = E_INDEX; da = o; db = k;
    } else if (accept('[')) {
      o = single();
      expr(); k = single();
      expect(']', "']'");
      dk = E_INDEX; da = o; db = k;
    } else if (accept(':')) {
      o = single();
      n = checkname();
      f = newtemp(); e3("%t = lindex(%t, K[%d]);\n", f, o, kconst(tv[n], tl[n]));
      callargs(f, o);
    } else if (tk[tp] == '(' || tk[tp] == X_STRING || tk[tp] == '{') {
      f = single();
      callargs(f, -1);
    } else return;
  }
}

int constructor() {
  int t = newtemp(); int n = 0; int k; int v; int i;
  e1("%t = newtable();\n", t);
  expect('{', "'{'");
  while (tk[tp] != '}') {
    if (tk[tp] == X_NAME && tk[tp + 1] == '=') {
      i = tp; tp = tp + 2;
      k = newtemp(); e2("%t = K[%d];\n", k, kconst(tv[i], tl[i]));
      expr(); v = single();
      e3("tset(%t, %t, %t);\n", t, k, v);
    } else if (accept('[')) {
      expr(); k = single();
      expect(']', "']'"); expect('=', "'='");
      expr(); v = single();
      e3("tset(%t, %t, %t);\n", t, k, v);
    } else {
      expr();
      i = tk[tp] == '}' || ((tk[tp] == ',' || tk[tp] == ';') && tk[tp + 1] == '}');
      if (i && dk == E_CALL) { e3("tappend(%t, %t, %t, ", t, da, db); e1("%d);\n", n + 1); }
      else if (i && dk == E_VARARG) {
        e3("tappend(%t, base + %d, nargs - %d, ", t, np[fl], np[fl]); e1("%d);\n", n + 1);
      } else {
        v = single(); n++;
        e1("tset(%t, ", t); puts_(lnumstr(encint(n))); e1("L, %t);\n", v);
      }
    }
    if (!accept(',') && !accept(';')) break;
  }
  expect('}', "'}'");
  dk = E_VAL; da = t;
  return t;
}

/* a numeric constant into a temp */
int numconst(int i, int neg) {
  int r = newtemp(); long v = tn[i]; double d;
  if (tk[i] == X_FLOAT) {
    if (neg) v = v ^ (-9223372036854775807L - 1);
  } else if (!tbig[i]) {
    v = (v << 16) >> 16;   /* decode */
    if (neg) v = -v;
    if (fits48(v)) v = encint(v);
    else { e1("%t = mkint(", r); puts_(lnumstr(v)); puts_("L);\n"); return r; }
  } else {
    if (neg) v = -v;
    if (fits48(v)) v = encint(v);
    else { e1("%t = mkint(", r); puts_(lnumstr(v)); puts_("L);\n"); return r; }
  }
  e1("%t = ", r); puts_(lnumstr(v)); puts_("L;\n");
  return r;
}

void simpleexp() {
  int t = tk[tp]; int r;
  if (t == X_INT || t == X_FLOAT) { tp++; r = numconst(tp - 1, 0); dk = E_VAL; da = r; return; }
  if (t == X_STRING) {
    tp++; r = newtemp(); e2("%t = K[%d];\n", r, kconst(tv[tp - 1], tl[tp - 1]));
    dk = E_VAL; da = r; return;
  }
  if (t == X_NIL || t == X_TRUE || t == X_FALSE) {
    tp++; r = newtemp(); e2("%t = %d;\n", r, t == X_NIL ? 0 : t == X_TRUE ? 4 : 2);
    dk = E_VAL; da = r; return;
  }
  if (t == X_DOTS) {
    if (!isva[fl]) die("cannot use '...' outside a vararg function");
    tp++; dk = E_VARARG; return;
  }
  if (t == '{') { constructor(); return; }
  if (t == X_FUNCTION) { tp++; r = funcbody(0, tline[tp]); dk = E_VAL; da = r; return; }
  suffixedexp();
}

/* binary operators: left and right priorities, and the runtime function */
int lprio(int op) {
  if (op == X_OR) return 1;
  if (op == X_AND) return 2;
  if (op == '<' || op == '>' || op == X_LE || op == X_GE || op == X_NE || op == X_EQ) return 3;
  if (op == '|') return 4;
  if (op == '~') return 5;
  if (op == '&') return 6;
  if (op == X_SHL || op == X_SHR) return 7;
  if (op == X_CONCAT) return 9;
  if (op == '+' || op == '-') return 10;
  if (op == '*' || op == '/' || op == X_IDIV || op == '%') return 11;
  if (op == '^') return 14;
  return 0;
}
int rprio(int op) {
  if (op == X_CONCAT) return 8;
  if (op == '^') return 13;
  return lprio(op);
}
char *opfn(int op) {
  if (op == '+') return "ladd"; if (op == '-') return "lsub"; if (op == '*') return "lmul";
  if (op == '/') return "ldiv"; if (op == X_IDIV) return "lidiv"; if (op == '%') return "lmod";
  if (op == '^') return "lpow"; if (op == X_CONCAT) return "lconcat";
  if (op == '&') return "lband"; if (op == '|') return "lbor"; if (op == '~') return "lbxor";
  if (op == X_SHL) return "lshl"; if (op == X_SHR) return "lshr";
  if (op == X_EQ) return "leq"; if (op == X_NE) return "lne";
  if (op == '<') return "llt"; if (op == X_LE) return "lle";
  if (op == '>') return "lgt"; return "lge";
}

void subexpr(int limit) {
  int op = tk[tp]; int a; int b; int t; char *f = 0;
  if (op == X_NOT) f = "lnot";
  else if (op == '-') f = "lunm";
  else if (op == '#') f = "llen";
  else if (op == '~') f = "lbnot";
  if (f) {
    tp++;
    if (op == '-' && (tk[tp] == X_INT || tk[tp] == X_FLOAT) && lprio(tk[tp + 1]) <= 12 && tk[tp + 1] != '^') {
      /* a negative constant */
      tp++; t = numconst(tp - 1, 1);
      dk = E_VAL; da = t;
    } else {
      subexpr(12);
      a = single();
      t = newtemp();
      e1("%t = ", t); puts_(f); e1("(%t);\n", a);
      dk = E_VAL; da = t;
    }
  } else simpleexp();
  op = tk[tp];
  while (lprio(op) > limit) {
    tp++;
    a = single();
    if (op == X_AND || op == X_OR) {
      t = newtemp();
      e2("%t = %t;\n", t, a);
      if (op == X_AND) e1("if (%t & -3) {\n", t); else e1("if (!(%t & -3)) {\n", t);
      subexpr(rprio(op));
      b = single();
      e2("%t = %t;\n}\n", t, b);
    } else {
      subexpr(rprio(op));
      b = single();
      t = newtemp();
      e1("%t = ", t); puts_(opfn(op)); e2("(%t, %t);\n", a, b);
    }
    dk = E_VAL; da = t;
    op = tk[tp];
  }
}

void expr() { subexpr(0); }

/* ----------------------------------------------------------- statements */

int blockend() {
  int t = tk[tp];
  return t == X_EOF || t == X_END || t == X_ELSE || t == X_ELSEIF || t == X_UNTIL;
}

int tgk[MAXSTK]; int tga[MAXSTK]; int tgb[MAXSTK]; int ntg;

void exprstat() {
  int first = ntg; int n; int out; int i;
  suffixedexp();
  if (tk[tp] != '=' && tk[tp] != ',') {
    if (dk != E_CALL) die("syntax error");
    return;   /* a call statement; its results are dropped */
  }
  while (1) {
    if (dk == E_CALL || dk == E_VAL) die("cannot assign to this expression");
    if (ntg >= MAXSTK) die("too many assignment targets");
    tgk[ntg] = dk; tga[ntg] = da; tgb[ntg] = db; ntg++;
    if (!accept(',')) break;
    suffixedexp();
  }
  expect('=', "'='");
  n = ntg - first;
  out = nvt; nvt = nvt + n;
  if (nvt > MAXSTK) die("too many values");
  explist_n(n, out);
  for (i = 0; i < n; i++) store(tgk[first + i], tga[first + i], tgb[first + i], vt[out + i]);
  nvt = out; ntg = first;
}

void localstat() {
  int first = ntg; int n; int out; int i; int c; int a;
  do {
    i = checkname();
    if (ntg >= MAXSTK) die("too many names");
    tga[ntg++] = i;
    if (accept('<')) { checkname(); expect('>', "'>'"); }   /* <const>, <close> */
  } while (accept(','));
  n = ntg - first;
  out = nvt; nvt = nvt + n;
  if (nvt > MAXSTK) die("too many values");
  if (accept('=')) explist_n(n, out);
  else for (i = 0; i < n; i++) { vt[out + i] = newtemp(); e1("%t = 0;\n", vt[out + i]); }
  for (i = 0; i < n; i++) {   /* the new locals come into scope only now */
    a = nact;
    c = declare(tv[tga[first + i]], tl[tga[first + i]]);
    if (iscapt(a)) e2("%l = newcell(%t);\n", c, vt[out + i]);
    else e2("%l = %t;\n", c, vt[out + i]);
  }
  nvt = out; ntg = first;
}

void localfunc() {
  int n = checkname(); int a = nact; int c = declare(tv[n], tl[n]); int f;
  if (iscapt(a)) e1("%l = newcell(0);\n", c);
  f = funcbody(0, tline[n]);
  if (iscapt(a)) e2("cset(%l, %t);\n", c, f); else e2("%l = %t;\n", c, f);
}

void funcstat() {
  int n = checkname(); int k; int a; int b; int o; int key; int method = 0; int f;
  singlevar(n);
  while (tk[tp] == '.' || tk[tp] == ':') {
    method = tk[tp] == ':';
    tp++;
    o = single();
    n = checkname();
    key = newtemp(); e2("%t = K[%d];\n", key, kconst(tv[n], tl[n]));
    dk = E_INDEX; da = o; db = key;
    if (method) break;
  }
  k = dk; a = da; b = db;
  f = funcbody(method, tline[n]);
  store(k, a, b, f);
}

void ifstat() {
  int c; int nclose = 1;
  expr(); c = single();
  expect(X_THEN, "'then'");
  e1("if (%t & -3) {\n", c);
  block();
  while (accept(X_ELSEIF)) {
    e0("} else {\n");
    expr(); c = single();
    expect(X_THEN, "'then'");
    e1("if (%t & -3) {\n", c);
    nclose++;
    block();
  }
  if (accept(X_ELSE)) { e0("} else {\n"); block(); }
  expect(X_END, "'end'");
  while (nclose--) e0("}\n");
}

void forstat(int line) {
  int n = checkname(); int a; int b; int c; int i; int save = nact; int v; int f; int s; int ctl;
  int nv; int first; int base; int cnt; int out;
  if (accept('=')) {
    expr(); a = single(); expect(',', "','");
    expr(); b = single();
    if (accept(',')) { expr(); c = single(); }
    else { c = newtemp(); e1("%t = ", c); puts_(lnumstr(encint(1))); puts_("L;\n"); }
    expect(X_DO, "'do'");
    /* f: run at all; i: the variable; cnt: iterations left (-1: a float
       loop); s: the step; ctl: the limit (float loops) */
    f = newtemp(); i = newtemp(); cnt = newtemp(); s = newtemp(); ctl = newtemp();
    ef("%t = forprep(%t, %t, %t);\n", f, a, b, c);
    ef("%t = for_i; %t = for_n; %t = for_s; %t = for_l;\n", i, cnt, s, ctl);
    e1("while (%t & -3) {\n", f);
    v = nact; declare(tv[n], tl[n]);
    if (iscapt(v)) e2("%l = newcell(%t);\n", ac[v], i); else e2("%l = %t;\n", ac[v], i);
    block();
    ef("if (%t >= 0) { if (%t == 0) break; %t = %t - 1; ", cnt, cnt, cnt, cnt);
    e3("%t = forstep(%t, %t); }\n", i, i, s);
    e0("else { ");
    ef("%t = fornextf(%t, %t, %t); ", i, i, ctl, s); e1("if (!%t) break; }\n", i);
    e0("}\n");
  } else {
    first = ntg;
    tga[ntg++] = n;
    while (accept(',')) { tga[ntg++] = checkname(); }
    nv = ntg - first;
    expect(X_IN, "'in'");
    out = nvt; nvt = nvt + 3;
    explist_n(3, out);
    f = vt[out]; s = vt[out + 1]; ctl = vt[out + 2];
    nvt = out;
    expect(X_DO, "'do'");
    e0("while (1) {\n");
    base = newtemp(); cnt = newtemp();
    e1("%t = top;\n", base);
    e2("stk[top++] = %t;\nstk[top++] = %t;\n", s, ctl);
    e3("%t = lcall(%t, %t, 2);\n", cnt, f, base);
    e1("top = %t;\n", base);
    e2("if (!%t || !stk[%t]) break;\n", cnt, base);
    e2("%t = stk[%t];\n", ctl, base);
    for (i = 0; i < nv; i++) {
      v = nact; declare(tv[tga[first + i]], tl[tga[first + i]]);
      if (iscapt(v)) ef("%l = newcell(%t > %d ? stk[%t + ", ac[v], cnt, i, base);
      else ef("%l = %t > %d ? stk[%t + ", ac[v], cnt, i, base);
      if (iscapt(v)) e1("%d] : 0);\n", i); else e1("%d] : 0;\n", i);
    }
    ntg = first;
    block();
    e0("}\n");
  }
  nact = save;
  expect(X_END, "'end'");
}

void retstat() {
  int b; int t;
  if (blockend() || tk[tp] == ';') { accept(';'); e0("return 0;\n"); return; }
  b = newtemp();
  e1("%t = top;\n", b);
  explist_push();
  accept(';');
  e2("return lret(base, %t, top - %t);\n", b, b);
}

int loopdepth[MAXLV];

void statement() {
  int t = tk[tp]; int save = ntemp[fl]; int c; int line = tline[tp]; int s;
  if (accept(';')) {}
  else if (accept(X_IF)) ifstat();
  else if (accept(X_WHILE)) {
    e0("while (1) {\n");
    expr(); c = single();
    expect(X_DO, "'do'");
    e1("if (!(%t & -3)) break;\n", c);
    loopdepth[fl]++; block(); loopdepth[fl]--;
    expect(X_END, "'end'");
    e0("}\n");
  } else if (accept(X_DO)) {
    block();
    expect(X_END, "'end'");
  } else if (accept(X_FOR)) {
    loopdepth[fl]++; forstat(line); loopdepth[fl]--;
  } else if (accept(X_REPEAT)) {
    e0("while (1) {\n");
    s = nact;
    loopdepth[fl]++;
    while (!blockend()) {
      if (tk[tp] == X_RETURN) { tp++; retstat(); break; }
      statement();
    }
    loopdepth[fl]--;
    expect(X_UNTIL, "'until'");
    expr(); c = single();
    nact = s;
    e1("if (%t & -3) break;\n}\n", c);
  } else if (accept(X_FUNCTION)) funcstat();
  else if (accept(X_LOCAL)) {
    if (accept(X_FUNCTION)) localfunc(); else localstat();
  } else if (accept(X_RETURN)) retstat();
  else if (accept(X_BREAK)) {
    if (!loopdepth[fl]) die("break outside a loop");
    e0("break;\n");
  } else if (t == X_GOTO || t == X_DBCOLON) die("goto is not supported");
  else exprstat();
  ntemp[fl] = save;
}

void block() {
  int save = nact;
  while (!blockend()) {
    if (tk[tp] == X_RETURN) { tp++; retstat(); break; }
    statement();
  }
  nact = save;
}

int selfname;

/* parse a function body; emit its C function; return a temp holding the
 * new closure (in the enclosing function) */
int funcbody(int method, int line) {
  int f = nfuncs++; int save = nact; int i; int a; int c; int n; int t; int base;
  if (fl + 1 >= MAXLV) die("functions nested too deeply");
  fl++;
  fnum[fl] = f; ntemp[fl] = 0; maxtemp[fl] = 0; nloc[fl] = 0; nup[fl] = 0;
  np[fl] = 0; isva[fl] = 0; llen[fl] = 0; loopdepth[fl] = 0;
  if (method == 1) {
    a = nact; c = declare(selfname, 4);
    if (iscapt(a)) e1("%l = newcell(nargs > 0 ? stk[base] : 0);\n", c);
    else e1("%l = nargs > 0 ? stk[base] : 0;\n", c);
    np[fl]++;
  }
  if (method == 2) isva[fl] = 1;   /* a chunk, from load */
  else expect('(', "'('");
  if (method != 2 && !accept(')')) {
    do {
      if (accept(X_DOTS)) { isva[fl] = 1; break; }
      n = checkname();
      a = nact; c = declare(tv[n], tl[n]);
      if (iscapt(a)) ef("%l = newcell(nargs > %d ? stk[base + %d] : 0);\n", c, np[fl], np[fl], 0);
      else e3("%l = nargs > %d ? stk[base + %d] : 0;\n", c, np[fl], np[fl]);
      np[fl]++;
    } while (accept(','));
    expect(')', "')'");
  }
  block();
  if (method == 2) expect(X_EOF, "end of chunk"); else expect(X_END, "'end'");
  e0("return 0;\n");
  /* the C function: header, declarations, body */
  if (gen) {
    fput("\nint F"); fput(numstr(f)); fput("(int clo, int base, int nargs) {\n");
    fput("top = base + nargs;\n");
    for (i = 0; i < maxtemp[fl]; i++) { fput("long T"); fput(numstr(i)); fput(";\n"); }
    for (i = 0; i < nloc[fl]; i++) { fput("long L"); fput(numstr(i)); fput(";\n"); }
    fputs_(lbuf + fl * LBUF, llen[fl]);
    fput("}\n");
  }
  n = nup[fl];
  fl--;
  nact = save;
  t = newtemp();
  e3("%t = mkclo(F%d, %d);\n", t, f, n);
  for (i = 0; i < n; i++) {
    if (upkind[(fl + 1) * MAXUP + i] == 0) e3("setuv(%t, %d, %l);\n", t, i, upsrc[(fl + 1) * MAXUP + i]);
    else e3("setuv(%t, %d, uvcell(clo, %d));\n", t, i, upsrc[(fl + 1) * MAXUP + i]);
  }
  return t;
}

/* the main chunk is function 0, a vararg function */
void chunk() {
  int i;
  tp = 0; nfuncs = 1; fl = 0; nact = 0; ndecl = 0; nk = 0; ntg = 0; nvt = 0;
  fnum[0] = 0; ntemp[0] = 0; maxtemp[0] = 0; nloc[0] = 0; nup[0] = 0; np[0] = 0; isva[0] = 1;
  llen[0] = 0; loopdepth[0] = 0;
  block();
  if (tk[tp] != X_EOF) die("'<eof>' expected");
  e0("return 0;\n");
  if (gen) {
    fput("\nint F0(int clo, int base, int nargs) {\n");
    fput("top = base + nargs;\n");
    for (i = 0; i < maxtemp[0]; i++) { fput("long T"); fput(numstr(i)); fput(";\n"); }
    for (i = 0; i < nloc[0]; i++) { fput("long L"); fput(numstr(i)); fput(";\n"); }
    fputs_(lbuf, llen[0]);
    fput("}\n");
  }
}


void kstring(int k) {
  int i; int c; char *h = "0123456789abcdef";
  fput("  K["); fput(numstr(k)); fput("] = lstr(\"");
  for (i = 0; i < klen[k]; i++) {
    c = pool[kpos[k] + i] & 255;
    if (c >= 32 && c < 127 && c != '"' && c != '\\') fputs_(pool + kpos[k] + i, 1);
    else { fput("\\x"); fputs_(h + (c >> 4), 1); fputs_(h + (c & 15), 1); }
  }
  fput("\", "); fput(numstr(klen[k])); fput(");\n");
}

int main() {
  int n; int i;
  while ((n = sys_read(src + srclen, MAXSRC - 1 - srclen)) > 0) srclen = srclen + n;
  src[srclen] = 0;
  srclen++;
  lex(0);
  selfname = npool;
  poolc('s'); poolc('e'); poolc('l'); poolc('f');
  gen = 0; chunk();   /* pass one: which locals are captured */
  gen = 1; chunk();
  fput("\nvoid lua_consts() {\n");
  for (i = 0; i < nk; i++) kstring(i);
  fput("}\n");
  n = 0;
  sys_write(1, "long K[", 7);
  sys_write(1, numstr(nk + 1), cstrlen(numstr(nk + 1)));
  sys_write(1, "];\n", 3);
  while (n < fout_n) n = n + sys_write(1, fout + n, fout_n - n);
  return 0;
}
