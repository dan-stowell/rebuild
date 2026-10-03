/* cc.c -- a compiler for a small subset of C, written in that subset.
 *
 * Reads one translation unit on stdin, writes a wasm module on stdout.
 * Single pass: the parser emits wasm as it goes.  See c/README.md for the
 * subset.  Types: int, char, void, and pointers to them; global arrays.
 */

enum {
  MAXSRC = 1048576, MAXTOK = 200000, MAXSYM = 20000, MAXFN = 4000,
  MAXCODE = 1048576, MAXDATA = 262144, MAXREL = 100000, MAXOUT = 2097152,
  DATA_BASE = 1024
};

int sys_read(char *buf, int n);
int sys_write(int fd, char *buf, int n);

/* ------------------------------------------------------------ utilities */

int cstrlen(char *s) { int n = 0; while (s[n]) n++; return n; }

int same(char *a, char *b, int n) {
  int i = 0;
  while (i < n) { if (a[i] != b[i]) return 0; i++; }
  return 1;
}

void eputs(char *s) { sys_write(2, s, cstrlen(s)); }

char nbuf[16];
void eputnum(int n) {
  int i = 15;
  if (n == 0) { eputs("0"); return; }
  nbuf[i] = 0;
  while (n > 0) { i--; nbuf[i] = '0' + n % 10; n = n / 10; }
  eputs(nbuf + i);
}

/* ---------------------------------------------------------------- lexer */

enum {
  T_EOF = 256, T_NUM, T_STR, T_ID,
  T_INT, T_CHAR, T_VOID, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_RETURN,
  T_BREAK, T_CONTINUE, T_SIZEOF, T_ENUM, T_STATIC, T_CONST,
  T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR, T_INC, T_DEC, T_SHL, T_SHR,
  T_ADDA, T_SUBA, T_MULA, T_DIVA, T_MODA, T_ANDA, T_ORA, T_XORA,
  T_SHLA, T_SHRA
};

char *keywords =
  "int char void if else while for do return break continue sizeof enum static const ";
char *puncts = "==!=<=>=&&||++--<<>>+=-=*=/=%=&=|=^=";

char src[MAXSRC];
int srclen;

int tk[MAXTOK];      /* kind */
int tv[MAXTOK];      /* value: number, string address, or name offset */
int tl[MAXTOK];      /* name length */
int tline[MAXTOK];
int ntok;
int tp;              /* the current token */

char data[MAXDATA];  /* initialized memory, at DATA_BASE */
int datalen;

void die(char *msg) {
  eputs("cc: line "); eputnum(tline[tp]); eputs(": "); eputs(msg); eputs("\n");
  __builtin_trap();
}

int isdig(int c) { return c >= '0' && c <= '9'; }
int isid(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || isdig(c);
}

int keyword(int p, int n) {
  char *k = keywords;
  int t = T_INT;
  int i;
  while (*k) {
    i = 0;
    while (k[i] != ' ') i++;
    if (i == n && same(src + p, k, n)) return t;
    k = k + i + 1;
    t++;
  }
  return T_ID;
}

int escape(int c) {
  if (c == 'n') return 10;
  if (c == 't') return 9;
  if (c == 'r') return 13;
  if (c == '0') return 0;
  return c;
}

