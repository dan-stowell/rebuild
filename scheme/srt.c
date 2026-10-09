/* srt.c -- the Scheme runtime library.  Needs libc.c, num.c, rt.c, read.c.
 *
 * Primitives come in two kinds.  p_name(a, b): fixed arity, returns the
 * value; the compiler calls these directly.  v_name(clo, n): the calling
 * convention of compiled procedures (arguments in stk, results in stk,
 * returns the count, negative while unwinding); for variadic primitives
 * and the ones that call procedures.
 */

int unwid;   /* while unwinding (a negative return): the continuation's id */

/* ---------------------------------------------------------------- errors */

void sbval(long v, int wr);
void err(char *msg, long irritant, int has) {
  flush();
  sbn = 0;
  sbcstr("Error: "); sbcstr(msg);
  if (has) { sbput(' '); sbval(irritant, 1); }
  sbput('\n');
  sys_write(2, sb, sbn);
  proc_exit(1);
}
void numerr(char *what, long v) {
  flush();
  sbn = 0;
  sbcstr("Error: "); sbcstr(what); sbcstr(": not a number: "); sbval(v, 1); sbput('\n');
  sys_write(2, sb, sbn);
  proc_exit(1);
}
void typeerr(char *fn, char *what, long v) {
  flush();
  sbn = 0;
  sbcstr("Error: "); sbcstr(fn); sbcstr(": not a "); sbcstr(what); sbcstr(": "); sbval(v, 1); sbput('\n');
  sys_write(2, sb, sbn);
  proc_exit(1);
}
int arityerr(int clo, int n) {
  flush();
  sbn = 0;
  sbcstr("Error: wrong number of arguments ("); sbint(n); sbcstr(") to "); sbval(clo, 1); sbput('\n');
  sys_write(2, sb, sbn);
  proc_exit(1);
  return -1;
}
void unbound(char *name) { err("unbound variable:", sym(name), 1); }

/* --------------------------------------------------------------- calling */

int call(long f, int n) {
  int fn;
  if (otype(f) != OFUN) err("not a procedure:", f, 1);
  fn = op(f)[1];
  return fn((int)f, n);
}
/* the arguments from stk[from] on, as a list */
long restlist(int from, int n) {
  long l = NULL; int i = n - 1;
  while (i >= from) { l = cons(stk[i], l); i--; }
  return l;
}

/* --------------------------------------------------------------- printing */

void sbdblscheme(double d) {
  int start = sbn; int i; int plain = 1;
  if (d != d) { sbcstr("+nan.0"); return; }
  if (d - d != d - d) { sbcstr(d > 0 ? "+inf.0" : "-inf.0"); return; }
  sbdblshort(d);
  for (i = start; i < sbn; i++) if (sb[i] == '.' || sb[i] == 'e') plain = 0;
  if (plain) sbcstr(".0");
}
void sbnum(long v) { if (isdbl(v)) sbdblscheme(dval(v)); else sbint(ival(v)); }

char *charname(int c) {
  if (c == ' ') return "space";
  if (c == '\n') return "newline";
  if (c == '\t') return "tab";
  if (c == '\r') return "return";
  if (c == 0) return "null";
  if (c == 127) return "delete";
  if (c == 27) return "escape";
  if (c == 7) return "alarm";
  if (c == 8) return "backspace";
  return (char *)0;
}

int symq(long v) { return otype(v) == OSYM; }
void sbval(long v, int wr) {
  int t; int i; int n; char *p; int c; long x;
  if (v == NIL) { return; }
  if (v == FALSE) { sbcstr("#f"); return; }
  if (v == TRUE) { sbcstr("#t"); return; }
  if (v == NULL) { sbcstr("()"); return; }
  if (v == EOFV) { sbcstr("#<eof>"); return; }
  if (isnum(v)) { sbnum(v); return; }
  if (ischar(v)) {
    c = charval(v);
    if (!wr) { sbput(c); return; }
    sbcstr("#\\");
    p = charname(c);
    if (p) sbcstr(p);
    else if (c < 32) { sbput('x'); sbintradix(c, 16); }
    else sbput(c);
    return;
  }
  t = otype(v);
  if (t == OSTR) {
    p = sptr(v); n = sl(v);
    if (!wr) { sbputs(p, n); return; }
    sbput('"');
    for (i = 0; i < n; i++) {
      c = p[i] & 255;
      if (c == '"' || c == '\\') { sbput('\\'); sbput(c); }
      else if (c == '\n') sbcstr("\\n");
      else if (c == '\t') sbcstr("\\t");
      else if (c == '\r') sbcstr("\\r");
      else sbput(c);
    }
    sbput('"');
    return;
  }
  if (t == OSYM) { sbputs(sptr(v), sl(v)); return; }
  if (t == OPAIR) {
    x = car_(v);
    if (wr >= 0 && symq(x) && ispair(cdr_(v)) && cdr_(cdr_(v)) == NULL) {
      p = (char *)0;
      if (x == S_quote) p = "'";
      if (x == S_quasiquote) p = "`";
      if (x == S_unquote) p = ",";
      if (x == S_unqspl) p = ",@";
      if (p) { sbcstr(p); sbval(car_(cdr_(v)), wr); return; }
    }
    sbput('(');
    while (1) {
      sbval(car_(v), wr);
      v = cdr_(v);
      if (!ispair(v)) break;
      sbput(' ');
    }
    if (v != NULL) { sbcstr(" . "); sbval(v, wr); }
    sbput(')');
    return;
  }
  if (t == OVEC) {
    sbcstr("#(");
    n = vlen(v);
    for (i = 0; i < n; i++) { if (i) sbput(' '); sbval(velts(v)[i], wr); }
    sbput(')');
    return;
  }
  if (t == OFUN) { sbcstr("#<procedure>"); return; }
  if (t == OREC) {
    sbcstr("#<"); sbval(velts(*(long *)(op(v) + 2))[0], 0); sbput('>');
    return;
  }
  if (t == OPROM) { sbcstr("#<promise>"); return; }
  sbcstr("#<object>");
}

/* --------------------------------------------------------------- numbers */

