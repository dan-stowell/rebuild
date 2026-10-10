/* cc.c -- a compiler for a small subset of C, written in that subset.
 *
 * Reads one translation unit on stdin, writes a wasm module on stdout.
 * Single pass: the parser emits wasm as it goes.  See c/README.md for the
 * subset.  Types: char, int, long (64-bit), double, their unsigned forms,
 * void, and pointers; global arrays.
 *
 * cc.c itself uses only int, char and pointers, so that the bootstrap
 * compiler (cc.sx) can compile it.
 */

enum {
  MAXSRC = 4194304, MAXTOK = 1000000, MAXSYM = 50000, MAXFN = 20000,
  MAXCODE = 8388608, MAXDATA = 1048576, MAXREL = 1000000, MAXOUT = 16777216,
  MAXP = 16, MAXLOC = 16384, DATA_BASE = 1024
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
  T_UNSIGNED, T_SIGNED, T_LONG, T_DOUBLE, T_SWITCH, T_CASE, T_DEFAULT,
  T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR, T_INC, T_DEC, T_SHL, T_SHR,
  T_ADDA, T_SUBA, T_MULA, T_DIVA, T_MODA, T_ANDA, T_ORA, T_XORA,
  T_SHLA, T_SHRA
};

/* a type is a base type + PTR * pointer depth */
enum { TY_VOID, TY_CHAR, TY_UCHAR, TY_INT, TY_UINT, TY_LONG, TY_ULONG, TY_DOUBLE, PTR };

char *keywords =
  "int char void if else while for do return break continue sizeof enum static const unsigned signed long double switch case default ";
char *puncts = "==!=<=>=&&||++--<<>>+=-=*=/=%=&=|=^=";

char src[MAXSRC];
int srclen;

int tk[MAXTOK];      /* kind */
int tv[MAXTOK];      /* value: number (its low 32 bits), string address, or name offset */
int th[MAXTOK];      /* a number's high 32 bits */
int tl[MAXTOK];      /* name length; a number's type */
int tline[MAXTOK];
int ntok;
int tp;              /* the current token */

char data[MAXDATA];  /* initialized memory, at DATA_BASE */
int datalen;