void lex() {
  int i = 0;
  int line = 1;
  int c;
  int v;
  int k;
  while (1) {
    c = src[i];
    if (c == 0) break;
    if (c == '\n') { line++; i++; continue; }
    if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
    if (c == '#' || (c == '/' && src[i + 1] == '/')) {
      while (src[i] && src[i] != '\n') i++;
      continue;
    }
    if (c == '/' && src[i + 1] == '*') {
      i = i + 2;
      while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) {
        if (src[i] == '\n') line++;
        i++;
      }
      i = i + 2;
      continue;
    }
    if (ntok >= MAXTOK - 1) die("too many tokens");
    tline[ntok] = line;
    if (isdig(c)) {
      v = 0;
      if (c == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i = i + 2;
        while (1) {
          c = src[i];
          if (isdig(c)) v = v * 16 + c - '0';
          else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
          else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
          else break;
          i++;
        }
      } else {
        while (isdig(src[i])) { v = v * 10 + src[i] - '0'; i++; }
      }
      tk[ntok] = T_NUM; tv[ntok] = v;
    } else if (isid(c)) {
      k = i;
      while (isid(src[i])) i++;
      tk[ntok] = keyword(k, i - k); tv[ntok] = k; tl[ntok] = i - k;
    } else if (c == '\'') {
      c = src[i + 1];
      i = i + 2;
      if (c == '\\') { c = escape(src[i]); i++; }
      i++;
      tk[ntok] = T_NUM; tv[ntok] = c;
    } else if (c == '"') {
      i++;
      tk[ntok] = T_STR; tv[ntok] = DATA_BASE + datalen;
      while (src[i] != '"') {
        if (!src[i]) die("unterminated string");
        c = src[i++];
        if (c == '\\') c = escape(src[i++]);
        data[datalen++] = c;
      }
      i++;
      data[datalen++] = 0;
    } else {
      if ((c == '<' || c == '>') && src[i + 1] == c && src[i + 2] == '=') {
        tk[ntok] = c == '<' ? T_SHLA : T_SHRA;
        i = i + 3;
      } else {
        k = 0;
        while (puncts[k] && !(puncts[k] == c && puncts[k + 1] == src[i + 1])) k = k + 2;
        if (puncts[k]) { tk[ntok] = T_EQ + k / 2; i = i + 2; }
        else { tk[ntok] = c; i++; }
      }
    }
    ntok++;
  }
  tk[ntok] = T_EOF; tline[ntok] = line;
}

int accept(int t) { if (tk[tp] == t) { tp++; return 1; } return 0; }
void expect(int t) { if (!accept(t)) die("syntax error"); }

/* ------------------------------------------------------------- emitting */

char code[MAXCODE];  /* the code section's bodies */
int clen;
char out[MAXOUT];    /* the module */
int olen;
int to_out;          /* emit to out instead of code */

void emit(int b) {
  if (to_out) out[olen++] = b; else code[clen++] = b;
}
void uleb(int v) {
  while (v & ~127) { emit(v & 127 | 128); v = (v >> 7) & 33554431; }
  emit(v);
}
void sleb(int v) {
  int b;
  while (1) {
    b = v & 127;
    v = v >> 7;
    if ((v == 0 && !(b & 64)) || (v == -1 && (b & 64))) { emit(b); return; }
    emit(b | 128);
  }
}
void pad5(int v) {  /* a 5-byte LEB128, so that it can be patched */
  emit(v & 127 | 128); emit(v >> 7 & 127 | 128); emit(v >> 14 & 127 | 128);
  emit(v >> 21 & 127 | 128); emit(v >> 28 & 15);
}
void patch5(char *b, int p, int v) {
  b[p] = v & 127 | 128; b[p + 1] = v >> 7 & 127 | 128; b[p + 2] = v >> 14 & 127 | 128;
  b[p + 3] = v >> 21 & 127 | 128; b[p + 4] = v >> 28 & 15;
}
void emit2(int a, int b) { emit(a); emit(b); }
void emit3(int a, int b, int c) { emit(a); emit(b); emit(c); }
void iconst(int v) { emit(0x41); sleb(v); }

/* relocations: patched once function indices and memory layout are known */
enum { R_FUNC = 1, R_BSS, R_HEAP };
int rpos[MAXREL]; int rkind[MAXREL]; int rval[MAXREL];
int nrel;
void reloc(int kind, int v) {
  if (nrel >= MAXREL) die("too many relocations");
  rpos[nrel] = clen; rkind[nrel] = kind; rval[nrel] = v; nrel++;
  pad5(0);
}

/* -------------------------------------------------------------- symbols */

enum { S_LOCAL = 1, S_GLOBAL, S_FUNC, S_CONST };
int sname[MAXSYM]; int slen[MAXSYM]; int skind[MAXSYM]; int stype[MAXSYM];
int sval[MAXSYM];  /* local index, address, bss offset, function, value */
int sarr[MAXSYM];  /* array length, or 0 */
int sbss[MAXSYM];  /* global lives in bss: sval is an offset */
int nsym;