long B(int c) { return c ? TRUE : FALSE; }
long p_add(long a, long b) { return n_add(a, b); }
long p_sub(long a, long b) { return n_sub(a, b); }
long p_mul(long a, long b) { return n_mul(a, b); }
long p_neg(long a) { return n_neg(a); }
long p_div(long a, long b) {
  long x; long y;
  if (isint(a) && isint(b)) {
    x = ival(a); y = ival(b);
    if (y == 0) divzero();
    if (remi(x, y) != 0) err("rational numbers are not supported:", cons(a, cons(b, NULL)), 1);
    return mkint(quoi(x, y));
  }
  return mkdbl(numd(a, "/") / numd(b, "/"));
}
long p_numeq(long a, long b) { return B(n_eq(a, b)); }
long p_lt(long a, long b) { if (isfix(a) && isfix(b)) return B(a < b); return B(n_lt(a, b)); }
long p_gt(long a, long b) { if (isfix(a) && isfix(b)) return B(a > b); return B(n_lt(b, a)); }
long p_le(long a, long b) { if (isfix(a) && isfix(b)) return B(a <= b); return B(n_le(a, b)); }
long p_ge(long a, long b) { if (isfix(a) && isfix(b)) return B(a >= b); return B(n_le(b, a)); }
long p_zerop(long a) { if (isint(a)) return B(ival(a) == 0); return B(numd(a, "zero?") == 0); }
long p_positivep(long a) { if (isint(a)) return B(ival(a) > 0); return B(numd(a, "positive?") > 0); }
long p_negativep(long a) { if (isint(a)) return B(ival(a) < 0); return B(numd(a, "negative?") < 0); }
long intarg(long a, char *fn) {
  long i;
  if (isint(a)) return ival(a);
  if (isdbl(a)) { i = flt2int(dval(a)); if (fok) return i; }
  typeerr(fn, "integer", a);
  return 0;
}
/* the result of an integer operation: inexact if an argument was */
long intres(long r, long a, long b) { if (isdbl(a) || isdbl(b)) return mkdbl((double)r); return mkint(r); }
long p_oddp(long a) { return B(intarg(a, "odd?") & 1); }
long p_evenp(long a) { return B(!(intarg(a, "even?") & 1)); }
long p_quotient(long a, long b) { return intres(quoi(intarg(a, "quotient"), intarg(b, "quotient")), a, b); }
long p_remainder(long a, long b) { return intres(remi(intarg(a, "remainder"), intarg(b, "remainder")), a, b); }
long p_modulo(long a, long b) { return intres(modi(intarg(a, "modulo"), intarg(b, "modulo")), a, b); }
long p_abs(long a) {
  if (isint(a)) { if (ival(a) < 0) return n_neg(a); return a; }
  return mkdbl(__builtin_fabs(numd(a, "abs")));
}
long p_max(long a, long b) {
  long r = n_lt(a, b) ? b : a;
  if (isdbl(a) || isdbl(b)) return mkdbl(todbl(r));
  return r;
}
long p_min(long a, long b) {
  long r = n_lt(b, a) ? b : a;
  if (isdbl(a) || isdbl(b)) return mkdbl(todbl(r));
  return r;
}
long gcdi(long x, long y) {
  long t;
  if (x < 0) x = -x;
  if (y < 0) y = -y;
  while (y) { t = x % y; x = y; y = t; }
  return x;
}
long p_gcd(long a, long b) { return intres(gcdi(intarg(a, "gcd"), intarg(b, "gcd")), a, b); }
long p_lcm(long a, long b) {
  long x = intarg(a, "lcm"); long y = intarg(b, "lcm"); long g;
  if (x == 0 || y == 0) return intres(0, a, b);
  g = gcdi(x, y);
  x = muli(x / g, y);
  return intres(x < 0 ? -x : x, a, b);
}
long p_numberp(long a) { return B(isnum(a)); }
long p_integerp(long a) {
  if (isint(a)) return TRUE;
  if (isdbl(a)) { flt2int(dval(a)); return B(fok || (__builtin_floor(dval(a)) == dval(a) && dval(a) - dval(a) == 0)); }
  return FALSE;
}
long p_exactp(long a) { if (!isnum(a)) numerr("exact?", a); return B(isint(a)); }
long p_inexactp(long a) { if (!isnum(a)) numerr("inexact?", a); return B(isdbl(a)); }
long p_exactintegerp(long a) { return B(isint(a)); }
long p_nanp(long a) { double d = numd(a, "nan?"); return B(d != d); }
long p_exact(long a) {
  long i;
  if (isint(a)) return a;
  i = flt2int(numd(a, "exact"));
  if (!fok) err("exact: no exact representation (no rationals):", a, 1);
  return mkint(i);
}
long p_inexact(long a) { if (isdbl(a)) return a; return mkdbl(numd(a, "inexact")); }
long p_floor(long a) { if (isint(a)) return a; return mkdbl(__builtin_floor(numd(a, "floor"))); }
long p_ceiling(long a) { if (isint(a)) return a; return mkdbl(__builtin_ceil(numd(a, "ceiling"))); }
long p_truncate(long a) { if (isint(a)) return a; return mkdbl(__builtin_trunc(numd(a, "truncate"))); }
long p_round(long a) { if (isint(a)) return a; return mkdbl(__builtin_nearbyint(numd(a, "round"))); }
long p_square(long a) { return n_mul(a, a); }
long p_sqrt(long a) {
  long i; long r; double d;
  if (isint(a) && ival(a) >= 0) {
    i = ival(a);
    r = (long)__builtin_sqrt((double)i);
    while (r * r > i) r--;
    while ((r + 1) * (r + 1) <= i) r++;
    if (r * r == i) return mkint(r);
  }
  d = numd(a, "sqrt");
  return mkdbl(__builtin_sqrt(d));
}
long p_expt(long a, long b) {
  long x; long n; long r;
  if (isint(a) && isint(b) && ival(b) >= 0) {
    x = ival(a); n = ival(b); r = 1;
    while (n) { if (n & 1) r = muli(r, x); n = n >> 1; if (n) x = muli(x, x); }
    return mkint(r);
  }
  return mkdbl(pow_(numd(a, "expt"), numd(b, "expt")));
}
long p_exp(long a) { return mkdbl(lexp(numd(a, "exp"))); }
long p_log(long a) { return mkdbl(llog(numd(a, "log"))); }
long p_log2(long a, long b) { return mkdbl(llog(numd(a, "log")) / llog(numd(b, "log"))); }
long p_number2string(long a) {
  if (!isnum(a)) numerr("number->string", a);
  sbn = 0; sbnum(a); return sbstr();
}
long p_number2string2(long a, long r) {
  if (isint(a) && ival(r) != 10) { sbn = 0; sbintradix(ival(a), (int)ival(r)); return sbstr(); }
  return p_number2string(a);
}
long p_string2number2(long s, long r) {
  long x;
  if (otype(s) != OSTR) typeerr("string->number", "string", s);
  x = parsenum(sptr(s), sl(s), (int)ival(r));
  return x;
}
long p_string2number(long s) { return p_string2number2(s, mkint(10)); }