void die(char *msg) {
  eputs("cc: line "); eputnum(tline[tp]); eputs(": "); eputs(msg); eputs("\n");
  __builtin_trap();
}
void diename(char *msg, int p, int n) {   /* die, naming the identifier at src[p..p+n) */
  eputs("cc: line "); eputnum(tline[tp]); eputs(": "); eputs(msg); eputs(": ");
  sys_write(2, src + p, n); eputs("\n");
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

int hexval(int c) {
  if (isdig(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* a 64-bit number in four 16-bit limbs, for lexing integer literals */
int limb[4];
void lmuladd(int m, int d) {
  int i = 0; int x;
  while (i < 4) { x = limb[i] * m + d; limb[i] = x & 65535; d = x >> 16; i++; }
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
      limb[0] = 0; limb[1] = 0; limb[2] = 0; limb[3] = 0;
      if (c == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i = i + 2;
        while (hexval(src[i]) >= 0) { lmuladd(16, hexval(src[i])); i++; }
      } else {
        while (isdig(src[i])) { lmuladd(10, src[i] - '0'); i++; }
      }
      if (src[i] == '.' || src[i] == 'e' || src[i] == 'E')
        die("floating-point literals are not supported (use __f64_from_bits)");
      tk[ntok] = T_NUM;
      tv[ntok] = limb[0] | limb[1] << 16; th[ntok] = limb[2] | limb[3] << 16;
      v = 0; k = 0;   /* suffixes: v unsigned, k long */
      while (src[i] == 'u' || src[i] == 'U' || src[i] == 'l' || src[i] == 'L') {
        if (src[i] == 'u' || src[i] == 'U') v = 1; else k = 1;
        i++;
      }
      if (th[ntok] || (tv[ntok] < 0 && !v)) k = 1;
      tl[ntok] = k ? (v ? TY_ULONG : TY_LONG) : (v ? TY_UINT : TY_INT);
    } else if (isid(c)) {
      k = i;
      while (isid(src[i])) i++;
      tk[ntok] = keyword(k, i - k); tv[ntok] = k; tl[ntok] = i - k;
    } else if (c == '\'') {
      c = src[i + 1];
      i = i + 2;
      if (c == '\\') { c = escape(src[i]); i++; }
      i++;
      tk[ntok] = T_NUM; tv[ntok] = c; th[ntok] = 0; tl[ntok] = TY_INT;
    } else if (c == '"') {
      i++;
      tk[ntok] = T_STR; tv[ntok] = DATA_BASE + datalen;
      while (src[i] != '"') {
        if (!src[i]) die("unterminated string");
        c = src[i++];
        if (c == '\\' && src[i] == 'x') {
          c = hexval(src[i + 1]) * 16 + hexval(src[i + 2]);
          i = i + 3;
        } else if (c == '\\') c = escape(src[i++]);
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
int to_out;          /* emit to out (1) or ldecl (2) instead of code */
char ldecl[2 * MAXLOC + 16];  /* a function's local declarations */
int ldlen;

void emit(int b) {
  if (to_out == 1) out[olen++] = b;
  else if (to_out == 2) ldecl[ldlen++] = b;
  else { if (clen >= MAXCODE) die("too much code"); code[clen++] = b; }
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
/* i64.const hi:lo */
void lconst(int lo, int hi) {
  int b;
  emit(0x42);
  while (1) {
    b = lo & 127;
    lo = ((lo >> 7) & 33554431) | (hi << 25);
    hi = hi >> 7;
    if ((hi == 0 && lo == 0 && !(b & 64)) || (hi == -1 && lo == -1 && (b & 64))) { emit(b); return; }
    emit(b | 128);
  }
}
/* f64.const with the bits hi:lo */
void dconst(int lo, int hi) {
  emit(0x44);
  emit(lo); emit(lo >> 8); emit(lo >> 16); emit(lo >> 24);
  emit(hi); emit(hi >> 8); emit(hi >> 16); emit(hi >> 24);
}

/* relocations: patched once function indices and memory layout are known */
enum { R_FUNC = 1, R_BSS, R_HEAP, R_TYPE };
int rpos[MAXREL]; int rkind[MAXREL]; int rval[MAXREL];
int nrel;
void reloc(int kind, int v) {
  if (nrel >= MAXREL) die("too many relocations");
  rpos[nrel] = clen; rkind[nrel] = kind; rval[nrel] = v; nrel++;
  pad5(0);
}

/* insert ldecl[0..ldlen) into the code at pos, moving relocations after it */
void insertcode(int pos) {
  int i;
  if (clen + ldlen > MAXCODE) die("too much code");
  for (i = clen - 1; i >= pos; i--) code[i + ldlen] = code[i];
  for (i = 0; i < ldlen; i++) code[pos + i] = ldecl[i];
  clen = clen + ldlen;
  for (i = 0; i < nrel; i++) if (rpos[i] >= pos) rpos[i] = rpos[i] + ldlen;
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
int fpt[MAXFN * MAXP];   /* parameter types */
int fused[MAXFN];        /* called (an undefined function is imported only if so) */
int nfn; int ndef; int nimp;
int mainf = -1;
int usetable;        /* functions are used as values: emit a table */

int bsslen;

/* ---------------------------------------------------------------- types */

int isptr(int t) { return t >= PTR; }
int tsize(int t) {
  if (isptr(t)) return 4;
  if (t == TY_LONG || t == TY_ULONG || t == TY_DOUBLE) return 8;
  if (t == TY_INT || t == TY_UINT) return 4;
  return 1;
}
/* the wasm value kind: 0 i32, 1 i64, 2 f64 */
int vk(int t) {
  if (isptr(t)) return 0;
  if (t == TY_LONG || t == TY_ULONG) return 1;
  if (t == TY_DOUBLE) return 2;
  return 0;
}
int valtype(int t) { int k = vk(t); return k == 0 ? 0x7f : k == 1 ? 0x7e : 0x7c; }
int isuns(int t) { return isptr(t) || t == TY_UCHAR || t == TY_UINT || t == TY_ULONG; }
int promote(int t) { if (t == TY_CHAR || t == TY_UCHAR) return TY_INT; return t; }
/* the usual arithmetic conversions */
int arith(int a, int b) {
  if (isptr(a) || isptr(b)) return TY_UINT;
  if (a == TY_DOUBLE || b == TY_DOUBLE) return TY_DOUBLE;
  if (a == TY_ULONG || b == TY_ULONG) return TY_ULONG;
  if (a == TY_LONG || b == TY_LONG) return TY_LONG;
  if (a == TY_UINT || b == TY_UINT) return TY_UINT;
  return TY_INT;
}

int istype(int i) {
  int t = tk[i];
  return t == T_INT || t == T_CHAR || t == T_VOID || t == T_STATIC || t == T_CONST ||
         t == T_UNSIGNED || t == T_SIGNED || t == T_LONG || t == T_DOUBLE;
}
int basetype() {
  int uns = 0; int lng = 0; int t = -1;
  while (1) {
    if (accept(T_STATIC) || accept(T_CONST) || accept(T_SIGNED)) continue;
    if (accept(T_UNSIGNED)) { uns = 1; continue; }
    if (accept(T_LONG)) { lng = 1; continue; }
    if (accept(T_INT)) { if (t < 0) t = TY_INT; continue; }
    if (accept(T_CHAR)) { t = TY_CHAR; continue; }
    if (accept(T_VOID)) { t = TY_VOID; continue; }
    if (accept(T_DOUBLE)) { t = TY_DOUBLE; continue; }
    break;
  }
  if (lng && t != TY_DOUBLE) t = TY_LONG;
  if (t < 0) { if (!uns) die("expected a type"); t = TY_INT; }
  if (uns) {
    if (t == TY_CHAR) t = TY_UCHAR;
    else if (t == TY_INT) t = TY_UINT;
    else if (t == TY_LONG) t = TY_ULONG;
  }
  return t;
}
int stars(int t) { while (accept('*')) t = t + PTR; return t; }

/* ---------------------------------------------------------- expressions */

/* After parsing an expression, lv says where its value is:
 * 0: on the wasm stack; 1: in wasm local lvi; 2: in memory, address on
 * the stack.  rv() turns the latter two into the first. */
int lv; int lvi;
int tmpa;            /* scratch locals: an address, */
int tmpk;            /* and values of each kind: tmpk + 0, 1, 2 */
int nloc;
int ltype[MAXLOC];   /* the types of this function's locals */
int curret;          /* this function's return type */
int callop;          /* where the last call's opcode is, and where it ends: */
int callend;         /* return f(x) becomes a tail call (return_call) */

void load(int t) {
  if (t == TY_CHAR) emit3(0x2c, 0, 0);
  else if (t == TY_UCHAR) emit3(0x2d, 0, 0);
  else if (vk(t) == 1) emit3(0x29, 3, 0);
  else if (vk(t) == 2) emit3(0x2b, 3, 0);
  else emit3(0x28, 2, 0);
}
void store(int t) {
  if (t == TY_CHAR || t == TY_UCHAR) emit3(0x3a, 0, 0);
  else if (vk(t) == 1) emit3(0x37, 3, 0);
  else if (vk(t) == 2) emit3(0x39, 3, 0);
  else emit3(0x36, 2, 0);
}
void rv(int t) {
  if (lv == 1) { emit(0x20); uleb(lvi); }
  else if (lv == 2) load(t);
  lv = 0;
}
void lget(int i) { emit(0x20); uleb(i); }
void lset(int i) { emit(0x21); uleb(i); }
void ltee(int i) { emit(0x22); uleb(i); }
int newlocal(int t) {
  if (nloc >= MAXLOC) die("too many locals");
  ltype[nloc] = t;
  return nloc++;
}

/* convert the value on the stack from type f to type t */
void conv(int f, int t) {
  int a = vk(f); int b = vk(t);
  if (a == 0 && b == 1) emit(isuns(f) ? 0xad : 0xac);
  else if (a == 0 && b == 2) emit(isuns(f) ? 0xb8 : 0xb7);
  else if (a == 1 && b == 0) emit(0xa7);
  else if (a == 1 && b == 2) emit(isuns(f) ? 0xba : 0xb9);
  else if (a == 2 && b == 0) emit(isuns(t) ? 0xab : 0xaa);
  else if (a == 2 && b == 1) emit(isuns(t) ? 0xb1 : 0xb0);
  if (t == TY_CHAR && f != TY_CHAR) { iconst(24); emit(0x74); iconst(24); emit(0x75); }
  if (t == TY_UCHAR && f != TY_UCHAR) { iconst(255); emit(0x71); }
}
/* left (type t) and right (type t2) are on the stack: convert both to ct */
void conv2(int t, int t2, int ct) {
  conv(t2, ct);
  if (vk(t) != vk(ct)) {
    lset(tmpk + vk(ct));
    conv(t, ct);
    lget(tmpk + vk(ct));
  }
}
/* turn a value into an i32 truth value */
void tobool(int t) {
  if (vk(t) == 1) { lconst(0, 0); emit(0x52); }
  else if (vk(t) == 2) { dconst(0, 0); emit(0x62); }
}
void zero(int t) {
  if (vk(t) == 1) lconst(0, 0); else if (vk(t) == 2) dconst(0, 0); else iconst(0);
}
void scale(int t) { int s = tsize(t); if (s != 1) { iconst(s); emit(0x6c); } }

int opcode(int op, int t) {
  int k = vk(t); int u = isuns(t);
  if (k == 2) {
    if (op == '+' || op == T_ADDA) return 0xa0;
    if (op == '-' || op == T_SUBA) return 0xa1;
    if (op == '*' || op == T_MULA) return 0xa2;
    if (op == '/' || op == T_DIVA) return 0xa3;
    if (op == T_EQ) return 0x61;
    if (op == T_NE) return 0x62;
    if (op == '<') return 0x63;
    if (op == '>') return 0x64;
    if (op == T_LE) return 0x65;
    if (op == T_GE) return 0x66;
    die("invalid operands to a binary operator");
  }
  if (op == '+' || op == T_ADDA) return k ? 0x7c : 0x6a;
  if (op == '-' || op == T_SUBA) return k ? 0x7d : 0x6b;
  if (op == '*' || op == T_MULA) return k ? 0x7e : 0x6c;
  if (op == '/' || op == T_DIVA) return k ? (u ? 0x80 : 0x7f) : (u ? 0x6e : 0x6d);
  if (op == '%' || op == T_MODA) return k ? (u ? 0x82 : 0x81) : (u ? 0x70 : 0x6f);
  if (op == '&' || op == T_ANDA) return k ? 0x83 : 0x71;
  if (op == '|' || op == T_ORA) return k ? 0x84 : 0x72;
  if (op == '^' || op == T_XORA) return k ? 0x85 : 0x73;
  if (op == T_SHL || op == T_SHLA) return k ? 0x86 : 0x74;
  if (op == T_SHR || op == T_SHRA) return k ? (u ? 0x88 : 0x87) : (u ? 0x76 : 0x75);
  if (op == T_EQ) return k ? 0x51 : 0x46;
  if (op == T_NE) return k ? 0x52 : 0x47;
  if (op == '<') return k ? (u ? 0x54 : 0x53) : (u ? 0x49 : 0x48);
  if (op == '>') return k ? (u ? 0x56 : 0x55) : (u ? 0x4b : 0x4a);
  if (op == T_LE) return k ? (u ? 0x58 : 0x57) : (u ? 0x4d : 0x4c);
  return k ? (u ? 0x5a : 0x59) : (u ? 0x4f : 0x4e);
}
int isshift(int op) { return op == T_SHL || op == T_SHR || op == T_SHLA || op == T_SHRA; }
int iscmp(int op) { return op == T_EQ || op == T_NE || op == '<' || op == '>' || op == T_LE || op == T_GE; }

/* apply arithmetic operator op to left (type t) and right (t2) on the
 * stack; returns the type of the result */
int binop(int op, int t, int t2) {
  int ct;
  if ((op == '+' || op == '-' || op == T_ADDA || op == T_SUBA) && isptr(t)) {
    if (isptr(t2)) {   /* pointer difference */
      emit(0x6b);
      if (tsize(t - PTR) != 1) { iconst(tsize(t - PTR)); emit(0x6d); }
      return TY_INT;
    }
    conv(t2, TY_INT);
    scale(t - PTR);
    emit(op == '+' || op == T_ADDA ? 0x6a : 0x6b);
    return t;
  }
  if (isshift(op)) { ct = promote(t); conv(t2, ct); emit(opcode(op, ct)); return ct; }
  ct = arith(promote(t), promote(t2));
  conv2(t, t2, ct);
  emit(opcode(op, ct));
  if (iscmp(op)) return TY_INT;
  return ct;
}

int expr();
int assign();
int cexpr();
int unary();

/* the value of symbol s (sets lv) */
int var(int s) {
  lv = 0;
  if (skind[s] == S_CONST) { iconst(sval[s]); return TY_INT; }
  if (skind[s] == S_LOCAL) { lv = 1; lvi = sval[s]; return stype[s]; }
  if (skind[s] == S_FUNC) {   /* a function's value is its table index */
    emit(0x41); reloc(R_FUNC, sval[s]);
    fused[sval[s]] = 1;
    usetable = 1;
    return TY_INT;
  }
  emit(0x41);
  if (sbss[s]) reloc(R_BSS, sval[s]); else sleb(sval[s]);
  if (sarr[s]) return stype[s] + PTR;
  lv = 2;
  return stype[s];
}

int call(int p, int n) {
  int s; int f; int np = 0; int t;
  if (nameis(p, n, "__builtin_trap")) { expect('('); expect(')'); emit(0); return TY_VOID; }
  if (nameis(p, n, "__memory_size")) { expect('('); expect(')'); emit2(0x3f, 0); return TY_INT; }
  if (nameis(p, n, "__memory_grow")) {
    expect('('); t = assign(); rv(t); conv(t, TY_INT); expect(')'); emit2(0x40, 0); return TY_INT;
  }
  if (nameis(p, n, "__heap_base")) { expect('('); expect(')'); emit(0x41); reloc(R_HEAP, 0); return TY_CHAR + PTR; }
  t = -1;
  if (nameis(p, n, "__f64_from_bits")) { t = TY_LONG; f = 0xbf; s = TY_DOUBLE; }
  if (nameis(p, n, "__f64_bits")) { t = TY_DOUBLE; f = 0xbd; s = TY_LONG; }
  if (nameis(p, n, "__builtin_sqrt")) { t = TY_DOUBLE; f = 0x9f; s = TY_DOUBLE; }
  if (nameis(p, n, "__builtin_floor")) { t = TY_DOUBLE; f = 0x9c; s = TY_DOUBLE; }
  if (nameis(p, n, "__builtin_ceil")) { t = TY_DOUBLE; f = 0x9b; s = TY_DOUBLE; }
  if (nameis(p, n, "__builtin_trunc")) { t = TY_DOUBLE; f = 0x9d; s = TY_DOUBLE; }
  if (nameis(p, n, "__builtin_fabs")) { t = TY_DOUBLE; f = 0x99; s = TY_DOUBLE; }
  if (nameis(p, n, "__builtin_nearbyint")) { t = TY_DOUBLE; f = 0x9e; s = TY_DOUBLE; }
  if (nameis(p, n, "__builtin_clz")) { t = TY_UINT; f = 0x67; s = TY_INT; }
  if (nameis(p, n, "__builtin_ctz")) { t = TY_UINT; f = 0x68; s = TY_INT; }
  if (nameis(p, n, "__builtin_popcount")) { t = TY_UINT; f = 0x69; s = TY_INT; }
  if (nameis(p, n, "__builtin_clzll")) { t = TY_ULONG; f = 0x79; s = TY_LONG; }
  if (nameis(p, n, "__builtin_ctzll")) { t = TY_ULONG; f = 0x7a; s = TY_LONG; }
  if (nameis(p, n, "__builtin_popcountll")) { t = TY_ULONG; f = 0x7b; s = TY_LONG; }
  if (t >= 0) {
    expect('('); np = assign(); rv(np); conv(np, t); expect(')');
    emit(f);
    return s;
  }
  /* two double arguments: wasm's f64.min, max, copysign */
  if (nameis(p, n, "__builtin_fmin")) f = 0xa4;
  if (nameis(p, n, "__builtin_fmax")) f = 0xa5;
  if (nameis(p, n, "__builtin_copysign")) f = 0xa6;
  if (nameis(p, n, "__builtin_fmin") || nameis(p, n, "__builtin_fmax") || nameis(p, n, "__builtin_copysign")) {
    expect('('); t = assign(); rv(t); conv(t, TY_DOUBLE); expect(',');
    t = assign(); rv(t); conv(t, TY_DOUBLE); expect(')');
    emit(f);
    return TY_DOUBLE;
  }
  s = lookup(p, n);
  if (s >= 0 && (skind[s] == S_LOCAL || skind[s] == S_GLOBAL)) {
    /* call through a function pointer held in an int: call_indirect,
       with every parameter and the result an int */
    expect('(');
    if (!accept(')')) {
      do { t = assign(); rv(t); conv(t, TY_INT); np++; } while (accept(','));
      expect(')');
    }
    rv(var(s));
    callop = clen;
    emit(0x11); reloc(R_TYPE, np); emit(0);
    callend = clen;
    usetable = 1;
    lv = 0;
    return TY_INT;
  }
  if (s < 0 || skind[s] != S_FUNC) diename("call of undeclared function", p, n);
  f = sval[s];
  expect('(');
  if (!accept(')')) {
    do {
      t = assign(); rv(t);
      if (np < fnp[f] && np < MAXP) conv(t, fpt[f * MAXP + np]);
      np++;
    } while (accept(','));
    expect(')');
  }
  if (np != fnp[f]) die("wrong number of arguments");
  callop = clen;
  emit(0x10); reloc(R_FUNC, f);
  callend = clen;
  fused[f] = 1;
  lv = 0;
  return fret[f];
}

int primary() {
  int t = tk[tp]; int s; int p; int n;
  lv = 0;
  if (t == T_NUM) {
    tp++;
    t = tl[tp - 1];
    if (vk(t) == 1) lconst(tv[tp - 1], th[tp - 1]); else iconst(tv[tp - 1]);
    return t;
  }
  if (t == T_STR) { tp++; iconst(tv[tp - 1]); return TY_CHAR + PTR; }
  if (t == '(') { tp++; t = expr(); expect(')'); return t; }
  if (t != T_ID) die("expected an expression");
  p = tv[tp]; n = tl[tp]; tp++;
  if (tk[tp] == '(') return call(p, n);
  s = lookup(p, n);
  if (s < 0) diename("undeclared identifier", p, n);
  return var(s);
}

/* ++x, --x, x++, x-- */
void addone(int t, int d) {
  if (isptr(t)) { iconst(d * tsize(t - PTR)); emit(0x6a); }
  else if (vk(t) == 1) { lconst(d, d < 0 ? -1 : 0); emit(0x7c); }
  else if (vk(t) == 2) { dconst(0, d < 0 ? -1074790400 : 1072693248); emit(0xa0); }
  else { iconst(d); emit(0x6a); conv(TY_INT, t); }
}
int incdec(int t, int d, int post) {
  int tv_ = tmpk + vk(t);
  if (lv == 1) {
    if (post) { lget(lvi); lget(lvi); addone(t, d); lset(lvi); }
    else { lget(lvi); addone(t, d); ltee(lvi); }
  } else if (lv == 2) {
    ltee(tmpa); lget(tmpa); load(t);
    if (post) { ltee(tv_); addone(t, d); store(t); lget(tv_); }
    else { addone(t, d); ltee(tv_); store(t); lget(tv_); }
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
      conv(t2, TY_INT);
      scale(t - PTR);
      emit(0x6a);
      expect(']');
      t = t - PTR; lv = 2;
    } else if (accept(T_INC)) t = incdec(t, 1, 1);
    else if (accept(T_DEC)) t = incdec(t, -1, 1);
    else return t;
  }
}

int unary() {
  int t = tk[tp]; int tc;
  if (accept('-')) {
    if (tk[tp] == T_NUM && tl[tp] == TY_INT) { tp++; iconst(-tv[tp - 1]); lv = 0; return TY_INT; }
    t = unary(); rv(t); t = promote(t);
    if (vk(t) == 2) emit(0x9a);   /* f64.neg */
    else if (vk(t) == 1) { lconst(-1, -1); emit(0x7e); }
    else { iconst(-1); emit(0x6c); }
    return t;
  }
  if (accept('+')) { t = unary(); rv(t); return promote(t); }
  if (accept('!')) {
    t = unary(); rv(t);
    if (vk(t) == 1) emit(0x50);
    else if (vk(t) == 2) { dconst(0, 0); emit(0x61); }
    else emit(0x45);
    return TY_INT;
  }
  if (accept('~')) {
    t = unary(); rv(t); t = promote(t);
    if (vk(t) == 1) { lconst(-1, -1); emit(0x85); } else { iconst(-1); emit(0x73); }
    return t;
  }
  if (accept('*')) {
    t = unary(); rv(t);
    if (!isptr(t)) die("dereference of a non-pointer");
    lv = 2;
    return t - PTR;
  }
  if (accept('&')) {
    t = unary();
    if (lv != 2) die("cannot take this address");
    lv = 0;
    return t + PTR;
  }
  if (accept(T_INC)) { t = unary(); return incdec(t, 1, 0); }
  if (accept(T_DEC)) { t = unary(); return incdec(t, -1, 0); }
  if (accept(T_SIZEOF)) {
    expect('('); t = stars(basetype()); expect(')');
    iconst(tsize(t)); lv = 0;
    return TY_INT;
  }
  if (t == '(' && istype(tp + 1)) {   /* a cast */
    tp++; tc = stars(basetype()); expect(')');
    t = unary(); rv(t);
    if (tc == TY_VOID) { if (t != TY_VOID) emit(0x1a); return TY_VOID; }
    conv(t, tc);
    return tc;
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

int binary(int minp) {
  int t = unary(); int op; int p; int t2;
  while (1) {
    op = tk[tp]; p = prec(op);
    if (p == 0 || p < minp) return t;
    tp++;
    rv(t);
    if (op == T_AND) {
      tobool(t);
      emit2(0x04, 0x7f); t2 = binary(p + 1); rv(t2); tobool(t2); iconst(0); emit(0x47);
      emit(0x05); iconst(0); emit(0x0b);
      t = TY_INT;
    } else if (op == T_OR) {
      tobool(t);
      emit2(0x04, 0x7f); iconst(1);
      emit(0x05); t2 = binary(p + 1); rv(t2); tobool(t2); iconst(0); emit(0x47); emit(0x0b);
      t = TY_INT;
    } else {
      t2 = binary(p + 1); rv(t2);
      t = binop(op, t, t2);
    }
  }
}

int cond() {
  int t = binary(1); int t2; int bt; int mid; int ct;
  if (!accept('?')) return t;
  rv(t); tobool(t);
  emit(0x04); bt = clen; emit(0x7f);
  t = expr(); rv(t);
  mid = clen;
  expect(':');
  emit(0x05);
  t2 = cond(); rv(t2);
  ct = t;   /* the common type of the two branches */
  if (t != TY_VOID && t2 != TY_VOID && !isptr(t) && !isptr(t2)) ct = arith(promote(t), promote(t2));
  if (t != TY_VOID) conv(t2, ct);
  to_out = 2; ldlen = 0; conv(t, ct); to_out = 0;
  insertcode(mid);
  code[bt] = ct == TY_VOID ? 0x40 : valtype(ct);
  emit(0x0b);
  return ct;
}

int assign() {
  int t = cond(); int op = tk[tp]; int t2; int i; int k;
  if (op != '=' && (op < T_ADDA || op > T_SHRA)) return t;
  tp++;
  k = tmpk + vk(t);
  if (lv == 1) {
    i = lvi;
    if (op == '=') { t2 = assign(); rv(t2); conv(t2, t); }
    else { lget(i); t2 = assign(); rv(t2); conv(binop(op, t, t2), t); }
    ltee(i);
  } else if (lv == 2) {
    if (op == '=') { t2 = assign(); rv(t2); conv(t2, t); }
    else { ltee(tmpa); lget(tmpa); load(t); t2 = assign(); rv(t2); conv(binop(op, t, t2), t); }
    ltee(k); store(t); lget(k);
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
void condition() { int t = expr(); rv(t); tobool(t); }

void decl() {
  int bt = basetype(); int t; int i; int t2;
  do {
    t = stars(bt);
    if (tk[tp] != T_ID) die("expected a name");
    if (tk[tp + 1] == '[') die("local arrays are not supported");
    i = newlocal(t);
    addsym(tv[tp], tl[tp], S_LOCAL, t, i);
    tp++;
    if (accept('=')) { t2 = assign(); rv(t2); conv(t2, t); lset(i); }
    else { zero(t); lset(i); }
  } while (accept(','));
  expect(';');
}

void stmt();

/* skip from an opening token to just past its matching closing one */
void skipbal(int open, int close) {
  int d = 0;
  do {
    if (tk[tp] == open) d++;
    if (tk[tp] == close) d--;
    if (tk[tp] == T_EOF) die("unbalanced brackets");
    tp++;
  } while (d);
}

/* switch: the case labels are found first by reading ahead, then
 * dispatched with br_table (or a chain of br_ifs if they are sparse) to
 * nested blocks, one per label, in which the code falls through */
enum { MAXCASE = 4096 };
int casev[MAXCASE]; int casedef[MAXCASE]; int ncase;
void switchstmt() {
  int first = ncase; int n; int i; int j; int d = 0; int save; int min = 0; int max = 0;
  int miss; int ncv = 0; int t; int ob = brk; int v;
  expect('('); t = expr(); rv(t); conv(t, TY_INT); expect(')');
  lset(tmpk);
  if (tk[tp] != '{') die("expected a block after switch");
  save = tp;
  while (1) {
    if (tk[tp] == T_EOF) die("unterminated switch");
    if (tk[tp] == T_SWITCH) { tp++; skipbal('(', ')'); skipbal('{', '}'); continue; }
    if (tk[tp] == '{') d++;
    if (tk[tp] == '}') { d--; if (!d) break; }
    if (d == 1 && (tk[tp] == T_CASE || tk[tp] == T_DEFAULT)) {
      if (ncase >= MAXCASE) die("too many cases");
      casedef[ncase] = tk[tp] == T_DEFAULT;
      tp++;
      casev[ncase] = casedef[ncase] ? 0 : cexpr();
      if (!casedef[ncase]) {
        if (!ncv || casev[ncase] < min) min = casev[ncase];
        if (!ncv || casev[ncase] > max) max = casev[ncase];
        ncv++;
      }
      ncase++;
      continue;
    }
    tp++;
  }
  tp = save;
  n = ncase - first;
  miss = n;   /* where an unmatched value goes: the default, or past the end */
  for (i = 0; i < n; i++) if (casedef[first + i]) miss = i;
  emit2(0x02, 0x40); depth++; brk = depth;
  for (i = 0; i < n; i++) { emit2(0x02, 0x40); depth++; }
  if (ncv && max - min >= 0 && max - min < 4 * ncv + 64) {
    lget(tmpk); iconst(min); emit(0x6b);
    emit(0x0e); uleb(max - min + 1);
    for (v = min; v <= max; v++) {
      j = miss;
      for (i = n - 1; i >= 0; i--) if (!casedef[first + i] && casev[first + i] == v) j = i;
      uleb(j);
    }
    uleb(miss);
  } else {
    for (i = 0; i < n; i++) {
      if (casedef[first + i]) continue;
      lget(tmpk); iconst(casev[first + i]); emit(0x46); emit(0x0d); uleb(i);
    }
    emit(0x0c); uleb(miss);
  }
  expect('{');
  save = nsym;
  while (!accept('}')) {
    if (accept(T_CASE)) { cexpr(); expect(':'); emit(0x0b); depth--; continue; }
    if (accept(T_DEFAULT)) { expect(':'); emit(0x0b); depth--; continue; }
    if (istype(tp)) decl(); else stmt();
  }
  nsym = save;
  emit(0x0b); depth--;
  brk = ob;
  ncase = first;
}

void stmt() {
  int t; int save; int ob = brk; int oc = cont; int top; int step; int n;
  if (accept('{')) {
    save = nsym;
    while (!accept('}')) { if (istype(tp)) decl(); else stmt(); }
    nsym = save;
  } else if (accept(T_IF)) {
    expect('('); condition(); expect(')');
    emit2(0x04, 0x40); depth++;
    stmt();
    if (accept(T_ELSE)) { emit(0x05); stmt(); }
    emit(0x0b); depth--;
  } else if (accept(T_WHILE)) {
    emit2(0x02, 0x40); emit2(0x03, 0x40); depth = depth + 2;
    brk = depth - 1; cont = depth;
    expect('('); condition(); expect(')');
    emit2(0x45, 0x0d); uleb(depth - brk);
    stmt();
    emit(0x0c); uleb(depth - cont);
    emit2(0x0b, 0x0b); depth = depth - 2;
  } else if (accept(T_DO)) {
    emit2(0x02, 0x40); emit2(0x03, 0x40); emit2(0x02, 0x40); depth = depth + 3;
    brk = depth - 2; top = depth - 1; cont = depth;
    stmt();
    emit(0x0b); depth--;
    expect(T_WHILE); expect('('); condition(); expect(')'); expect(';');
    emit(0x0d); uleb(depth - top);
    emit2(0x0b, 0x0b); depth = depth - 2;
  } else if (accept(T_FOR)) {
    expect('(');
    if (!accept(';')) { discard(expr()); expect(';'); }
    emit2(0x02, 0x40); emit2(0x03, 0x40); depth = depth + 2;
    brk = depth - 1; top = depth;
    if (!accept(';')) {
      condition(); expect(';');
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
    if (!accept(';')) {
      callend = -1;
      t = expr(); rv(t); conv(t, curret); expect(';');
      /* a call whose result is returned as is: a tail call (return_call) */
      if (to_out == 0 && clen == callend && t != TY_VOID) code[callop] = code[callop] + 2;
      else emit(0x0f);
    } else emit(0x0f);
  } else if (accept(T_SWITCH)) {
    switchstmt();
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
  int s = lookup(p, n); int f; int np = 0; int save; int t; int body; int start; int r0; int i; int len;
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
  nloc = 0;
  if (tk[tp] == T_VOID && tk[tp + 1] == ')') tp++;
  if (!accept(')')) {
    do {
      t = stars(basetype());
      if (np < MAXP) fpt[f * MAXP + np] = t;
      if (tk[tp] == T_ID) { addsym(tv[tp], tl[tp], S_LOCAL, t, np); tp++; }
      newlocal(t);
      np++;
    } while (accept(','));
    expect(')');
  }
  if (np > MAXP) die("too many parameters");
  fnp[f] = np;
  if (tk[tp] != '{') { nsym = save; return 0; }
  if (fdef[f]) die("function redefined");
  fdef[f] = 1; ford[f] = ndef; byord[ndef] = f; ndef++;
  tmpa = newlocal(TY_INT);
  tmpk = newlocal(TY_INT); newlocal(TY_LONG); newlocal(TY_DOUBLE);
  curret = ret; depth = 0;
  body = clen; pad5(0);
  start = clen; r0 = nrel;
  stmt();
  if (ret != TY_VOID) zero(ret);
  emit(0x0b);
  /* now that the locals are known, insert their declarations */
  to_out = 2; ldlen = 0;
  uleb(nloc - np);
  for (i = np; i < nloc; i++) emit2(1, valtype(ltype[i]));
  to_out = 0;
  insertcode(start);
  patch5(code, body, clen - body - 5);
  nsym = save;
  return 1;
}

void global(int t, int p, int n) {
  int len = 0; int s; int lo; int hi; int a; int i;
  if (accept('[')) { len = cexpr(); expect(']'); }
  if (accept('=')) {
    if (len) die("array initializers are not supported");
    hi = 0;
    a = tk[tp] == '-' && tk[tp + 1] == T_NUM;   /* a literal, maybe negated, of any size */
    if (tk[tp] == T_STR) { lo = tv[tp]; tp++; }
    else if (tk[tp + a] == T_NUM && prec(tk[tp + a + 1]) == 0) {
      lo = tv[tp + a]; hi = th[tp + a]; tp = tp + a + 1;
      if (a) { lo = -lo; hi = ~hi + (lo == 0); }
    } else { lo = cexpr(); hi = lo < 0 ? -1 : 0; }
    if (t == TY_DOUBLE && (lo || hi)) die("double initializers must be 0");
    a = tsize(t);
    while (datalen % a) datalen++;
    s = addsym(p, n, S_GLOBAL, t, DATA_BASE + datalen);
    for (i = 0; i < a; i++) {
      data[datalen++] = i < 4 ? lo >> (8 * i) : hi >> (8 * (i - 4));
    }
  } else {
    while (bsslen % 8) bsslen++;
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
void functype(int f) {
  int v;
  emit(0x60); uleb(fnp[f]);
  for (v = 0; v < fnp[f]; v++) emit(valtype(fpt[f * MAXP + v]));
  if (fret[f] == TY_VOID) emit(0); else emit2(1, valtype(fret[f]));
}

void finish() {
  int f; int i; int s; int v; int bss; int heap;
  char *wasi = "wasi_snapshot_preview1";
  if (mainf < 0 || !fdef[mainf]) die("no main function");
  for (f = 0; f < nfn; f++) if (!fdef[f] && fused[f]) fimp[f] = nimp++;
  bss = DATA_BASE + datalen;
  while (bss % 16) bss++;
  heap = bss + bsslen;
  while (heap % 16) heap++;
  for (i = 0; i < nrel; i++) {
    v = rval[i];
    if (rkind[i] == R_FUNC) v = fdef[v] ? nimp + ford[v] : fimp[v];
    else if (rkind[i] == R_BSS) v = bss + v;
    else if (rkind[i] == R_TYPE) v = nimp + ndef + 1 + v;
    else v = heap;
    patch5(code, rpos[i], v);
  }

  to_out = 1;
  emit(0); emit(0x61); emit(0x73); emit(0x6d); emit(1); emit(0); emit(0); emit(0);
  /* types: one per function, in function index order, then _start */
  s = section(1);
  uleb(nimp + ndef + 10);
  for (f = 0; f < nfn; f++) if (!fdef[f] && fused[f]) functype(f);
  for (i = 0; i < ndef; i++) functype(byord[i]);
  emit3(0x60, 0, 0);
  for (i = 0; i <= 8; i++) {   /* for call_indirect: (int * i) -> int */
    emit(0x60); uleb(i);
    for (v = 0; v < i; v++) emit(0x7f);
    emit2(1, 0x7f);
  }
  endsection(s);
  s = section(2);
  uleb(nimp);
  for (f = 0; f < nfn; f++) {
    if (fdef[f] || !fused[f]) continue;
    name(wasi, cstrlen(wasi));
    name(src + fname[f], fnlen[f]);
    emit(0); uleb(fimp[f]);
  }
  endsection(s);
  s = section(3);
  uleb(ndef + 1);
  for (i = 0; i <= ndef; i++) uleb(nimp + i);
  endsection(s);
  if (usetable) {    /* table slot i holds function i */
    s = section(4);
    emit3(1, 0x70, 0); uleb(nimp + ndef);
    endsection(s);
  }
  s = section(5);
  emit2(1, 0); uleb(heap / 65536 + 1);
  endsection(s);
  s = section(7);
  uleb(2);
  name("memory", 6); emit2(2, 0);
  name("_start", 6); emit(0); uleb(nimp + ndef);
  endsection(s);
  if (usetable) {
    s = section(9);
    emit2(1, 0); iconst(0); emit(0x0b);
    uleb(nimp + ndef);
    for (i = 0; i < nimp + ndef; i++) uleb(i);
    endsection(s);
  }
  s = section(10);
  uleb(ndef + 1);
  for (i = 0; i < clen; i++) emit(code[i]);
  v = olen; pad5(0);
  emit(0); emit(0x10); uleb(nimp + ford[mainf]);
  if (fret[mainf] != TY_VOID) { conv(fret[mainf], TY_INT); emit2(0x04, 0x40); emit2(0, 0x0b); }
  emit(0x0b);
  patch5(out, v, olen - v - 5);
  endsection(s);
  s = section(11);
  uleb(1);
  emit(0); iconst(DATA_BASE); emit(0x0b);
  uleb(datalen);
  for (i = 0; i < datalen; i++) emit(data[i]);
  endsection(s);
  /* function names, for backtraces */
  s = section(0);
  name("name", 4);
  emit(1); v = olen; pad5(0);
  uleb(nimp + ndef);
  for (f = 0; f < nfn; f++) if (!fdef[f] && fused[f]) { uleb(fimp[f]); name(src + fname[f], fnlen[f]); }
  for (i = 0; i < ndef; i++) { uleb(nimp + i); name(src + fname[byord[i]], fnlen[byord[i]]); }
  patch5(out, v, olen - v - 5);
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