int lookup(int p, int n) {
  int i = nsym - 1;
  while (i >= 0) {
    if (slen[i] == n && same(src + sname[i], src + p, n)) return i;
    i--;
  }
  return -1;
}

int addsym(int p, int n, int kind, int type, int v) {
  if (nsym >= MAXSYM) die("too many symbols");
  sname[nsym] = p; slen[nsym] = n; skind[nsym] = kind; stype[nsym] = type;
  sval[nsym] = v; sarr[nsym] = 0; sbss[nsym] = 0;
  return nsym++;
}

int nameis(int p, int n, char *s) { return n == cstrlen(s) && same(src + p, s, n); }

int fname[MAXFN]; int fnlen[MAXFN]; int fnp[MAXFN]; int fret[MAXFN];
int fdef[MAXFN]; int ford[MAXFN]; int fimp[MAXFN]; int byord[MAXFN];
int nfn; int ndef; int nimp;
int mainf = -1;

int bsslen;

/* ---------------------------------------------------------------- types */

/* a type is base + 4 * pointer depth */
enum { TY_VOID = 0, TY_CHAR = 1, TY_INT = 2 };

int isptr(int t) { return t >= 4; }
int tsize(int t) { if (t == TY_CHAR || t == TY_VOID) return 1; return 4; }

int istype(int i) {
  int t = tk[i];
  return t == T_INT || t == T_CHAR || t == T_VOID || t == T_STATIC || t == T_CONST;
}
int basetype() {
  int t;
  while (accept(T_STATIC) || accept(T_CONST)) ;
  if (accept(T_INT)) t = TY_INT;
  else if (accept(T_CHAR)) t = TY_CHAR;
  else if (accept(T_VOID)) t = TY_VOID;
  else die("expected a type");
  while (accept(T_CONST)) ;
  return t;
}
int stars(int t) { while (accept('*')) t = t + 4; return t; }

/* ---------------------------------------------------------- expressions */

/* After parsing an expression, lv says where its value is:
 * 0: on the wasm stack; 1: in wasm local lvi; 2: in memory, address on
 * the stack.  rv() turns the latter two into the first. */
int lv; int lvi;
int tmpa; int tmpb;  /* scratch locals */
int nloc;

void load(int t) { if (t == TY_CHAR) emit3(0x2c, 0, 0); else emit3(0x28, 2, 0); }
void store(int t) { if (t == TY_CHAR) emit3(0x3a, 0, 0); else emit3(0x36, 2, 0); }
void rv(int t) {
  if (lv == 1) { emit(0x20); uleb(lvi); }
  else if (lv == 2) load(t);
  lv = 0;
}
void sext(int t) {   /* a char held in a local */
  if (t == TY_CHAR) { iconst(24); emit(0x74); iconst(24); emit(0x75); }
}
void scale(int t) { int s = tsize(t); if (s != 1) { iconst(s); emit(0x6c); } }
void lget(int i) { emit(0x20); uleb(i); }
void lset(int i) { emit(0x21); uleb(i); }
void ltee(int i) { emit(0x22); uleb(i); }

int expr();
int assign();
int cexpr();

int call(int p, int n) {
  int s; int f; int np = 0; int t;
  if (nameis(p, n, "__builtin_trap")) { expect('('); expect(')'); emit(0); return TY_VOID; }
  if (nameis(p, n, "__memory_size")) { expect('('); expect(')'); emit2(0x3f, 0); return TY_INT; }
  if (nameis(p, n, "__memory_grow")) {
    expect('('); t = assign(); rv(t); expect(')'); emit2(0x40, 0); return TY_INT;
  }
  if (nameis(p, n, "__heap_base")) { expect('('); expect(')'); emit(0x41); reloc(R_HEAP, 0); return TY_CHAR + 4; }
  s = lookup(p, n);
  if (s < 0 || skind[s] != S_FUNC) die("call of undeclared function");
  f = sval[s];
  expect('(');
  if (!accept(')')) {
    do { t = assign(); rv(t); np++; } while (accept(','));
    expect(')');
  }
  if (np != fnp[f]) die("wrong number of arguments");
  emit(0x10); reloc(R_FUNC, f);
  lv = 0;
  return fret[f];
}