/* ------------------------------------------------------------- equality */

long p_not(long a) { return a == FALSE ? TRUE : FALSE; }
long p_booleanp(long a) { return B(a == TRUE || a == FALSE); }
long p_eqp(long a, long b) { return B(a == b); }
long p_eqvp(long a, long b) { return B(a == b); }
int equal(long a, long b) {
  int t; int i; int n;
  while (1) {
    if (a == b) return 1;
    t = otype(a);
    if (t != otype(b) || t == 0) return 0;
    if (t == OPAIR) {
      if (!equal(car_(a), car_(b))) return 0;
      a = cdr_(a); b = cdr_(b);
      continue;
    }
    if (t == OSTR) return sl(a) == sl(b) && memeq(sptr(a), sptr(b), sl(a));
    if (t == OVEC) {
      n = vlen(a);
      if (n != vlen(b)) return 0;
      for (i = 0; i < n; i++) if (!equal(velts(a)[i], velts(b)[i])) return 0;
      return 1;
    }
    return 0;
  }
  return 0;
}
long p_equalp(long a, long b) { return B(equal(a, b)); }

/* ----------------------------------------------------------------- pairs */

long p_cons(long a, long b) { return cons(a, b); }
long p_car(long p) { if (otype(p) != OPAIR) typeerr("car", "pair", p); return car_(p); }
long p_cdr(long p) { if (otype(p) != OPAIR) typeerr("cdr", "pair", p); return cdr_(p); }
long p_setcar(long p, long v) { if (otype(p) != OPAIR) typeerr("set-car!", "pair", p); *(long *)((int)p + 8) = v; return NIL; }
long p_setcdr(long p, long v) { if (otype(p) != OPAIR) typeerr("set-cdr!", "pair", p); *(long *)((int)p + 16) = v; return NIL; }
long p_caar(long p) { return p_car(p_car(p)); }
long p_cadr(long p) { return p_car(p_cdr(p)); }
long p_cdar(long p) { return p_cdr(p_car(p)); }
long p_cddr(long p) { return p_cdr(p_cdr(p)); }
long p_caddr(long p) { return p_car(p_cdr(p_cdr(p))); }
long p_cdddr(long p) { return p_cdr(p_cdr(p_cdr(p))); }
long p_cadddr(long p) { return p_car(p_cdr(p_cdr(p_cdr(p)))); }
long p_pairp(long a) { return B(otype(a) == OPAIR); }
long p_nullp(long a) { return B(a == NULL); }
long p_listp(long a) {
  long slow = a;
  while (1) {
    if (a == NULL) return TRUE;
    if (otype(a) != OPAIR) return FALSE;
    a = cdr_(a);
    if (a == NULL) return TRUE;
    if (otype(a) != OPAIR) return FALSE;
    a = cdr_(a);
    slow = cdr_(slow);
    if (a == slow) return FALSE;
  }
  return FALSE;
}
long p_length(long l) {
  long n = 0; long l0 = l;
  while (otype(l) == OPAIR) { n++; l = cdr_(l); }
  if (l != NULL) typeerr("length", "list", l0);
  return mkint(n);
}
long p_reverse(long l) {
  long r = NULL;
  while (otype(l) == OPAIR) { r = cons(car_(l), r); l = cdr_(l); }
  return r;
}
long p_append(long a, long b) {
  long head; long tail; long c;
  if (a == NULL) return b;
  head = tail = cons(p_car(a), NULL);
  a = cdr_(a);
  while (otype(a) == OPAIR) { c = cons(car_(a), NULL); *(long *)((int)tail + 16) = c; tail = c; a = cdr_(a); }
  *(long *)((int)tail + 16) = b;
  return head;
}
long p_listtail(long l, long k) {
  long n = ival(k);
  while (n > 0) { l = p_cdr(l); n--; }
  return l;
}
long p_listref(long l, long k) { return p_car(p_listtail(l, k)); }
long p_lastpair(long l) { while (otype(cdr_(l)) == OPAIR) l = cdr_(l); return l; }
long p_listcopy(long l) {
  if (otype(l) != OPAIR) return l;
  return p_append(l, NULL);
}
long p_makelist(long k, long fill) {
  long l = NULL; long n = ival(k);
  while (n > 0) { l = cons(fill, l); n--; }
  return l;
}
long p_makelist1(long k) { return p_makelist(k, NIL); }
long p_memq(long x, long l) {
  while (otype(l) == OPAIR) { if (car_(l) == x) return l; l = cdr_(l); }
  return FALSE;
}
long p_member(long x, long l) {
  while (otype(l) == OPAIR) { if (equal(car_(l), x)) return l; l = cdr_(l); }
  return FALSE;
}
long p_assq(long x, long l) {
  while (otype(l) == OPAIR) { if (p_car(car_(l)) == x) return car_(l); l = cdr_(l); }
  return FALSE;
}
long p_assoc(long x, long l) {
  while (otype(l) == OPAIR) { if (equal(p_car(car_(l)), x)) return car_(l); l = cdr_(l); }
  return FALSE;
}

/* --------------------------------------------------------------- symbols */

long p_symbolp(long a) { return B(otype(a) == OSYM); }
long p_symbol2string(long a) {
  if (otype(a) != OSYM) typeerr("symbol->string", "symbol", a);
  return mkstr(sptr(a), sl(a));
}
long p_string2symbol(long a) {
  if (otype(a) != OSTR) typeerr("string->symbol", "string", a);
  return intern(sptr(a), sl(a));
}
long p_symboleq(long a, long b) { return B(a == b); }

/* ------------------------------------------------------------ characters */