int primary() {
  int t = tk[tp]; int s; int p; int n;
  lv = 0;
  if (t == T_NUM) { tp++; iconst(tv[tp - 1]); return TY_INT; }
  if (t == T_STR) { tp++; iconst(tv[tp - 1]); return TY_CHAR + 4; }
  if (t == '(') { tp++; t = expr(); expect(')'); return t; }
  if (t != T_ID) die("expected an expression");
  p = tv[tp]; n = tl[tp]; tp++;
  if (tk[tp] == '(') return call(p, n);
  s = lookup(p, n);
  if (s < 0) die("undeclared identifier");
  if (skind[s] == S_CONST) { iconst(sval[s]); return TY_INT; }
  if (skind[s] == S_LOCAL) { lv = 1; lvi = sval[s]; return stype[s]; }
  if (skind[s] != S_GLOBAL) die("not a variable");
  emit(0x41);
  if (sbss[s]) reloc(R_BSS, sval[s]); else sleb(sval[s]);
  if (sarr[s]) return stype[s] + 4;
  lv = 2;
  return stype[s];
}

/* ++x, --x, x++, x-- */
int incdec(int t, int d, int post) {
  if (isptr(t)) d = d * tsize(t - 4);
  if (lv == 1) {
    if (post) { lget(lvi); lget(lvi); iconst(d); emit(0x6a); sext(t); lset(lvi); }
    else { lget(lvi); iconst(d); emit(0x6a); sext(t); ltee(lvi); }
  } else if (lv == 2) {
    ltee(tmpa); lget(tmpa); load(t);
    if (post) { ltee(tmpb); iconst(d); emit(0x6a); store(t); lget(tmpb); }
    else { iconst(d); emit(0x6a); ltee(tmpb); store(t); lget(tmpb); }
  } else die("not an lvalue");
  lv = 0;
  return t;
}

int postfix() {
  int t = primary(); int t2;
  while (1) {
    if (accept('[')) {
      rv(t);
      if (!isptr(t)) die("subscript of a non-pointer");
      t2 = expr(); rv(t2);
      scale(t - 4);
      emit(0x6a);
      expect(']');
      t = t - 4; lv = 2;
    } else if (accept(T_INC)) t = incdec(t, 1, 1);
    else if (accept(T_DEC)) t = incdec(t, -1, 1);
    else return t;
  }
}

int unary() {
  int t = tk[tp];
  if (accept('-')) {
    if (tk[tp] == T_NUM) { tp++; iconst(-tv[tp - 1]); lv = 0; return TY_INT; }
    iconst(0); t = unary(); rv(t); emit(0x6b); return TY_INT;
  }
  if (accept('!')) { t = unary(); rv(t); emit(0x45); return TY_INT; }
  if (accept('~')) { t = unary(); rv(t); iconst(-1); emit(0x73); return TY_INT; }
  if (accept('*')) {
    t = unary(); rv(t);
    if (!isptr(t)) die("dereference of a non-pointer");
    lv = 2;
    return t - 4;
  }
  if (accept('&')) {
    t = unary();
    if (lv != 2) die("cannot take this address");
    lv = 0;
    return t + 4;
  }
  if (accept(T_INC)) { t = unary(); return incdec(t, 1, 0); }
  if (accept(T_DEC)) { t = unary(); return incdec(t, -1, 0); }
  if (accept(T_SIZEOF)) {
    expect('('); t = stars(basetype()); expect(')');
    iconst(tsize(t)); lv = 0;
    return TY_INT;
  }
  if (t == '(' && istype(tp + 1)) {
    tp++; t = stars(basetype()); expect(')');
    rv(unary());
    return t;
  }
  return postfix();
}

int prec(int t) {
  if (t == T_OR) return 1;
  if (t == T_AND) return 2;
  if (t == '|') return 3;
  if (t == '^') return 4;
  if (t == '&') return 5;
  if (t == T_EQ || t == T_NE) return 6;
  if (t == '<' || t == '>' || t == T_LE || t == T_GE) return 7;
  if (t == T_SHL || t == T_SHR) return 8;
  if (t == '+' || t == '-') return 9;
  if (t == '*' || t == '/' || t == '%') return 10;
  return 0;
}

int opcode(int t) {
  if (t == '+' || t == T_ADDA) return 0x6a;
  if (t == '-' || t == T_SUBA) return 0x6b;
  if (t == '*' || t == T_MULA) return 0x6c;
  if (t == '/' || t == T_DIVA) return 0x6d;
  if (t == '%' || t == T_MODA) return 0x6f;
  if (t == '&' || t == T_ANDA) return 0x71;
  if (t == '|' || t == T_ORA) return 0x72;
  if (t == '^' || t == T_XORA) return 0x73;
  if (t == T_SHL || t == T_SHLA) return 0x74;
  if (t == T_SHR || t == T_SHRA) return 0x75;
  if (t == T_EQ) return 0x46;
  if (t == T_NE) return 0x47;
  if (t == '<') return 0x48;
  if (t == '>') return 0x4a;
  if (t == T_LE) return 0x4c;
  return 0x4e;
}

int binary(int minp) {
  int t = unary(); int op; int p; int t2;
  while (1) {
    op = tk[tp]; p = prec(op);
    if (p == 0 || p < minp) return t;
    tp++;
    rv(t);
    if (op == T_AND) {
      emit2(0x04, 0x7f); t2 = binary(p + 1); rv(t2); iconst(0); emit(0x47);
      emit(0x05); iconst(0); emit(0x0b);
      t = TY_INT;
    } else if (op == T_OR) {
      emit2(0x04, 0x7f); iconst(1);
      emit(0x05); t2 = binary(p + 1); rv(t2); iconst(0); emit(0x47); emit(0x0b);
      t = TY_INT;
    } else {
      t2 = binary(p + 1); rv(t2);
      if ((op == '+' || op == '-') && isptr(t) && !isptr(t2)) scale(t - 4);
      emit(opcode(op));
      if (op == '-' && isptr(t) && isptr(t2)) {
        if (tsize(t - 4) != 1) { iconst(tsize(t - 4)); emit(0x6d); }
        t = TY_INT;
      } else if (!((op == '+' || op == '-') && isptr(t))) t = TY_INT;
    }
  }
}

int cond() {
  int t = binary(1); int t2;
  if (!accept('?')) return t;
  rv(t);
  emit2(0x04, 0x7f);
  t = expr(); rv(t);
  expect(':');
  emit(0x05);
  t2 = cond(); rv(t2);
  emit(0x0b);
  return t;
}

int assign() {
  int t = cond(); int op = tk[tp]; int t2; int i;
  if (op != '=' && (op < T_ADDA || op > T_SHRA)) return t;
  tp++;
  if (lv == 1) {
    i = lvi;
    if (op != '=') lget(i);
    t2 = assign(); rv(t2);
    if (op != '=') {
      if (isptr(t) && (op == T_ADDA || op == T_SUBA)) scale(t - 4);
      emit(opcode(op));
    }
    sext(t);
    ltee(i);
  } else if (lv == 2) {
    if (op != '=') { ltee(tmpa); lget(tmpa); load(t); }
    t2 = assign(); rv(t2);
    if (op != '=') {
      if (isptr(t) && (op == T_ADDA || op == T_SUBA)) scale(t - 4);
      emit(opcode(op));
    }
    ltee(tmpb); store(t); lget(tmpb);
  } else die("not an lvalue");
  lv = 0;
  return t;
}

int expr() { return assign(); }