long p_charp(long a) { return B(ischar(a)); }
int chr(long a, char *fn) { if (!ischar(a)) typeerr(fn, "character", a); return charval(a); }
long p_char2int(long a) { return mkint(chr(a, "char->integer")); }
long p_int2char(long a) { return mkchar((int)intarg(a, "integer->char")); }
long p_chareq(long a, long b) { return B(chr(a, "char=?") == chr(b, "char=?")); }
long p_charlt(long a, long b) { return B(chr(a, "char<?") < chr(b, "char<?")); }
long p_chargt(long a, long b) { return B(chr(a, "char>?") > chr(b, "char>?")); }
long p_charle(long a, long b) { return B(chr(a, "char<=?") <= chr(b, "char<=?")); }
long p_charge(long a, long b) { return B(chr(a, "char>=?") >= chr(b, "char>=?")); }
int upc(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int downc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
long p_charupcase(long a) { return mkchar(upc(chr(a, "char-upcase"))); }
long p_chardowncase(long a) { return mkchar(downc(chr(a, "char-downcase"))); }
long p_charalphap(long a) { int c = downc(chr(a, "char-alphabetic?")); return B(c >= 'a' && c <= 'z'); }
long p_charnumericp(long a) { int c = chr(a, "char-numeric?"); return B(c >= '0' && c <= '9'); }
long p_charwhitespacep(long a) { int c = chr(a, "char-whitespace?"); return B(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 12); }
long p_charupperp(long a) { int c = chr(a, "char-upper-case?"); return B(c >= 'A' && c <= 'Z'); }
long p_charlowerp(long a) { int c = chr(a, "char-lower-case?"); return B(c >= 'a' && c <= 'z'); }
long p_digitvalue(long a) { int c = chr(a, "digit-value"); if (c >= '0' && c <= '9') return mkint(c - '0'); return FALSE; }
long p_charcieq(long a, long b) { return B(downc(chr(a, "char-ci=?")) == downc(chr(b, "char-ci=?"))); }

/* --------------------------------------------------------------- strings */

long p_stringp(long a) { return B(otype(a) == OSTR); }
long str(long a, char *fn) { if (otype(a) != OSTR) typeerr(fn, "string", a); return a; }
long idx(long k, int n, char *fn) {
  long i;
  if (!isint(k)) typeerr(fn, "index", k);
  i = ival(k);
  if (i < 0 || i >= n) err("index out of range:", k, 1);
  return i;
}
long p_makestring(long k, long c) {
  long s; int n = (int)intarg(k, "make-string"); int i; char *p;
  s = mkstr("", 0);
  s = (long)(int)alloc(17 + n);
  op(s)[0] = OSTR; op(s)[1] = n; op(s)[2] = 0; op(s)[3] = 0;
  p = sptr(s);
  for (i = 0; i < n; i++) p[i] = c == NIL ? ' ' : chr(c, "make-string");
  p[n] = 0;
  return s;
}
long p_makestring1(long k) { return p_makestring(k, NIL); }
long p_stringlength(long s) { return mkint(sl(str(s, "string-length"))); }
long p_stringref(long s, long k) { str(s, "string-ref"); return mkchar(sptr(s)[idx(k, sl(s), "string-ref")]); }
long p_stringset(long s, long k, long c) { str(s, "string-set!"); sptr(s)[idx(k, sl(s), "string-set!")] = chr(c, "string-set!"); return NIL; }
long p_substring(long s, long a, long b) {
  int i = (int)intarg(a, "substring"); int j = (int)intarg(b, "substring");
  str(s, "substring");
  if (i < 0 || j > sl(s) || i > j) err("substring: bad range:", cons(a, cons(b, NULL)), 1);
  return mkstr(sptr(s) + i, j - i);
}
long p_substring2(long s, long a) { return p_substring(s, a, mkint(sl(str(s, "substring")))); }
long p_stringcopy(long s) { return p_substring2(s, mkint(0)); }
long p_stringappend(long a, long b) {
  str(a, "string-append"); str(b, "string-append");
  sbn = 0; sbputs(sptr(a), sl(a)); sbputs(sptr(b), sl(b));
  return sbstr();
}
long p_string2list(long s) {
  long l = NULL; int i;
  str(s, "string->list");
  for (i = sl(s) - 1; i >= 0; i--) l = cons(mkchar(sptr(s)[i]), l);
  return l;
}
long p_list2string(long l) {
  sbn = 0;
  while (otype(l) == OPAIR) { sbput(chr(car_(l), "list->string")); l = cdr_(l); }
  return sbstr();
}
int strcmp2(long a, long b, char *fn) {
  int n; int m; int i; int c;
  str(a, fn); str(b, fn);
  n = sl(a); m = sl(b);
  for (i = 0; i < n && i < m; i++) {
    c = (sptr(a)[i] & 255) - (sptr(b)[i] & 255);
    if (c) return c;
  }
  return n - m;
}
long p_streq(long a, long b) { return B(strcmp2(a, b, "string=?") == 0); }
long p_strlt(long a, long b) { return B(strcmp2(a, b, "string<?") < 0); }
long p_strgt(long a, long b) { return B(strcmp2(a, b, "string>?") > 0); }
long p_strle(long a, long b) { return B(strcmp2(a, b, "string<=?") <= 0); }
long p_strge(long a, long b) { return B(strcmp2(a, b, "string>=?") >= 0); }
long p_stringfill(long s, long c) {
  int i; int ch = chr(c, "string-fill!");
  str(s, "string-fill!");
  for (i = 0; i < sl(s); i++) sptr(s)[i] = ch;
  return NIL;
}
long p_stringupcase(long s) { long r = p_stringcopy(s); int i; for (i = 0; i < sl(r); i++) sptr(r)[i] = upc(sptr(r)[i]); return r; }
long p_stringdowncase(long s) { long r = p_stringcopy(s); int i; for (i = 0; i < sl(r); i++) sptr(r)[i] = downc(sptr(r)[i]); return r; }

/* --------------------------------------------------------------- vectors */

long p_vectorp(long a) { return B(otype(a) == OVEC); }
long vec(long a, char *fn) { if (otype(a) != OVEC) typeerr(fn, "vector", a); return a; }
long p_makevector(long k, long fill) {
  long n = intarg(k, "make-vector");
  if (n < 0) err("make-vector: negative length:", k, 1);
  return mkvec((int)n, fill);
}
long p_makevector1(long k) { return p_makevector(k, FALSE); }
long p_vectorlength(long v) { return mkint(vlen(vec(v, "vector-length"))); }
long p_vectorref(long v, long k) { vec(v, "vector-ref"); return velts(v)[idx(k, vlen(v), "vector-ref")]; }
long p_vectorset(long v, long k, long x) { vec(v, "vector-set!"); velts(v)[idx(k, vlen(v), "vector-set!")] = x; return NIL; }
long p_vector2list(long v) {
  long l = NULL; int i;
  vec(v, "vector->list");
  for (i = vlen(v) - 1; i >= 0; i--) l = cons(velts(v)[i], l);
  return l;
}
long p_list2vector(long l) { return list2vec(l); }
long p_vectorfill(long v, long x) { int i; vec(v, "vector-fill!"); for (i = 0; i < vlen(v); i++) velts(v)[i] = x; return NIL; }
long p_vectorcopy(long v) {
  long r; int i;
  vec(v, "vector-copy");
  r = mkvec(vlen(v), NIL);
  for (i = 0; i < vlen(v); i++) velts(r)[i] = velts(v)[i];
  return r;
}

/* ----------------------------------------------------- records, promises */

/* a record type is #(name field-count); a record is [OREC, n, type, fields] */
long p_mkrtd(long name, long n) { long t = mkvec(2, name); velts(t)[1] = n; return t; }
long p_recalloc(long t) {
  int n = (int)ival(velts(t)[1]); int *o = (int *)alloc(16 + 8 * n); int i;
  o[0] = OREC; o[1] = n;
  *(long *)(o + 2) = t;
  for (i = 0; i < n; i++) *(long *)(o + 4 + 2 * i) = FALSE;
  return (int)o;
}
long p_recp(long o, long t) { return B(otype(o) == OREC && *(long *)(op(o) + 2) == t); }
long rec(long o, long t) {
  if (otype(o) != OREC || *(long *)(op(o) + 2) != t) {
    flush(); sbn = 0;
    sbcstr("Error: not a "); sbval(velts(t)[0], 0); sbcstr(": "); sbval(o, 1); sbput('\n');
    sys_write(2, sb, sbn); proc_exit(1);
  }
  return o;
}
long p_recref(long o, long t, long i) { rec(o, t); return *(long *)(op(o) + 4 + 2 * (int)ival(i)); }
long p_recset(long o, long t, long i, long v) { rec(o, t); *(long *)(op(o) + 4 + 2 * (int)ival(i)) = v; return NIL; }

/* a promise is [OPROM, done, value or thunk] */
long p_makepromise(long done, long x) {
  int *o = (int *)alloc(16);
  o[0] = OPROM; o[1] = done != FALSE;
  *(long *)(o + 2) = x;
  return (int)o;
}
long p_promisep(long a) { return B(otype(a) == OPROM); }
int v_force(int clo, int n) {
  long p = stk[0]; int *o; long q; int r;
  if (n != 1) return arityerr(clo, n);
  if (otype(p) != OPROM) return 1;   /* force of a non-promise: itself */
  o = op(p);
  while (!o[1]) {
    r = call(*(long *)(o + 2), 0);
    if (r < 0) return r;
    q = stk[0];
    if (o[1]) break;
    if (otype(q) == OPROM) {   /* delay-force: take over q's state */
      o[1] = op(q)[1];
      *(long *)(o + 2) = *(long *)(op(q) + 2);
      op(q)[1] = o[1]; *(long *)(op(q) + 2) = *(long *)(o + 2);
    } else {
      o[1] = 1;
      *(long *)(o + 2) = q;
    }
  }
  stk[0] = *(long *)(o + 2);
  return 1;
}

/* -------------------------------------------------------------- control */

long p_procedurep(long a) { return B(otype(a) == OFUN); }

/* variadic arithmetic and constructors */
int v_add(int clo, int n) { long r = mkint(0); int i; for (i = 0; i < n; i++) r = n_add(r, stk[i]); stk[0] = r; return 1; }
int v_mul(int clo, int n) { long r = mkint(1); int i; for (i = 0; i < n; i++) r = n_mul(r, stk[i]); stk[0] = r; return 1; }
int v_sub(int clo, int n) {
  long r; int i;
  if (n == 0) return arityerr(clo, n);
  if (n == 1) { stk[0] = n_neg(stk[0]); return 1; }
  r = stk[0];
  for (i = 1; i < n; i++) r = n_sub(r, stk[i]);
  stk[0] = r; return 1;
}
int v_div(int clo, int n) {
  long r; int i;
  if (n == 0) return arityerr(clo, n);
  if (n == 1) { stk[0] = p_div(mkint(1), stk[0]); return 1; }
  r = stk[0];
  for (i = 1; i < n; i++) r = p_div(r, stk[i]);
  stk[0] = r; return 1;
}
/* (function pointers in cc are int -> int, so these dispatch on a code) */
long bin(int f, long a, long b) {
  switch (f) {
  case 0: return p_numeq(a, b);
  case 1: return p_lt(a, b);
  case 2: return p_gt(a, b);
  case 3: return p_le(a, b);
  case 4: return p_ge(a, b);
  case 5: return p_max(a, b);
  case 6: return p_min(a, b);
  case 7: return p_gcd(a, b);
  case 8: return p_lcm(a, b);
  }
  return NIL;
}
int cmpchain(int clo, int n, int f) {
  int i; long r = TRUE;
  if (n < 1) return arityerr(clo, n);
  if (n == 1) numd(stk[0], "comparison");
  for (i = 0; i + 1 < n; i++) if (bin(f, stk[i], stk[i + 1]) == FALSE) r = FALSE;
  stk[0] = r; return 1;
}
int v_numeq(int clo, int n) { return cmpchain(clo, n, 0); }
int v_lt(int clo, int n) { return cmpchain(clo, n, 1); }
int v_gt(int clo, int n) { return cmpchain(clo, n, 2); }
int v_le(int clo, int n) { return cmpchain(clo, n, 3); }
int v_ge(int clo, int n) { return cmpchain(clo, n, 4); }
int fold(int clo, int n, int f) {
  long r; int i;
  if (n < 1) return arityerr(clo, n);
  r = stk[0];
  for (i = 1; i < n; i++) r = bin(f, r, stk[i]);
  stk[0] = r; return 1;
}
int v_max(int clo, int n) { if (n == 1) numd(stk[0], "max"); return fold(clo, n, 5); }
int v_min(int clo, int n) { if (n == 1) numd(stk[0], "min"); return fold(clo, n, 6); }
int v_gcd(int clo, int n) { if (n == 0) { stk[0] = mkint(0); return 1; } return fold(clo, n, 7); }
int v_lcm(int clo, int n) { if (n == 0) { stk[0] = mkint(1); return 1; } return fold(clo, n, 8); }
int v_list(int clo, int n) { stk[0] = restlist(0, n); return 1; }
int v_vector(int clo, int n) {
  long v = mkvec(n, NIL); int i;
  for (i = 0; i < n; i++) velts(v)[i] = stk[i];
  stk[0] = v; return 1;
}
int v_append(int clo, int n) {
  long r; int i;
  if (n == 0) { stk[0] = NULL; return 1; }
  r = stk[n - 1];
  for (i = n - 2; i >= 0; i--) r = p_append(stk[i], r);
  stk[0] = r; return 1;
}
int v_stringappend(int clo, int n) {
  int i;
  sbn = 0;
  for (i = 0; i < n; i++) { str(stk[i], "string-append"); sbputs(sptr(stk[i]), sl(stk[i])); }
  stk[0] = sbstr(); return 1;
}
int v_string(int clo, int n) {
  int i;
  sbn = 0;
  for (i = 0; i < n; i++) sbput(chr(stk[i], "string"));
  stk[0] = sbstr(); return 1;
}

/* ------------------------------------------------ higher-order, control */

int v_apply(int clo, int n) {
  long f; long l; int i; int m;
  if (n < 1) return arityerr(clo, n);
  f = stk[0];
  if (n == 1) return call(f, 0);
  for (i = 1; i < n - 1; i++) stk[i - 1] = stk[i];
  l = stk[n - 1]; m = n - 2;
  while (otype(l) == OPAIR) { stk[m++] = car_(l); l = cdr_(l); }
  if (l != NULL) typeerr("apply", "list", stk[n - 1]);
  return call(f, m);
}
/* map and for-each over one or more lists; vectors and strings likewise */
int mapn(int clo, int n, int keep, char *fn) {
  long f; long ls; long l; long head = NULL; long tail = NULL; long c; long args; int m; int i; int r;
  if (n < 2) return arityerr(clo, n);
  f = stk[0];
  ls = restlist(1, n);
  m = n - 1;
  while (1) {
    for (l = ls; l != NULL; l = cdr_(l)) if (otype(car_(l)) != OPAIR) {
      stk[0] = keep ? head : NIL;
      return 1;
    }
    args = NULL;
    for (l = ls; l != NULL; l = cdr_(l)) { args = cons(car_(car_(l)), args); *(long *)((int)l + 8) = cdr_(car_(l)); }
    i = m;
    while (args != NULL) { stk[--i] = car_(args); args = cdr_(args); }
    r = call(f, m);
    if (r < 0) return r;
    if (keep) {
      c = cons(stk[0], NULL);
      if (head == NULL) head = c; else *(long *)((int)tail + 16) = c;
      tail = c;
    }
  }
  return 0;
}
int v_map(int clo, int n) {
  long f; long l; long head = NULL; long tail = NULL; long c; int r;
  if (n != 2) return mapn(clo, n, 1, "map");
  f = stk[0]; l = stk[1];
  while (otype(l) == OPAIR) {
    stk[0] = car_(l);
    r = call(f, 1);
    if (r < 0) return r;
    c = cons(stk[0], NULL);
    if (head == NULL) head = c; else *(long *)((int)tail + 16) = c;
    tail = c;
    l = cdr_(l);
  }
  stk[0] = head; return 1;
}
int v_foreach(int clo, int n) {
  long f; long l; int r;
  if (n != 2) return mapn(clo, n, 0, "for-each");
  f = stk[0]; l = stk[1];
  while (otype(l) == OPAIR) {
    stk[0] = car_(l);
    r = call(f, 1);
    if (r < 0) return r;
    l = cdr_(l);
  }
  stk[0] = NIL; return 1;
}
int v_vectormap(int clo, int n) {
  long f; long v; long w; int i; int r;
  if (n != 2) err("vector-map: only one vector is supported", NIL, 0);
  f = stk[0]; v = vec(stk[1], "vector-map");
  w = mkvec(vlen(v), NIL);
  for (i = 0; i < vlen(v); i++) {
    stk[0] = velts(v)[i];
    r = call(f, 1);
    if (r < 0) return r;
    velts(w)[i] = stk[0];
  }
  stk[0] = w; return 1;
}
int v_vectorforeach(int clo, int n) {
  long f; long v; int i; int r;
  if (n != 2) err("vector-for-each: only one vector is supported", NIL, 0);
  f = stk[0]; v = vec(stk[1], "vector-for-each");
  for (i = 0; i < vlen(v); i++) {
    stk[0] = velts(v)[i];
    r = call(f, 1);
    if (r < 0) return r;
  }
  stk[0] = NIL; return 1;
}
int v_stringforeach(int clo, int n) {
  long f; long s; int i; int r;
  if (n != 2) err("string-for-each: only one string is supported", NIL, 0);
  f = stk[0]; s = str(stk[1], "string-for-each");
  for (i = 0; i < sl(s); i++) {
    stk[0] = mkchar(sptr(s)[i]);
    r = call(f, 1);
    if (r < 0) return r;
  }
  stk[0] = NIL; return 1;
}
/* member and assoc with a third argument, the equality predicate */
int v_member3(int clo, int n) {
  long x; long l; long f; int r;
  if (n != 3) return arityerr(clo, n);
  x = stk[0]; l = stk[1]; f = stk[2];
  while (otype(l) == OPAIR) {
    stk[0] = x; stk[1] = car_(l);
    r = call(f, 2);
    if (r < 0) return r;
    if (stk[0] != FALSE) { stk[0] = l; return 1; }
    l = cdr_(l);
  }
  stk[0] = FALSE; return 1;
}
int v_assoc3(int clo, int n) {
  long x; long l; long f; int r;
  if (n != 3) return arityerr(clo, n);
  x = stk[0]; l = stk[1]; f = stk[2];
  while (otype(l) == OPAIR) {
    stk[0] = x; stk[1] = p_car(car_(l));
    r = call(f, 2);
    if (r < 0) return r;
    if (stk[0] != FALSE) { stk[0] = car_(l); return 1; }
    l = cdr_(l);
  }
  stk[0] = FALSE; return 1;
}

int v_values(int clo, int n) { return n; }
int v_callwithvalues(int clo, int n) {
  long g; int r;
  if (n != 2) return arityerr(clo, n);
  g = stk[1];
  r = call(stk[0], 0);
  if (r < 0) return r;
  return call(g, r);
}

/* escape-only continuations: invoking one unwinds to its call/cc */
int ncont; int unwn;
int contfn(int clo, int n) {
  long *fr = cfree(clo);
  if (fr[1] == FALSE) err("continuation invoked after its extent ended (only escaping continuations are supported)", NIL, 0);
  unwid = (int)ival(fr[0]); unwn = n;
  return -1;
}
int v_callcc(int clo, int n) {
  long k; long f; int id; int r;
  if (n != 1) return arityerr(clo, n);
  id = ++ncont;
  k = mkclo(contfn, 2);
  cfree(k)[0] = mkint(id); cfree(k)[1] = TRUE;
  f = stk[0]; stk[0] = k;
  r = call(f, 1);
  cfree(k)[1] = FALSE;
  if (r < 0 && unwid == id) return unwn;
  return r;
}

/* ------------------------------------------------------------ exceptions */

long handlers = NULL;   /* the installed exception handlers, innermost first */
long ERRT;              /* the record type of error objects */

void uncaught(long obj) {
  long l;
  flush();
  sbn = 0;
  sbcstr("Error: ");
  if (otype(obj) == OREC && *(long *)(op(obj) + 2) == ERRT) {
    sbval(p_recref(obj, ERRT, mkint(0)), 0);
    for (l = p_recref(obj, ERRT, mkint(1)); otype(l) == OPAIR; l = cdr_(l)) { sbput(' '); sbval(car_(l), 1); }
  } else {
    sbcstr("uncaught exception: "); sbval(obj, 1);
  }
  sbput('\n');
  sys_write(2, sb, sbn);
  proc_exit(1);
}
int raise(long obj, int continuable) {
  long h; long saved = handlers; int r;
  if (handlers == NULL) uncaught(obj);
  h = car_(handlers);
  handlers = cdr_(handlers);
  stk[0] = obj;
  r = call(h, 1);
  handlers = saved;
  if (r < 0 || continuable) return r;
  err("exception handler returned from a non-continuable raise:", obj, 1);
  return -1;
}
int v_raise(int clo, int n) { if (n != 1) return arityerr(clo, n); return raise(stk[0], 0); }
int v_raisecontinuable(int clo, int n) { if (n != 1) return arityerr(clo, n); return raise(stk[0], 1); }
int v_error(int clo, int n) {
  long e;
  if (n < 1) return arityerr(clo, n);
  e = p_recalloc(ERRT);
  p_recset(e, ERRT, mkint(0), stk[0]);
  p_recset(e, ERRT, mkint(1), restlist(1, n));
  return raise(e, 0);
}
long p_errorobjectp(long e) { return p_recp(e, ERRT); }
long p_errorobjectmessage(long e) { return p_recref(e, ERRT, mkint(0)); }
long p_errorobjectirritants(long e) { return p_recref(e, ERRT, mkint(1)); }
int v_withexceptionhandler(int clo, int n) {
  long saved = handlers; int r;
  if (n != 2) return arityerr(clo, n);
  handlers = cons(stk[0], handlers);
  r = call(stk[1], 0);
  handlers = saved;
  return r;
}
/* (guard (e clause ...) body ...) is (%guard (lambda () body ...) (lambda (e) (cond clause ... (else (raise-continuable e))))) */
int v_guard(int clo, int n) {
  long saved = handlers; long k; long h; int id; int r;
  if (n != 2) return arityerr(clo, n);
  h = stk[1];
  id = ++ncont;
  k = mkclo(contfn, 2);
  cfree(k)[0] = mkint(id); cfree(k)[1] = TRUE;
  handlers = cons(k, handlers);
  r = call(stk[0], 0);
  handlers = saved;
  cfree(k)[1] = FALSE;
  if (r < 0 && unwid == id) return call(h, 1);
  return r;
}

long savevals(int n) { long v = mkvec(n, NIL); int i; for (i = 0; i < n; i++) velts(v)[i] = stk[i]; return v; }
int restorevals(long v) { int i; for (i = 0; i < vlen(v); i++) stk[i] = velts(v)[i]; return vlen(v); }
int v_dynamicwind(int clo, int n) {
  long before; long thunk; long after; long saved; int r; int id; int m;
  if (n != 3) return arityerr(clo, n);
  before = stk[0]; thunk = stk[1]; after = stk[2];
  r = call(before, 0);
  if (r < 0) return r;
  r = call(thunk, 0);
  if (r < 0) {
    id = unwid; m = unwn;
    saved = savevals(m);
    if (call(after, 0) < 0) return -1;
    restorevals(saved);
    unwid = id; unwn = m;
    return r;
  }
  saved = savevals(r);
  if (call(after, 0) < 0) return -1;
  return restorevals(saved);
}

int v_exit(int clo, int n) {
  int code = 0;
  if (n > 0) {
    if (stk[0] == FALSE) code = 1;
    else if (isint(stk[0])) code = (int)ival(stk[0]);
  }
  flush();
  proc_exit(code);
  return 0;
}

/* parameters: a closure whose first free value is the current value, and
 * whose second is the converter */
int paramfn(int clo, int n) {
  if (n != 0) err("parameters take no arguments (to set one, use parameterize)", NIL, 0);
  stk[0] = cfree(clo)[0];
  return 1;
}
int v_makeparameter(int clo, int n) {
  long p; long v; long conv = FALSE; int r;
  if (n < 1 || n > 2) return arityerr(clo, n);
  v = stk[0];
  if (n == 2) { conv = stk[1]; stk[0] = v; r = call(conv, 1); if (r < 0) return r; v = stk[0]; }
  p = mkclo(paramfn, 2);
  cfree(p)[0] = v; cfree(p)[1] = conv;
  stk[0] = p; return 1;
}
/* (%parameterize params values thunk) */
int v_parameterize(int clo, int n) {
  long ps = stk[0]; long vs = stk[1]; long thunk = stk[2]; long olds = NULL; long l; long m; long p; long v;
  int r; int id; int k; long saved;
  m = vs;
  for (l = ps; l != NULL; l = cdr_(l)) {
    p = car_(l); v = car_(m); m = cdr_(m);
    if (otype(p) != OFUN || op(p)[1] != paramfn) err("parameterize: not a parameter:", p, 1);
    if (cfree(p)[1] != FALSE) { stk[0] = v; r = call(cfree(p)[1], 1); if (r < 0) return r; v = stk[0]; }
    olds = cons(cfree(p)[0], olds);
    cfree(p)[0] = v;
  }
  r = call(thunk, 0);
  id = unwid; k = unwn;
  saved = savevals(r < 0 ? k : r);
  olds = p_reverse(olds);
  for (l = ps; l != NULL; l = cdr_(l)) { cfree(car_(l))[0] = car_(olds); olds = cdr_(olds); }
  restorevals(saved);
  unwid = id; unwn = k;
  return r;
}

/* ------------------------------------------------------------------- I/O */

/* a port is a record: kind, buffer (a malloc'd pointer), length, position.
 * kinds: 0 stdout, 1 stderr, 2 string output, 3 stdin, 4 string input */
long PORTT; long STDOUT; long STDERR; long STDIN;
long mkport(int kind) {
  long p = p_recalloc(PORTT);
  p_recset(p, PORTT, mkint(0), mkint(kind));
  p_recset(p, PORTT, mkint(1), mkint(0));
  p_recset(p, PORTT, mkint(2), mkint(0));
  p_recset(p, PORTT, mkint(3), mkint(0));
  return p;
}
long *pf(long p) { return (long *)((int)p + 16); }
int portkind(long p) {
  if (otype(p) != OREC || *(long *)(op(p) + 2) != PORTT) typeerr("port operation", "port", p);
  return (int)ival(pf(p)[0]);
}
void pout(long port, char *s, int n) {
  int k = portkind(port); char *b; int len; int cap; char *nb;
  if (k == 0) { outb(s, n); return; }
  if (k == 1) { flush(); sys_write(2, s, n); return; }
  if (k != 2) typeerr("write", "output port", port);
  b = (char *)(int)ival(pf(port)[1]); len = (int)ival(pf(port)[2]); cap = (int)ival(pf(port)[3]);
  if (len + n > cap) {
    while (len + n > cap) cap = cap ? cap * 2 : 256;
    nb = malloc(cap); memcopy(nb, b, len); b = nb;
    pf(port)[1] = mkint((int)b); pf(port)[3] = mkint(cap);
  }
  memcopy(b + len, s, n);
  pf(port)[2] = mkint(len + n);
}
long outval(long x, long port, int wr) {
  sbn = 0; sbval(x, wr);
  pout(port, sb, sbn); sbn = 0;
  return NIL;
}
long p_display(long x) { sbn = 0; sbval(x, 0); outb(sb, sbn); sbn = 0; return NIL; }
long p_display2(long x, long port) { return outval(x, port, 0); }
long p_write(long x) { sbn = 0; sbval(x, 1); outb(sb, sbn); sbn = 0; return NIL; }
long p_write2(long x, long port) { return outval(x, port, 1); }
long p_newline() { outb("\n", 1); return NIL; }
long p_newline1(long port) { pout(port, "\n", 1); return NIL; }
long p_writechar(long c) { char *b = sb; sbn = 0; sbput(chr(c, "write-char")); outb(sb, 1); sbn = 0; return NIL; }
long p_writechar2(long c, long port) { sbn = 0; sbput(chr(c, "write-char")); pout(port, sb, 1); sbn = 0; return NIL; }
long p_writestring(long s) { str(s, "write-string"); outb(sptr(s), sl(s)); return NIL; }
long p_writestring2(long s, long port) { str(s, "write-string"); pout(port, sptr(s), sl(s)); return NIL; }
long p_currentoutputport() { return STDOUT; }
long p_currenterrorport() { return STDERR; }
long p_currentinputport() { return STDIN; }
long p_flushoutputport() { flush(); return NIL; }
long p_flushoutputport1(long p) { flush(); return NIL; }
long p_openoutputstring() { return mkport(2); }
long p_getoutputstring(long p) {
  if (portkind(p) != 2) typeerr("get-output-string", "string output port", p);
  return mkstr((char *)(int)ival(pf(p)[1]), (int)ival(pf(p)[2]));
}
long p_openinputstring(long s) {
  long p = mkport(4);
  str(s, "open-input-string");
  pf(p)[1] = s;
  return p;
}
long p_eofobject() { return EOFV; }
long p_eofobjectp(long x) { return B(x == EOFV); }
/* string input ports: read from them by pointing the reader there */
int strgetc(long p, int peek) {
  long s = pf(p)[1]; int i = (int)ival(pf(p)[3]);
  if (i >= sl(s)) return -1;
  if (!peek) pf(p)[3] = mkint(i + 1);
  return sptr(s)[i] & 255;
}
int pgetc(long p, int peek) {
  int k = portkind(p);
  if (k == 3) return peek ? ipeek() : igetc();
  if (k == 4) return strgetc(p, peek);
  typeerr("read", "input port", p);
  return -1;
}
long p_readchar1(long p) { int c = pgetc(p, 0); return c < 0 ? EOFV : mkchar(c); }
long p_peekchar1(long p) { int c = pgetc(p, 1); return c < 0 ? EOFV : mkchar(c); }
long p_readchar() { return p_readchar1(STDIN); }
long p_peekchar() { return p_peekchar1(STDIN); }
long p_readline1(long p) {
  int c = pgetc(p, 0);
  if (c < 0) return EOFV;
  sbn = 0;
  while (c >= 0 && c != '\n') { sbput(c); c = pgetc(p, 0); }
  return sbstr();
}
long p_readline() { return p_readline1(STDIN); }
long p_charreadyp() { return TRUE; }
long p_read() { return readdatum(); }

long p_unspecified() { return NIL; }

/* ------------------------------------------------------------------ main */

void s_init() {
  ERRT = p_mkrtd(sym("error-object"), mkint(2));
  PORTT = p_mkrtd(sym("port"), mkint(4));
  STDOUT = mkport(0); STDERR = mkport(1); STDIN = mkport(3);
}
void sc_init();
int F0(int clo, int n);
int main() {
  rt_init(); read_init(); s_init(); sc_init();
  if (F0(0, 0) < 0) err("continuation invoked outside its extent", NIL, 0);
  flush();
  return 0;
}