/* constant expressions: array sizes, enum values, global initializers */
int cunary() {
  int s; int v;
  if (accept('-')) return -cunary();
  if (accept('~')) return ~cunary();
  if (accept('!')) return !cunary();
  if (accept('(')) { v = cexpr(); expect(')'); return v; }
  if (accept(T_SIZEOF)) { expect('('); v = tsize(stars(basetype())); expect(')'); return v; }
  if (tk[tp] == T_NUM) { tp++; return tv[tp - 1]; }
  if (tk[tp] == T_ID) {
    s = lookup(tv[tp], tl[tp]);
    if (s >= 0 && skind[s] == S_CONST) { tp++; return sval[s]; }
  }
  die("expected a constant");
  return 0;
}
int cbinary(int minp) {
  int v = cunary(); int op; int p; int r;
  while (1) {
    op = tk[tp]; p = prec(op);
    if (p == 0 || p < minp) return v;
    tp++;
    r = cbinary(p + 1);
    if (op == '+') v = v + r;
    else if (op == '-') v = v - r;
    else if (op == '*') v = v * r;
    else if (op == '/') v = v / r;
    else if (op == '%') v = v % r;
    else if (op == '&') v = v & r;
    else if (op == '|') v = v | r;
    else if (op == '^') v = v ^ r;
    else if (op == T_SHL) v = v << r;
    else if (op == T_SHR) v = v >> r;
    else if (op == T_EQ) v = v == r;
    else if (op == T_NE) v = v != r;
    else if (op == '<') v = v < r;
    else if (op == '>') v = v > r;
    else if (op == T_LE) v = v <= r;
    else if (op == T_GE) v = v >= r;
    else if (op == T_AND) v = v && r;
    else v = v || r;
  }
}
int cexpr() { return cbinary(1); }

/* ----------------------------------------------------------- statements */

int depth;           /* wasm blocks open in this function */
int brk; int cont;   /* the depths that break and continue go to */

void discard(int t) {
  if (lv == 2 || (lv == 0 && t != TY_VOID)) emit(0x1a);
  lv = 0;
}

void decl() {
  int bt = basetype(); int t; int i;
  do {
    t = stars(bt);
    if (tk[tp] != T_ID) die("expected a name");
    if (tk[tp + 1] == '[') die("local arrays are not supported");
    i = nloc++;
    addsym(tv[tp], tl[tp], S_LOCAL, t, i);
    tp++;
    if (accept('=')) { rv(assign()); sext(t); lset(i); }
    else { iconst(0); lset(i); }
  } while (accept(','));
  expect(';');
}

void stmt() {
  int t; int save; int ob = brk; int oc = cont; int top; int step; int n;
  if (accept('{')) {
    save = nsym;
    while (!accept('}')) { if (istype(tp)) decl(); else stmt(); }
    nsym = save;
  } else if (accept(T_IF)) {
    expect('('); rv(expr()); expect(')');
    emit2(0x04, 0x40); depth++;
    stmt();
    if (accept(T_ELSE)) { emit(0x05); stmt(); }
    emit(0x0b); depth--;
  } else if (accept(T_WHILE)) {
    emit2(0x02, 0x40); emit2(0x03, 0x40); depth = depth + 2;
    brk = depth - 1; cont = depth;
    expect('('); rv(expr()); expect(')');
    emit2(0x45, 0x0d); uleb(depth - brk);
    stmt();
    emit(0x0c); uleb(depth - cont);
    emit2(0x0b, 0x0b); depth = depth - 2;
  } else if (accept(T_DO)) {
    emit2(0x02, 0x40); emit2(0x03, 0x40); emit2(0x02, 0x40); depth = depth + 3;
    brk = depth - 2; top = depth - 1; cont = depth;
    stmt();
    emit(0x0b); depth--;
    expect(T_WHILE); expect('('); rv(expr()); expect(')'); expect(';');
    emit(0x0d); uleb(depth - top);
    emit2(0x0b, 0x0b); depth = depth - 2;
  } else if (accept(T_FOR)) {
    expect('(');
    if (!accept(';')) { discard(expr()); expect(';'); }
    emit2(0x02, 0x40); emit2(0x03, 0x40); depth = depth + 2;
    brk = depth - 1; top = depth;
    if (!accept(';')) {
      rv(expr()); expect(';');
      emit2(0x45, 0x0d); uleb(depth - brk);
    }
    step = tp; n = 0;   /* skip the step; it is compiled after the body */
    while (n > 0 || tk[tp] != ')') {
      if (tk[tp] == '(') n++;
      if (tk[tp] == ')') n--;
      if (tk[tp] == T_EOF) die("unterminated for");
      tp++;
    }
    tp++;
    emit2(0x02, 0x40); depth++; cont = depth;
    stmt();
    emit(0x0b); depth--;
    if (tk[step] != ')') { save = tp; tp = step; discard(expr()); tp = save; }
    emit(0x0c); uleb(depth - top);
    emit2(0x0b, 0x0b); depth = depth - 2;
  } else if (accept(T_RETURN)) {
    if (!accept(';')) { rv(expr()); expect(';'); }
    emit(0x0f);
  } else if (accept(T_BREAK)) {
    expect(';'); emit(0x0c); uleb(depth - brk);
  } else if (accept(T_CONTINUE)) {
    expect(';'); emit(0x0c); uleb(depth - cont);
  } else if (!accept(';')) {
    discard(expr()); expect(';');
  }
  brk = ob; cont = oc;
}

/* ------------------------------------------------------------ top level */

int function(int ret, int p, int n) {
  int s = lookup(p, n); int f; int np = 0; int save; int t; int body; int locs;
  if (s >= 0 && skind[s] == S_FUNC) f = sval[s];
  else {
    if (nfn >= MAXFN) die("too many functions");
    f = nfn++;
    addsym(p, n, S_FUNC, ret, f);
    fname[f] = p; fnlen[f] = n; fret[f] = ret; fdef[f] = 0;
  }
  if (nameis(p, n, "main")) mainf = f;
  expect('(');
  save = nsym;
  if (tk[tp] == T_VOID && tk[tp + 1] == ')') tp++;
  if (!accept(')')) {
    do {
      t = stars(basetype());
      if (tk[tp] == T_ID) { addsym(tv[tp], tl[tp], S_LOCAL, t, np); tp++; }
      np++;
    } while (accept(','));
    expect(')');
  }
  fnp[f] = np;
  if (tk[tp] != '{') { nsym = save; return 0; }
  if (fdef[f]) die("function redefined");
  fdef[f] = 1; ford[f] = ndef; byord[ndef] = f; ndef++;
  tmpa = np; tmpb = np + 1; nloc = np + 2; depth = 0;
  body = clen; pad5(0);
  emit(1); locs = clen; pad5(0); emit(0x7f);
  stmt();
  if (ret != TY_VOID) iconst(0);
  emit(0x0b);
  patch5(code, locs, nloc - np);
  patch5(code, body, clen - body - 5);
  nsym = save;
  return 1;
}

void global(int t, int p, int n) {
  int len = 0; int s; int v; int a;
  if (accept('[')) { len = cexpr(); expect(']'); }
  if (accept('=')) {
    if (len) die("array initializers are not supported");
    if (tk[tp] == T_STR) { v = tv[tp]; tp++; } else v = cexpr();
    a = tsize(t);
    while (datalen % a) datalen++;
    s = addsym(p, n, S_GLOBAL, t, DATA_BASE + datalen);
    data[datalen++] = v;
    if (a == 4) { data[datalen++] = v >> 8; data[datalen++] = v >> 16; data[datalen++] = v >> 24; }
  } else {
    while (bsslen % 4) bsslen++;
    s = addsym(p, n, S_GLOBAL, t, bsslen);
    sbss[s] = 1; sarr[s] = len;
    if (len) bsslen = bsslen + len * tsize(t); else bsslen = bsslen + tsize(t);
  }
}

void enumdecl() {
  int v = 0; int i;
  accept(T_ID);
  expect('{');
  while (!accept('}')) {
    if (tk[tp] != T_ID) die("expected a name");
    i = tp++;
    if (accept('=')) v = cexpr();
    addsym(tv[i], tl[i], S_CONST, TY_INT, v);
    v++;
    if (!accept(',')) { expect('}'); break; }
  }
  expect(';');
}

void toplevel() {
  int bt; int t; int p; int n;
  while (tk[tp] != T_EOF) {
    if (accept(T_ENUM)) { enumdecl(); continue; }
    bt = basetype();
    t = stars(bt);
    if (tk[tp] != T_ID) die("expected a name");
    p = tv[tp]; n = tl[tp]; tp++;
    if (tk[tp] == '(') {
      if (!function(t, p, n)) expect(';');
      continue;
    }
    global(t, p, n);
    while (accept(',')) {
      t = stars(bt);
      if (tk[tp] != T_ID) die("expected a name");
      tp++;
      global(t, tv[tp - 1], tl[tp - 1]);
    }
    expect(';');
  }
}

/* --------------------------------------------------------------- output */

int section(int id) { emit(id); pad5(0); return olen - 5; }
void endsection(int s) { patch5(out, s, olen - s - 5); }
void name(char *s, int n) { int i = 0; uleb(n); while (i < n) emit(s[i++]); }

void finish() {
  int f; int i; int s; int v; int bss; int heap;
  char *wasi = "wasi_snapshot_preview1";
  if (mainf < 0 || !fdef[mainf]) die("no main function");
  for (f = 0; f < nfn; f++) if (!fdef[f]) fimp[f] = nimp++;
  bss = DATA_BASE + datalen;
  while (bss % 16) bss++;
  heap = bss + bsslen;
  while (heap % 16) heap++;
  for (i = 0; i < nrel; i++) {
    v = rval[i];
    if (rkind[i] == R_FUNC) v = fdef[v] ? nimp + ford[v] : fimp[v];
    else if (rkind[i] == R_BSS) v = bss + v;
    else v = heap;
    patch5(code, rpos[i], v);
  }

  to_out = 1;
  emit(0); emit(0x61); emit(0x73); emit(0x6d); emit(1); emit(0); emit(0); emit(0);
  /* types: one per function, in function index order, then _start */
  s = section(1);
  uleb(nfn + 1);
  for (i = 0; i < nfn + 1; i++) {
    if (i == nfn) { emit3(0x60, 0, 0); continue; }
    f = i < nimp ? -1 : byord[i - nimp];
    if (f < 0) { f = 0; while (fdef[f] || fimp[f] != i) f++; }
    emit(0x60); uleb(fnp[f]);
    for (v = 0; v < fnp[f]; v++) emit(0x7f);
    if (fret[f] == TY_VOID) emit(0); else emit2(1, 0x7f);
  }
  endsection(s);
  s = section(2);
  uleb(nimp);
  for (f = 0; f < nfn; f++) {
    if (fdef[f]) continue;
    name(wasi, cstrlen(wasi));
    name(src + fname[f], fnlen[f]);
    emit(0); uleb(fimp[f]);
  }
  endsection(s);
  s = section(3);
  uleb(ndef + 1);
  for (i = 0; i <= ndef; i++) uleb(nimp + i);
  endsection(s);
  s = section(5);
  emit2(1, 0); uleb(heap / 65536 + 1);
  endsection(s);
  s = section(7);
  uleb(2);
  name("memory", 6); emit2(2, 0);
  name("_start", 6); emit(0); uleb(nimp + ndef);
  endsection(s);
  s = section(10);
  uleb(ndef + 1);
  for (i = 0; i < clen; i++) emit(code[i]);
  v = olen; pad5(0);
  emit(0); emit(0x10); uleb(nimp + ford[mainf]);
  if (fret[mainf] != TY_VOID) { emit2(0x04, 0x40); emit2(0, 0x0b); }
  emit(0x0b);
  patch5(out, v, olen - v - 5);
  endsection(s);
  s = section(11);
  uleb(1);
  emit(0); iconst(DATA_BASE); emit(0x0b);
  uleb(datalen);
  for (i = 0; i < datalen; i++) emit(data[i]);
  endsection(s);
}

int main() {
  int n;
  while ((n = sys_read(src + srclen, MAXSRC - 1 - srclen)) > 0) srclen = srclen + n;
  src[srclen] = 0;
  lex();
  toplevel();
  finish();
  n = 0;
  while (n < olen) n = n + sys_write(1, out + n, olen - n);
  return 0;
}
