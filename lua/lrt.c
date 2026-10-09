/* lrt.c -- the runtime for Lua programs compiled by luac (Lua -> C).
 *
 * A value is a long (64 bits):
 *   top 16 bits 0:       nil 0, false 2, true 4, or a pointer to an object
 *                        (whose first word says what it is)
 *   top 16 bits 0xffff:  an integer in the low 48 bits
 *   anything else:       a double, plus 2^48
 * Integers outside 48 bits are boxed objects, interned, so that two equal
 * values are always the same bits.  Memory is never freed.
 *
 * Calling convention: arguments are on the value stack stk[base ..
 * base+nargs); a function leaves its results at stk[base ..] and returns
 * how many there are.  top is the first free slot.
 * Needs num.c first.
 */

enum { NIL = 0, FALSE = 2, TRUE = 4 };
enum { OSTR = 1, OTAB, OFUN, OINT };
enum { STKSZ = 1000000 };

long stk[STKSZ];
int top;

void proc_exit(int code);   /* WASI */
void lua_consts();
int F0(int clo, int base, int nargs);

/* ---------------------------------------------------------------- basics */

int slen(char *s) { int n = 0; while (s[n]) n++; return n; }
int memeq(char *a, char *b, int n) {
  int i = 0;
  while (i < n) { if (a[i] != b[i]) return 0; i++; }
  return 1;
}
void memcopy(char *d, char *s, int n) { int i = 0; while (i < n) { d[i] = s[i]; i++; } }

char obuf[65536];
int obn;
void flush() {
  int n = 0; int w;
  while (n < obn) { w = sys_write(1, obuf + n, obn - n); if (w <= 0) break; n = n + w; }
  obn = 0;
}
void outb(char *p, int n) {
  while (n > 0) { if (obn == 65536) flush(); obuf[obn++] = *p++; n--; }
}
void eputs(char *s) { sys_write(2, s, slen(s)); }

void lerror3(char *a, char *b, char *c) {
  flush();
  eputs("lua: "); eputs(a); eputs(b); eputs(c); eputs("\n");
  proc_exit(1);
}
void lerror(char *msg) { lerror3(msg, "", ""); }

/* ---------------------------------------------------------------- values */

long INTTAG = -281474976710656L;   /* 0xffff000000000000 */
long DOFF = 281474976710656L;      /* 2^48 */
long MASK48 = 281474976710655L;

int isobj(long v) { return (v >> 32) == 0 && v > 8; }
int otype(long v) { if (isobj(v)) return *(int *)(int)v; return 0; }
int istab(long v) { return otype(v) == OTAB; }
int isstr(long v) { return otype(v) == OSTR; }
int isfix(long v) { return (v >> 48) == -1; }
int isdbl(long v) { long t = v >> 48; return t != 0 && t != -1; }
int isint(long v) { return isfix(v) || otype(v) == OINT; }
int isnum(long v) { return isfix(v) || isdbl(v) || otype(v) == OINT; }
long truthy(long v) { return v & -3; }

double dval(long v) { return __f64_from_bits(v - DOFF); }
long mkdbl(double d) {
  long b = __f64_bits(d);
  if ((b >> 49) == -1) b = -2251799813685248L;   /* a NaN that would collide: canonical */
  return b + DOFF;
}

long ints[4096];   /* interned boxed integers: buckets */
long mkint(long n) {
  int h; int *o; long p;
  if (((n << 16) >> 16) == n) return (n & MASK48) | INTTAG;
  h = (int)((n ^ (n >> 29)) * 2654435761L) & 4095;
  p = ints[h];
  while (p) {
    o = (int *)(int)p;
    if (*(long *)(o + 2) == n) return p;
    p = *(long *)(o + 4);
  }
  o = (int *)malloc(24);
  o[0] = OINT;
  *(long *)(o + 2) = n;
  *(long *)(o + 4) = ints[h];
  ints[h] = (long)(int)o;
  return ints[h];
}
long ival(long v) {
  if (isfix(v)) return (v << 16) >> 16;
  return *(long *)((int *)(int)v + 2);
}

char *tname(long v) {
  int t;
  if (v == NIL) return "nil";
  if (v == TRUE || v == FALSE) return "boolean";
  if (isnum(v)) return "number";
  t = otype(v);
  if (t == OSTR) return "string";
  if (t == OTAB) return "table";
  return "function";
}

/* a growable scratch buffer for building strings */
char *sb; int sbn; int sbcap;
void sbneed(int n) {
  char *nb;
  if (sbn + n <= sbcap) return;
  while (sbn + n > sbcap) sbcap = sbcap ? sbcap * 2 : 1024;
  nb = malloc(sbcap);
  memcopy(nb, sb, sbn);
  sb = nb;
}
void sbput(int c) { sbneed(1); sb[sbn++] = c; }
void sbputs(char *p, int n) { sbneed(n); memcopy(sb + sbn, p, n); sbn = sbn + n; }
void sbint(long x) {
  int start; int i; int j; int c; unsigned long u;
  if (x < 0) { sbput('-'); u = -(unsigned long)x; } else u = (unsigned long)x;
  start = sbn;
  do { sbput('0' + (int)(u % 10)); u = u / 10; } while (u);
  i = start; j = sbn - 1;
  while (i < j) { c = sb[i]; sb[i] = sb[j]; sb[j] = c; i++; j--; }
}
void sbhex(unsigned long x, int upper) {
  int d = 60; int c;
  while (d > 0 && !((x >> d) & 15)) d = d - 4;
  while (d >= 0) {
    c = (int)(x >> d) & 15;
    sbput(c < 10 ? '0' + c : (upper ? 'A' : 'a') + c - 10);
    d = d - 4;
  }
}
char fbuf[1400];
void sbdbl(double d, int conv, int prec) { sbputs(fbuf, fmtdbl(d, conv, prec, 0, fbuf)); }
/* a float the way Lua prints it: %.14g, and ".0" if it looks like an integer */
void sbnum(double d) {
  int start = sbn; int i; int isint_ = 1;
  sbdbl(d, 'g', 14);
  for (i = start; i < sbn; i++) if (sb[i] == '.' || sb[i] == 'e' || sb[i] == 'n' || sb[i] == 'i') isint_ = 0;
  if (isint_) sbputs(".0", 2);
}

/* --------------------------------------------------------------- strings */

/* string: [OSTR, length, hash, next in intern chain] then the bytes */
int strtab[65536];

long lstr(char *p, int n) {
  int h = n; int i = 0; int s; int *o; char *d;
  while (i < n) { h = h * 31 + (p[i] & 255); i++; }
  s = strtab[h & 65535];
  while (s) {
    o = (int *)s;
    if (o[1] == n && o[2] == h && memeq((char *)s + 16, p, n)) return s;
    s = o[3];
  }
  o = (int *)malloc(17 + n);
  o[0] = OSTR; o[1] = n; o[2] = h; o[3] = strtab[h & 65535];
  d = (char *)o + 16;
  memcopy(d, p, n);
  d[n] = 0;
  strtab[h & 65535] = (int)o;
  return (int)o;
}
long cstr(char *s) { return lstr(s, slen(s)); }
char *sptr(long v) { return (char *)(int)v + 16; }
int sl(long v) { return ((int *)(int)v)[1]; }
long sbstr() { long s = lstr(sb, sbn); sbn = 0; return s; }

/* a string as a number (Lua's rules), or NIL */
long str2num(long s) {
  char *p = sptr(s); int n = sl(s); int i = 0; int j; int neg = 0; unsigned long v = 0; int d; int any = 0;
  double f;
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
  j = i;
  if (i < n && (p[i] == '-' || p[i] == '+')) { neg = p[i] == '-'; i++; }
  if (i + 1 < n && p[i] == '0' && (p[i + 1] == 'x' || p[i + 1] == 'X')) {   /* hex: wraps */
    i = i + 2;
    while (i < n) {
      d = p[i];
      if (d >= '0' && d <= '9') d = d - '0';
      else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
      else if (d >= 'A' && d <= 'F') d = d - 'A' + 10;
      else break;
      v = v * 16 + d; i++; any = 1;
    }
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    if (!any || i != n) return NIL;
    return mkint(neg ? -(long)v : (long)v);
  }
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    if (v > 922337203685477580UL || (v == 922337203685477580UL && p[i] > '7' + neg)) any = 2;
    v = v * 10 + p[i] - '0'; i++; if (!any) any = 1;
  }
  d = i;
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
  if (any == 1 && i == n) return mkint(neg ? -(long)v : (long)v);
  f = str2dbl(p + j, n - j);
  if (!numok) return NIL;
  return mkdbl(f);
}

/* ---------------------------------------------------------------- tables */

/* table: [OTAB, array, n, array capacity, hash keys, hash values,
 *         hash capacity, hash count, metatable]  (ints; the arrays hold longs) */
int newtab() {
  int *t = (int *)malloc(36);
  t[0] = OTAB; t[1] = 0; t[2] = 0; t[3] = 0; t[4] = 0; t[5] = 0; t[6] = 0; t[7] = 0; t[8] = 0;
  return (int)t;
}
long newtable() { return newtab(); }
int khash(long k) { long h = (k ^ (k >> 32)) * -7046029254386353131L; return (int)(h >> 32); }
int hslot(int *t, long k) {
  int m = t[6] - 1; int i = khash(k) & m; long *hk = (long *)t[4];
  while (hk[i] && hk[i] != k) i = (i + 1) & m;
  return i;
}
int fok;   /* set by flt2int */
long flt2int(double d);
long tget(long tv, long k) {
  int *t = (int *)(int)tv; long i; int s;
  if (isfix(k)) {
    i = (k << 16) >> 16;
    if (i >= 1 && i <= t[2]) return ((long *)t[1])[i - 1];
  } else if (isdbl(k)) {
    i = flt2int(dval(k));
    if (fok) return tget(tv, mkint(i));
  }
  if (!t[6]) return NIL;
  s = hslot(t, k);
  if (((long *)t[4])[s] == k) return ((long *)t[5])[s];
  return NIL;
}
void hset(int *t, long k, long v);
void hresize(int *t) {
  int oc = t[6]; long *ok = (long *)t[4]; long *ov = (long *)t[5]; int i; int nc = 4;
  while (nc * 3 <= t[7] * 4 + 4) nc = nc * 2;
  t[4] = (int)malloc(nc * 8); t[5] = (int)malloc(nc * 8); t[6] = nc; t[7] = 0;
  for (i = 0; i < oc; i++) if (ok[i] && ov[i] != NIL) hset(t, ok[i], ov[i]);
}
void hset(int *t, long k, long v) {
  int i; long *hk;
  if (!t[6]) { if (v == NIL) return; hresize(t); }
  i = hslot(t, k); hk = (long *)t[4];
  if (!hk[i]) {
    if (v == NIL) return;
    if ((t[7] + 1) * 4 > t[6] * 3) { hresize(t); i = hslot(t, k); hk = (long *)t[4]; }
    hk[i] = k; t[7]++;
  }
  ((long *)t[5])[i] = v;
}
void aappend(int *t, long v) {
  long *na; int nc;
  if (t[2] == t[3]) {
    nc = t[3] ? t[3] * 2 : 4;
    na = (long *)malloc(nc * 8);
    memcopy((char *)na, (char *)t[1], t[2] * 8);
    t[1] = (int)na; t[3] = nc;
  }
  ((long *)t[1])[t[2]++] = v;
}
void tset(long tv, long k, long v) {
  int *t = (int *)(int)tv; long i; long *a; int j; long k2;
  if (isfix(k)) {
    i = (k << 16) >> 16;
    if (i >= 1 && i <= t[2]) {
      a = (long *)t[1];
      a[i - 1] = v;
      if (v == NIL && i == t[2]) while (t[2] > 0 && a[t[2] - 1] == NIL) t[2]--;
      return;
    }
    if (i == t[2] + 1 && v != NIL) {
      aappend(t, v);
      if (t[6]) {
        hset(t, k, NIL);
        while (1) {   /* move following keys from the hash part */
          k2 = mkint(t[2] + 1);
          j = hslot(t, k2);
          if (((long *)t[4])[j] != k2 || ((long *)t[5])[j] == NIL) break;
          aappend(t, ((long *)t[5])[j]);
          ((long *)t[5])[j] = NIL;
        }
      }
      return;
    }
  } else if (isdbl(k)) {
    if (dval(k) != dval(k)) lerror("table index is NaN");
    i = flt2int(dval(k));
    if (fok) { tset(tv, mkint(i), v); return; }
  }
  if (k == NIL) lerror("table index is nil");
  hset(t, k, v);
}
int tlen(long tv) { return ((int *)(int)tv)[2]; }

/* next(t, k): the key after k, writing key and value to stk[at] */
int tnext(long tv, long k, int at) {
  int *t = (int *)(int)tv; int pos; int j; long *hk; long *hv; long i;
  if (isdbl(k)) { i = flt2int(dval(k)); if (fok) k = mkint(i); }
  if (k == NIL) pos = 0;
  else if (isfix(k) && ival(k) >= 1 && ival(k) <= t[2]) pos = (int)ival(k);
  else {
    if (!t[6] || ((long *)t[4])[hslot(t, k)] != k) lerror("invalid key to 'next'");
    pos = t[2] + hslot(t, k) + 1;
  }
  while (pos < t[2]) {
    if (((long *)t[1])[pos] != NIL) {
      stk[at] = mkint(pos + 1); stk[at + 1] = ((long *)t[1])[pos];
      return 2;
    }
    pos++;
  }
  hk = (long *)t[4]; hv = (long *)t[5];
  for (j = pos - t[2]; j < t[6]; j++) {
    if (hk[j] && hv[j] != NIL) { stk[at] = hk[j]; stk[at + 1] = hv[j]; return 2; }
  }
  stk[at] = NIL;
  return 1;
}

/* ------------------------------------------------------ cells, closures */

int newcell(long v) { long *c = (long *)malloc(8); *c = v; return (int)c; }
long cget(long c) { return *(long *)(int)c; }
void cset(long c, long v) { *(long *)(int)c = v; }

/* closure: [OFUN, C function, number of upvalues, upvalue cells...] */
long mkclo(int fn, int nup) {
  int *o = (int *)malloc(12 + 4 * nup);
  o[0] = OFUN; o[1] = fn; o[2] = nup;
  return (int)o;
}
void setuv(long c, int i, long cell) { ((int *)(int)c)[3 + i] = (int)cell; }
long uvcell(int c, int i) { return ((int *)c)[3 + i]; }
long uvget(int c, int i) { return *(long *)(((int *)c)[3 + i]); }
void uvset(int c, int i, long v) { *(long *)(((int *)c)[3 + i]) = v; }

long s_index; long s_newindex; long s_call; long s_tostring; long s_len; long s_eq;
long s_add; long s_sub; long s_mul; long s_div; long s_mod; long s_unm; long s_concat; long s_lt; long s_le;
long s_idiv;
long strlib;
long globals;

long metaof(long v) { if (istab(v)) return ((int *)(int)v)[8]; return NIL; }
long metafield(long v, long k) { long m = metaof(v); if (m == NIL) return NIL; return tget(m, k); }

int lcall(long f, int base, int nargs) {
  int fp; long h; int i;
  if (base > STKSZ - 1000) lerror("stack overflow");
  if (otype(f) == OFUN) { fp = ((int *)(int)f)[1]; return fp((int)f, base, nargs); }
  h = metafield(f, s_call);
  if (h != NIL) {
    for (i = nargs; i > 0; i--) stk[base + i] = stk[base + i - 1];
    stk[base] = f;
    return lcall(h, base, nargs + 1);
  }
  lerror3("attempt to call a ", tname(f), " value");
  return 0;
}

/* call f with up to two arguments; return its first result */
long call2(long f, long a, long b, int nargs) {
  int b0 = top; int n;
  stk[top++] = a; stk[top++] = b;
  n = lcall(f, b0, nargs);
  top = b0;
  return n ? stk[b0] : NIL;
}

long gget(long k) { return tget(globals, k); }
void gset(long k, long v) { tset(globals, k, v); }

long lindex(long o, long k) {
  long v; long h;
  if (istab(o)) {
    v = tget(o, k);
    if (v != NIL || !((int *)(int)o)[8]) return v;
    h = tget(((int *)(int)o)[8], s_index);
    if (h == NIL) return NIL;
    if (istab(h)) return lindex(h, k);
    return call2(h, o, k, 2);
  }
  if (isstr(o)) return tget(strlib, k);
  lerror3("attempt to index a ", tname(o), " value");
  return NIL;
}
void lsetindex(long o, long k, long v) {
  long h; int b0;
  if (!istab(o)) lerror3("attempt to index a ", tname(o), " value");
  if (((int *)(int)o)[8] && tget(o, k) == NIL) {
    h = tget(((int *)(int)o)[8], s_newindex);
    if (istab(h)) { lsetindex(h, k, v); return; }
    if (h != NIL) {
      b0 = top;
      stk[top++] = o; stk[top++] = k; stk[top++] = v;
      lcall(h, b0, 3);
      top = b0;
      return;
    }
  }
  tset(o, k, v);
}

/* ------------------------------------------------------------ operators */

double TWO63;   /* 2^63, set by lua_init */

/* d as an integer, if it has an exact integer value; fok says whether */
long flt2int(double d) {
  fok = 0;
  if (d != d || d < -TWO63 || d >= TWO63) return 0;
  if (__builtin_floor(d) != d) return 0;
  fok = 1;
  return (long)d;
}
long tonumber(long v) {
  if (isnum(v)) return v;
  if (isstr(v)) return str2num(v);
  return NIL;
}
double todbl(long v) { if (isdbl(v)) return dval(v); return (double)ival(v); }
long toint(long v) {   /* for bitwise operators */
  long i;
  v = tonumber(v);
  if (isint(v)) return ival(v);
  if (isdbl(v)) {
    i = flt2int(dval(v));
    if (fok) return i;
    lerror("number has no integer representation");
  }
  return 0;
}

long arithmeta(long a, long b, long ev, char *what) {
  long h = metafield(a, ev);
  if (h == NIL) h = metafield(b, ev);
  if (h != NIL) return call2(h, a, b, 2);
  if (!isnum(tonumber(a))) lerror3("attempt to ", what, tname(a)[0] == 's' ? " a string value" : " a non-number value");
  lerror3("attempt to ", what, " a non-number value");
  return NIL;
}
/* operands that are numbers or numeric strings, else a metamethod */
int numeric(long a, long b) { return isnum(tonumber(a)) && isnum(tonumber(b)); }

long ladd(long a, long b) {
  if (isfix(a) && isfix(b)) return mkint(((a << 16) >> 16) + ((b << 16) >> 16));
  if (!numeric(a, b)) return arithmeta(a, b, s_add, "perform arithmetic on");
  a = tonumber(a); b = tonumber(b);
  if (isint(a) && isint(b)) return mkint(ival(a) + ival(b));
  return mkdbl(todbl(a) + todbl(b));
}
long lsub(long a, long b) {
  if (isfix(a) && isfix(b)) return mkint(((a << 16) >> 16) - ((b << 16) >> 16));
  if (!numeric(a, b)) return arithmeta(a, b, s_sub, "perform arithmetic on");
  a = tonumber(a); b = tonumber(b);
  if (isint(a) && isint(b)) return mkint(ival(a) - ival(b));
  return mkdbl(todbl(a) - todbl(b));
}
long lmul(long a, long b) {
  if (!numeric(a, b)) return arithmeta(a, b, s_mul, "perform arithmetic on");
  a = tonumber(a); b = tonumber(b);
  if (isint(a) && isint(b)) return mkint(ival(a) * ival(b));
  return mkdbl(todbl(a) * todbl(b));
}
long ldiv(long a, long b) {
  if (!numeric(a, b)) return arithmeta(a, b, s_div, "perform arithmetic on");
  return mkdbl(todbl(tonumber(a)) / todbl(tonumber(b)));
}
long lidiv(long a, long b) {
  long x; long y; long q;
  if (!numeric(a, b)) return arithmeta(a, b, s_idiv, "perform arithmetic on");
  a = tonumber(a); b = tonumber(b);
  if (isint(a) && isint(b)) {
    x = ival(a); y = ival(b);
    if (y == 0) lerror("attempt to perform 'n//0'");
    if (y == -1) return mkint(-x);
    q = x / y;
    if (x % y != 0 && (x < 0) != (y < 0)) q--;
    return mkint(q);
  }
  return mkdbl(__builtin_floor(todbl(a) / todbl(b)));
}
double fmod_(double a, double b);
long lmod(long a, long b) {
  long x; long y; long r; double m; double fb;
  if (!numeric(a, b)) return arithmeta(a, b, s_mod, "perform arithmetic on");
  a = tonumber(a); b = tonumber(b);
  if (isint(a) && isint(b)) {
    x = ival(a); y = ival(b);
    if (y == 0) lerror("attempt to perform 'n%%0'");
    if (y == -1) return mkint(0);
    r = x % y;
    if (r != 0 && (r < 0) != (y < 0)) r = r + y;
    return mkint(r);
  }
  fb = todbl(b);
  m = fmod_(todbl(a), fb);
  if (m > 0 ? fb < 0 : (m < 0 && fb != m)) m = m + fb;
  return mkdbl(m);
}
double powi(double x, long n) {
  double r = 1; int neg = n < 0;
  if (neg) n = -n;
  while (n) { if (n & 1) r = r * x; x = x * x; n = n >> 1; }
  return neg ? 1 / r : r;
}
double lexp(double x); double llog(double x);
double pow_(double x, double y) {
  long n = flt2int(y);
  if (fok && n >= -1024 && n <= 1024) return powi(x, n);
  if (y + y == 1) return __builtin_sqrt(x);
  if (x < 0) return (double)0 / (double)0;
  return lexp(y * llog(x));
}
long lpow(long a, long b) {
  if (!numeric(a, b)) return arithmeta(a, b, s_mul, "perform arithmetic on");
  return mkdbl(pow_(todbl(tonumber(a)), todbl(tonumber(b))));
}
long lunm(long a) {
  if (isfix(a)) return mkint(-((a << 16) >> 16));
  if (!isnum(tonumber(a))) return arithmeta(a, a, s_unm, "perform arithmetic on");
  a = tonumber(a);
  if (isint(a)) return mkint(-ival(a));
  return mkdbl(-dval(a));
}
long bitmeta(long a, long b) {
  if (!isnum(tonumber(a)) || !isnum(tonumber(b))) lerror("attempt to perform bitwise operation on a non-number value");
  return 0;
}
long lband(long a, long b) { bitmeta(a, b); return mkint(toint(a) & toint(b)); }
long lbor(long a, long b) { bitmeta(a, b); return mkint(toint(a) | toint(b)); }
long lbxor(long a, long b) { bitmeta(a, b); return mkint(toint(a) ^ toint(b)); }
long lshl(long a, long b) {
  long x; long y; bitmeta(a, b); x = toint(a); y = toint(b);
  if (y <= -64 || y >= 64) return mkint(0);
  if (y < 0) return mkint((long)((unsigned long)x >> -y));
  return mkint(x << y);
}
long lshr(long a, long b) {
  long x; long y; bitmeta(a, b); x = toint(a); y = toint(b);
  if (y <= -64 || y >= 64) return mkint(0);
  if (y < 0) return mkint(x << -y);
  return mkint((long)((unsigned long)x >> y));
}
long lbnot(long a) { bitmeta(a, a); return mkint(~toint(a)); }

long lnot(long a) { return truthy(a) ? FALSE : TRUE; }

/* numeric comparisons, exact even between integers and floats */
int ltnum(long a, long b) {
  long i; double f;
  if (isint(a) && isint(b)) return ival(a) < ival(b);
  if (isdbl(a) && isdbl(b)) return dval(a) < dval(b);
  if (isint(a)) {   /* i < f */
    i = ival(a); f = dval(b);
    if (f != f) return 0;
    if (f >= TWO63) return 1;
    if (f <= -TWO63) return 0;
    return i < (long)__builtin_ceil(f) || (i < (long)f + (__builtin_floor(f) != f));
  }
  f = dval(a); i = ival(b);   /* f < i */
  if (f != f) return 0;
  if (f >= TWO63) return 0;
  if (f < -TWO63) return 1;
  return (long)__builtin_floor(f) < i;
}
int lenum(long a, long b) {
  long i; double f;
  if (isint(a) && isint(b)) return ival(a) <= ival(b);
  if (isdbl(a) && isdbl(b)) return dval(a) <= dval(b);
  if (isint(a)) {
    i = ival(a); f = dval(b);
    if (f != f) return 0;
    if (f >= TWO63) return 1;
    if (f < -TWO63) return 0;
    return i <= (long)__builtin_floor(f);
  }
  f = dval(a); i = ival(b);
  if (f != f) return 0;
  if (f >= TWO63) return 0;
  if (f < -TWO63) return 1;
  return (long)__builtin_ceil(f) <= i;
}
int eqnum(long a, long b) {
  long i;
  if (isdbl(a) && isdbl(b)) return dval(a) == dval(b);
  if (isdbl(a)) { i = flt2int(dval(a)); return fok && i == ival(b); }
  i = flt2int(dval(b)); return fok && i == ival(a);
}
int rawequal(long a, long b) {
  if (a == b) return !isdbl(a) || dval(a) == dval(a);
  if (isnum(a) && isnum(b)) return eqnum(a, b);
  return 0;
}
long leq(long a, long b) {
  long h;
  if (rawequal(a, b)) return TRUE;
  if (istab(a) && istab(b)) {
    h = metafield(a, s_eq);
    if (h == NIL) h = metafield(b, s_eq);
    if (h != NIL) return truthy(call2(h, a, b, 2)) ? TRUE : FALSE;
  }
  return FALSE;
}
long lne(long a, long b) { return leq(a, b) == TRUE ? FALSE : TRUE; }

int strcmp_(long a, long b) {
  char *p = sptr(a); char *q = sptr(b); int n = sl(a); int m = sl(b); int i = 0;
  while (i < n && i < m) {
    if ((p[i] & 255) != (q[i] & 255)) return (p[i] & 255) - (q[i] & 255);
    i++;
  }
  return n - m;
}
long cmpother(long a, long b, long ev) {
  long h = metafield(a, ev);
  if (h == NIL) h = metafield(b, ev);
  if (h != NIL) return truthy(call2(h, a, b, 2)) ? TRUE : FALSE;
  if (tname(a) == tname(b)) lerror3("attempt to compare two ", tname(a), " values");
  lerror3("attempt to compare ", tname(a), " with another type");
  return FALSE;
}
long llt(long a, long b) {
  if (isfix(a) && isfix(b)) return (a << 16) < (b << 16) ? TRUE : FALSE;
  if (isnum(a) && isnum(b)) return ltnum(a, b) ? TRUE : FALSE;
  if (isstr(a) && isstr(b)) return strcmp_(a, b) < 0 ? TRUE : FALSE;
  return cmpother(a, b, s_lt);
}
long lle(long a, long b) {
  if (isfix(a) && isfix(b)) return (a << 16) <= (b << 16) ? TRUE : FALSE;
  if (isnum(a) && isnum(b)) return lenum(a, b) ? TRUE : FALSE;
  if (isstr(a) && isstr(b)) return strcmp_(a, b) <= 0 ? TRUE : FALSE;
  return cmpother(a, b, s_le);
}
long lgt(long a, long b) { return llt(b, a); }
long lge(long a, long b) { return lle(b, a); }

long tostr(long v);
long llen(long v) {
  long h;
  if (isstr(v)) return mkint(sl(v));
  if (istab(v)) {
    h = metafield(v, s_len);
    if (h != NIL) return call2(h, v, NIL, 1);
    return mkint(tlen(v));
  }
  lerror3("attempt to get length of a ", tname(v), " value");
  return NIL;
}
void sbval(long v) {   /* a string or number, for concatenation */
  if (isint(v)) sbint(ival(v));
  else if (isdbl(v)) sbnum(dval(v));
  else if (isstr(v)) sbputs(sptr(v), sl(v));
  else lerror3("attempt to concatenate a ", tname(v), " value");
}
long lconcat(long a, long b) {
  long h;
  if (!(isnum(a) || isstr(a)) || !(isnum(b) || isstr(b))) {
    h = metafield(a, s_concat);
    if (h == NIL) h = metafield(b, s_concat);
    if (h != NIL) return call2(h, a, b, 2);
  }
  sbn = 0;
  sbval(a); sbval(b);
  return sbstr();
}

/* the values in stk[from .. from+n) are pushed at at */
int pushva(int at, int from, int n) {
  int i;
  for (i = 0; i < n; i++) stk[at + i] = stk[from + i];
  return at + (n > 0 ? n : 0);
}
/* move n results from stk[from] down to stk[base] */
int lret(int base, int from, int n) {
  int i;
  for (i = 0; i < n; i++) stk[base + i] = stk[from + i];
  return n;
}
/* t[start], t[start+1], ... = stk[from .. from+n) */
void tappend(long t, int from, int n, int start) {
  int i;
  for (i = 0; i < n; i++) tset(t, mkint(start + i), stk[from + i]);
}

/* numeric for: forprep checks the values and sets for_n, the number of
 * iterations after the first (-1 for a float loop); fornext steps */
long for_i; long for_n; long for_s; long for_l;
long forprep(long a, long b, long c) {
  long i; long l; long s; double fl;
  a = tonumber(a); b = tonumber(b); c = tonumber(c);
  if (!isnum(a)) lerror("'for' initial value must be a number");
  if (!isnum(b)) lerror("'for' limit must be a number");
  if (!isnum(c)) lerror("'for' step must be a number");
  if (isint(a) && isint(c)) {
    i = ival(a); s = ival(c);
    if (s == 0) lerror("'for' step is zero");
    if (isint(b)) l = ival(b);
    else {   /* a float limit: clip it to an integer */
      fl = dval(b);
      if (fl != fl) return FALSE;
      if (s > 0) { fl = __builtin_floor(fl); if (fl >= TWO63) l = 9223372036854775807L; else if (fl < -TWO63) return FALSE; else l = (long)fl; }
      else { fl = __builtin_ceil(fl); if (fl < -TWO63) l = -9223372036854775807L - 1; else if (fl >= TWO63) return FALSE; else l = (long)fl; }
    }
    if (s > 0 ? i > l : i < l) return FALSE;
    /* iterations after the first, computed without overflow */
    if (s > 0) for_n = (long)(((unsigned long)l - (unsigned long)i) / (unsigned long)s);
    else for_n = (long)(((unsigned long)i - (unsigned long)l) / ((unsigned long)(-(s + 1)) + 1));
    for_i = mkint(i); for_s = s; for_l = 0;
    return TRUE;
  }
  if (todbl(c) == 0) lerror("'for' step is zero");
  for_i = mkdbl(todbl(a)); for_l = mkdbl(todbl(b)); for_s = mkdbl(todbl(c)); for_n = -1;
  if (todbl(c) > 0 ? todbl(a) > todbl(b) : todbl(a) < todbl(b)) return FALSE;
  return TRUE;
}
long forstep(long i, long s) { return mkint(ival(i) + s); }
long fornextf(long i, long l, long s) {   /* 0 when done */
  double x = dval(i) + dval(s);
  if (dval(s) > 0 ? x > dval(l) : x < dval(l)) return 0;
  return mkdbl(x);
}

/* fmod, exactly (as C's): the remainder of x / y with the sign of x */
double fmod_(double x, double y) {
  long bx = __f64_bits(x); long by = __f64_bits(y); long M = 4503599627370495L;
  long ex = (bx >> 52) & 2047; long ey = (by >> 52) & 2047; long mx; long my;
  if (x != x || y != y || ex == 2047 || y == 0) return (x * y) / (x * y);
  if (ey == 2047) return x;
  mx = bx & M; my = by & M;
  if (ex) mx = mx | (M + 1); else ex = 1;
  if (ey) my = my | (M + 1); else ey = 1;
  if (ex < ey || (ex == ey && mx < my)) return x;
  while (ex > ey) { if (mx >= my) mx = mx - my; mx = mx << 1; ex--; }
  if (mx >= my) mx = mx - my;
  return mkdouble((unsigned long)mx, (int)ey - 1075, 0, bx < 0);
}

/* exp and log, accurate to about an ulp (not correctly rounded) */
double LN2HI; double LN2LO;
double llog(double x) {
  long b = __f64_bits(x); long k; double m; double s; double s2; double t; double r; int i;
  if (x != x || x < 0) return (double)0 / (double)0;
  if (x == 0) return -(double)1 / (double)0;
  if ((b >> 52) == 2047) return x;
  k = (b >> 52) - 1023;
  if (k == -1023) { x = x * (double)4503599627370496L; b = __f64_bits(x); k = (b >> 52) - 1023 - 52; }
  m = __f64_from_bits((b & 4503599627370495L) | 4607182418800017408L);   /* in [1, 2) */
  if (m > __f64_from_bits(4609047870845172685L)) { m = m / 2; k++; }
  s = (m - 1) / (m + 1); s2 = s * s;
  t = 0; i = 41;
  while (i >= 1) { t = t * s2 + (double)1 / i; i = i - 2; }
  r = 2 * s * t;
  return (double)k * LN2HI + ((double)k * LN2LO + r);
}
double lexp(double x) {
  double k; double r; double t; double term; int i;
  if (x != x) return x;
  if (x > 710) return (double)1 / (double)0;
  if (x < -746) return 0;
  k = __builtin_floor(x / (LN2HI + LN2LO) + (double)1 / 2);
  r = (x - k * LN2HI) - k * LN2LO;
  t = 1; term = 1;
  for (i = 1; i < 30; i++) { term = term * r / i; t = t + term; }
  return t * powi(2, (long)k);
}

/* -------------------------------------------------------------- library */

long arg(int base, int nargs, int i) { return i < nargs ? stk[base + i] : NIL; }
int ret1(int base, long v) { stk[base] = v; return 1; }

long tostr(long v) {
  long h;
  sbn = 0;
  if (v == NIL) return cstr("nil");
  if (v == TRUE) return cstr("true");
  if (v == FALSE) return cstr("false");
  if (isint(v)) { sbint(ival(v)); return sbstr(); }
  if (isdbl(v)) { sbnum(dval(v)); return sbstr(); }
  if (isstr(v)) return v;
  h = metafield(v, s_tostring);
  if (h != NIL) return call2(h, v, NIL, 1);
  sbputs(tname(v), slen(tname(v)));
  sbputs(": 0x", 4);
  sbhex((unsigned long)v + 1407374883553280UL, 0);   /* looks like an address */
  return sbstr();
}

void argerr(int i, char *fname, char *msg) {
  char *n = "#?";
  flush();
  eputs("lua: bad argument #"); sbn = 0; sbint(i + 1); sbput(0); eputs(sb);
  eputs(" to '"); eputs(fname); eputs("' ("); eputs(msg); eputs(")\n");
  proc_exit(1);
}
long checkint(int base, int nargs, int i, char *fname) {
  long v = tonumber(arg(base, nargs, i)); long r;
  if (isint(v)) return ival(v);
  if (isdbl(v)) {
    r = flt2int(dval(v));
    if (fok) return r;
    argerr(i, fname, "number has no integer representation");
  }
  argerr(i, fname, "number expected");
  return 0;
}
double checknum(int base, int nargs, int i, char *fname) {
  long v = tonumber(arg(base, nargs, i));
  if (!isnum(v)) argerr(i, fname, "number expected");
  return todbl(v);
}
long optint(int base, int nargs, int i, long d, char *fname) {
  if (arg(base, nargs, i) == NIL) return d;
  return checkint(base, nargs, i, fname);
}
long checkstr(int base, int nargs, int i, char *fname) {
  long v = arg(base, nargs, i);
  if (isnum(v)) return tostr(v);
  if (!isstr(v)) argerr(i, fname, "string expected");
  return v;
}
long checktab(int base, int nargs, int i, char *fname) {
  long v = arg(base, nargs, i);
  if (!istab(v)) argerr(i, fname, "table expected");
  return v;
}

int lib_print(int clo, int base, int nargs) {
  int i; long s;
  for (i = 0; i < nargs; i++) {
    if (i) outb("\t", 1);
    s = tostr(stk[base + i]);
    outb(sptr(s), sl(s));
  }
  outb("\n", 1);
  return 0;
}
int lib_type(int clo, int base, int nargs) {
  if (!nargs) argerr(0, "type", "value expected");
  return ret1(base, cstr(tname(stk[base])));
}
int lib_tostring(int clo, int base, int nargs) { return ret1(base, tostr(arg(base, nargs, 0))); }
int lib_tonumber(int clo, int base, int nargs) {
  long v = arg(base, nargs, 0); long b; long s; char *p; int n; int i; int d; long r = 0; int neg = 0;
  if (arg(base, nargs, 1) == NIL) return ret1(base, tonumber(v));
  b = checkint(base, nargs, 1, "tonumber");
  s = checkstr(base, nargs, 0, "tonumber"); p = sptr(s); n = sl(s); i = 0;
  while (i < n && p[i] == ' ') i++;
  if (i < n && p[i] == '-') { neg = 1; i++; }
  if (i == n) return ret1(base, NIL);
  while (i < n && p[i] != ' ') {
    d = p[i];
    if (d >= '0' && d <= '9') d = d - '0';
    else if (d >= 'a' && d <= 'z') d = d - 'a' + 10;
    else if (d >= 'A' && d <= 'Z') d = d - 'A' + 10;
    else return ret1(base, NIL);
    if (d >= b) return ret1(base, NIL);
    r = r * b + d; i++;
  }
  while (i < n && p[i] == ' ') i++;
  if (i != n) return ret1(base, NIL);
  return ret1(base, mkint(neg ? -r : r));
}
int lib_next(int clo, int base, int nargs) {
  return tnext(checktab(base, nargs, 0, "next"), arg(base, nargs, 1), base);
}
long nextfn;
int lib_pairs(int clo, int base, int nargs) {
  long t = checktab(base, nargs, 0, "pairs");
  stk[base] = nextfn; stk[base + 1] = t; stk[base + 2] = NIL;
  return 3;
}
int lib_ipairs_iter(int clo, int base, int nargs) {
  long i = ladd(stk[base + 1], mkint(1)); long v = lindex(stk[base], i);
  if (v == NIL) return ret1(base, NIL);
  stk[base] = i; stk[base + 1] = v;
  return 2;
}
long ipairsfn;
int lib_ipairs(int clo, int base, int nargs) {
  stk[base + 1] = arg(base, nargs, 0); stk[base] = ipairsfn; stk[base + 2] = mkint(0);
  return 3;
}
int lib_select(int clo, int base, int nargs) {
  long n = arg(base, nargs, 0);
  if (isstr(n) && sl(n) == 1 && sptr(n)[0] == '#') return ret1(base, mkint(nargs - 1));
  n = checkint(base, nargs, 0, "select");
  if (n < 0) n = nargs + n;
  else if (n == 0) argerr(0, "select", "index out of range");
  if (n < 1) argerr(0, "select", "index out of range");
  if (n >= nargs) return 0;
  return lret(base, base + (int)n, nargs - (int)n);
}
int lib_rawget(int clo, int base, int nargs) {
  return ret1(base, tget(checktab(base, nargs, 0, "rawget"), arg(base, nargs, 1)));
}
int lib_rawset(int clo, int base, int nargs) {
  tset(checktab(base, nargs, 0, "rawset"), arg(base, nargs, 1), arg(base, nargs, 2));
  return 1;
}
int lib_rawequal(int clo, int base, int nargs) {
  return ret1(base, rawequal(arg(base, nargs, 0), arg(base, nargs, 1)) ? TRUE : FALSE);
}
int lib_rawlen(int clo, int base, int nargs) {
  long v = arg(base, nargs, 0);
  if (istab(v)) return ret1(base, mkint(tlen(v)));
  return ret1(base, llen(v));
}
int lib_setmetatable(int clo, int base, int nargs) {
  long t = checktab(base, nargs, 0, "setmetatable");
  ((int *)(int)t)[8] = (int)arg(base, nargs, 1);
  return 1;
}
int lib_getmetatable(int clo, int base, int nargs) {
  long v = arg(base, nargs, 0);
  if (isstr(v)) return ret1(base, NIL);
  return ret1(base, metaof(v));
}
int lib_assert(int clo, int base, int nargs) {
  long m;
  if (!nargs || !truthy(stk[base])) {
    if (nargs > 1) { m = tostr(stk[base + 1]); lerror(sptr(m)); }
    lerror("assertion failed!");
  }
  return nargs;
}
int lib_error(int clo, int base, int nargs) {
  long m = tostr(arg(base, nargs, 0));
  lerror(sptr(m));
  return 0;
}
/* pcall cannot catch errors here (an error stops the program), but a
 * call that succeeds behaves as it should.  The one error it does catch
 * is the common probe for an optional module, pcall(require, name). */
long requirefn; long preload; long loaded;
int lib_pcall(int clo, int base, int nargs) {
  int n; long name;
  if (nargs >= 2 && stk[base] == requirefn && isstr(stk[base + 1])) {
    name = stk[base + 1];
    if (tget(loaded, name) == NIL && tget(preload, name) == NIL) {
      stk[base] = FALSE;
      sbn = 0;
      sbputs("module '", 8); sbputs(sptr(name), sl(name)); sbputs("' not found", 11);
      stk[base + 1] = sbstr();
      return 2;
    }
  }
  n = lcall(arg(base, nargs, 0), base + 1, nargs - 1);
  stk[base] = TRUE;
  return n + 1;
}
int lib_load(int clo, int base, int nargs) {
  lerror("load: only constant strings can be loaded (they are compiled ahead of time)");
  return 0;
}
int lib_unpack(int clo, int base, int nargs) {
  long t = arg(base, nargs, 0); long i = optint(base, nargs, 1, 1, "unpack");
  long j = arg(base, nargs, 2) == NIL ? ival(llen(t)) : checkint(base, nargs, 2, "unpack");
  int n = 0;
  if (i <= j && j - i >= STKSZ - 1000 - base) lerror("too many results to unpack");
  while (i <= j) { stk[base + n] = lindex(t, mkint(i)); n++; i++; }
  return n;
}

/* string library */
long strindex(long i, long n) {   /* Lua's index into a string of length n */
  if (i < 0) i = n + i + 1;
  return i;
}
int lib_len(int clo, int base, int nargs) { return ret1(base, mkint(sl(checkstr(base, nargs, 0, "len")))); }
int lib_sub(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "sub"); long n = sl(s);
  long i = strindex(optint(base, nargs, 1, 1, "sub"), n);
  long j = strindex(optint(base, nargs, 2, -1, "sub"), n);
  if (i < 1) i = 1;
  if (j > n) j = n;
  if (i > j) return ret1(base, cstr(""));
  return ret1(base, lstr(sptr(s) + (int)i - 1, (int)(j - i + 1)));
}
int lib_upper(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "upper"); int i; int c;
  sbn = 0;
  for (i = 0; i < sl(s); i++) { c = sptr(s)[i]; sbput(c >= 'a' && c <= 'z' ? c - 32 : c); }
  return ret1(base, sbstr());
}
int lib_lower(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "lower"); int i; int c;
  sbn = 0;
  for (i = 0; i < sl(s); i++) { c = sptr(s)[i]; sbput(c >= 'A' && c <= 'Z' ? c + 32 : c); }
  return ret1(base, sbstr());
}
int lib_rep(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "rep"); long n = checkint(base, nargs, 1, "rep");
  long sep = arg(base, nargs, 2) == NIL ? NIL : checkstr(base, nargs, 2, "rep");
  sbn = 0;
  while (n > 0) {
    sbputs(sptr(s), sl(s));
    if (n > 1 && sep != NIL) sbputs(sptr(sep), sl(sep));
    n--;
  }
  return ret1(base, sbstr());
}
int lib_reverse(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "reverse"); int i;
  sbn = 0;
  for (i = sl(s) - 1; i >= 0; i--) sbput(sptr(s)[i]);
  return ret1(base, sbstr());
}
int lib_byte(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "byte"); long n = sl(s);
  long i = strindex(optint(base, nargs, 1, 1, "byte"), n);
  long j = arg(base, nargs, 2) == NIL ? i : strindex(checkint(base, nargs, 2, "byte"), n);
  int k = 0; char *p = sptr(s);
  if (i < 1) i = 1;
  if (j > n) j = n;
  while (i <= j) { stk[base + k] = mkint(p[(int)i - 1] & 255); k++; i++; }
  return k;
}
int lib_char(int clo, int base, int nargs) {
  int i;
  sbn = 0;
  for (i = 0; i < nargs; i++) sbput((int)checkint(base, nargs, i, "char"));
  return ret1(base, sbstr());
}
int lib_find(int clo, int base, int nargs) {
  long s = checkstr(base, nargs, 0, "find"); long p = checkstr(base, nargs, 1, "find");
  long init = strindex(optint(base, nargs, 2, 1, "find"), sl(s));
  int i; int j; char *ps = sptr(p);
  if (!truthy(arg(base, nargs, 3)))
    for (j = 0; j < sl(p); j++)
      if (ps[j] == '^' || ps[j] == '$' || ps[j] == '*' || ps[j] == '+' || ps[j] == '?' ||
          ps[j] == '.' || ps[j] == '[' || ps[j] == '%' || ps[j] == '(' || ps[j] == '-')
        lerror("string patterns are not supported (only plain find)");
  if (init < 1) init = 1;
  for (i = (int)init - 1; i + sl(p) <= sl(s); i++) {
    if (memeq(sptr(s) + i, ps, sl(p))) {
      stk[base] = mkint(i + 1); stk[base + 1] = mkint(i + sl(p));
      return 2;
    }
  }
  return ret1(base, NIL);
}
int lib_format(int clo, int base, int nargs) {
  long f = checkstr(base, nargs, 0, "format"); char *p = sptr(f); int n = sl(f);
  int i = 0; int a = 1; int left; int zero; int plus; int space; int alt; int width; int prec; int c;
  long v; int start; int len; int pad; int j; long sv; char *tmp; int tn; unsigned long u;
  sbn = 0;
  while (i < n) {
    if (p[i] != '%') { sbput(p[i]); i++; continue; }
    i++;
    if (p[i] == '%') { sbput('%'); i++; continue; }
    left = 0; zero = 0; plus = 0; space = 0; alt = 0; width = 0; prec = -1;
    while (p[i] == '-' || p[i] == '0' || p[i] == '+' || p[i] == ' ' || p[i] == '#') {
      if (p[i] == '-') left = 1;
      if (p[i] == '0') zero = 1;
      if (p[i] == '+') plus = 1;
      if (p[i] == ' ') space = 1;
      if (p[i] == '#') alt = 1;
      i++;
    }
    while (p[i] >= '0' && p[i] <= '9') { width = width * 10 + p[i] - '0'; i++; }
    if (p[i] == '.') { i++; prec = 0; while (p[i] >= '0' && p[i] <= '9') { prec = prec * 10 + p[i] - '0'; i++; } }
    c = p[i]; i++;
    start = sbn;
    if (a >= nargs && c != '%') argerr(a, "format", "no value");
    if (c == 'd' || c == 'i') {
      v = checkint(base, nargs, a++, "format");
      if (v < 0) { sbput('-'); u = -(unsigned long)v; }
      else { if (plus) sbput('+'); else if (space) sbput(' '); u = (unsigned long)v; }
      len = 1; sv = (long)(u / 10);
      while (sv) { sv = sv / 10; len++; }
      while (prec > len) { sbput('0'); prec--; }
      if (u >= 9223372036854775808UL) sbputs("9223372036854775808", 19); else sbint((long)u);
    } else if (c == 'x' || c == 'X' || c == 'o') {
      v = checkint(base, nargs, a++, "format");
      if (c == 'o') {
        u = (unsigned long)v; len = sbn;
        do { sbput('0' + (int)(u & 7)); u = u >> 3; } while (u);
        for (j = 0; j < (sbn - len) / 2; j++) { pad = sb[len + j]; sb[len + j] = sb[sbn - 1 - j]; sb[sbn - 1 - j] = pad; }
      } else sbhex((unsigned long)v, c == 'X');
    } else if (c == 'c') {
      sbput((int)checkint(base, nargs, a++, "format"));
    } else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
      double d = checknum(base, nargs, a++, "format");
      if (plus && __f64_bits(d) >= 0) sbput('+');
      else if (space && __f64_bits(d) >= 0) sbput(' ');
      sbneed(1400);
      sbn = sbn + fmtdbl(d, c, prec, alt, sb + sbn);
    } else if (c == 's') {
      sv = tostr(arg(base, nargs, a++));
      tmp = sptr(sv); tn = sl(sv);
      if (prec >= 0 && prec < tn) tn = prec;
      sbn = start;   /* tostr used the buffer */
      sbputs(tmp, tn);
    } else if (c == 'q') {
      sv = arg(base, nargs, a++);
      if (isstr(sv)) {
        sbput('"');
        for (len = 0; len < sl(sv); len++) {
          c = sptr(sv)[len];
          if (c == '"' || c == '\\') { sbput('\\'); sbput(c); }
          else if (c == '\n') { sbput('\\'); sbput('n'); }
          else if (c == '\r') { sbput('\\'); sbput('r'); }
          else if (c == 0) { sbput('\\'); sbput('0'); }
          else sbput(c);
        }
        sbput('"');
      } else { sv = tostr(sv); sbn = start; sbputs(sptr(sv), sl(sv)); }
    } else lerror("invalid conversion in format");
    len = sbn - start;
    if (len < width) {   /* pad */
      pad = width - len;
      sbneed(pad);
      if (left) { while (pad--) sbput(' '); }
      else {
        for (j = len - 1; j >= 0; j--) sb[start + pad + j] = sb[start + j];
        for (j = 0; j < pad; j++) sb[start + j] = zero && c != 's' ? '0' : ' ';
        if (zero && c != 's' && (sb[start + pad] == '-' || sb[start + pad] == '+')) {
          sb[start] = sb[start + pad]; sb[start + pad] = '0';
        }
        sbn = sbn + pad;
      }
    }
  }
  return ret1(base, sbstr());
}

/* table library */
int lib_insert(int clo, int base, int nargs) {
  long t = checktab(base, nargs, 0, "insert"); long n = tlen(t); long pos; long i;
  if (nargs == 2) { tset(t, mkint(n + 1), stk[base + 1]); return 0; }
  if (nargs != 3) lerror("wrong number of arguments to 'insert'");
  pos = checkint(base, nargs, 1, "insert");
  if (pos < 1 || pos > n + 1) argerr(1, "insert", "position out of bounds");
  for (i = n; i >= pos; i--) tset(t, mkint(i + 1), tget(t, mkint(i)));
  tset(t, mkint(pos), stk[base + 2]);
  return 0;
}
int lib_remove(int clo, int base, int nargs) {
  long t = checktab(base, nargs, 0, "remove"); long n = tlen(t); long pos = n; long v; long i;
  if (nargs > 1) pos = checkint(base, nargs, 1, "remove");
  if (n == 0 && nargs < 2) return ret1(base, NIL);
  v = tget(t, mkint(pos));
  for (i = pos; i < n; i++) tset(t, mkint(i), tget(t, mkint(i + 1)));
  if (pos <= n) tset(t, mkint(n), NIL);
  return ret1(base, v);
}
int lib_concat(int clo, int base, int nargs) {
  long t = checktab(base, nargs, 0, "concat");
  long sep = arg(base, nargs, 1) == NIL ? NIL : checkstr(base, nargs, 1, "concat");
  long i = optint(base, nargs, 2, 1, "concat");
  long j = arg(base, nargs, 3) == NIL ? tlen(t) : checkint(base, nargs, 3, "concat");
  long v;
  sbn = 0;
  for (; i <= j; i++) {
    v = tget(t, mkint(i));
    if (!isnum(v) && !isstr(v)) lerror("invalid value (at index) in table for 'concat'");
    sbval(v);
    if (i < j && sep != NIL) sbputs(sptr(sep), sl(sep));
  }
  return ret1(base, sbstr());
}
long sortcmp;
int lessthan(long a, long b) {
  if (sortcmp == NIL) return llt(a, b) == TRUE;
  return truthy(call2(sortcmp, a, b, 2)) != 0;
}
void msort(long *a, long *tmp, int n) {
  int h; int i; int j; int k;
  if (n < 2) return;
  h = n / 2;
  msort(a, tmp, h); msort(a + h, tmp, n - h);
  i = 0; j = h; k = 0;
  while (i < h && j < n) {
    if (lessthan(a[j], a[i])) tmp[k++] = a[j++]; else tmp[k++] = a[i++];
  }
  while (i < h) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  for (i = 0; i < n; i++) a[i] = tmp[i];
}
int lib_sort(int clo, int base, int nargs) {
  long t = checktab(base, nargs, 0, "sort"); int n = tlen(t); long *a; long *tmp; int i; long old = sortcmp;
  int ot = top;
  sortcmp = arg(base, nargs, 1);
  top = base + nargs;
  a = (long *)malloc(n * 8 + 8); tmp = (long *)malloc(n * 8 + 8);
  for (i = 0; i < n; i++) a[i] = tget(t, mkint(i + 1));
  msort(a, tmp, n);
  for (i = 0; i < n; i++) tset(t, mkint(i + 1), a[i]);
  sortcmp = old; top = ot;
  return 0;
}

/* math library */
long numarg(int base, int nargs, int i, char *fname) {
  long v = tonumber(arg(base, nargs, i));
  if (!isnum(v)) argerr(i, fname, "number expected");
  return v;
}
int lib_abs(int clo, int base, int nargs) {
  long v = numarg(base, nargs, 0, "abs");
  if (isint(v)) return ret1(base, mkint(ival(v) < 0 ? -ival(v) : ival(v)));
  return ret1(base, mkdbl(__builtin_fabs(dval(v))));
}
int lib_max(int clo, int base, int nargs) {
  long m = numarg(base, nargs, 0, "max"); int i; long v;
  for (i = 1; i < nargs; i++) { v = numarg(base, nargs, i, "max"); if (ltnum(m, v)) m = v; }
  return ret1(base, m);
}
int lib_min(int clo, int base, int nargs) {
  long m = numarg(base, nargs, 0, "min"); int i; long v;
  for (i = 1; i < nargs; i++) { v = numarg(base, nargs, i, "min"); if (ltnum(v, m)) m = v; }
  return ret1(base, m);
}
long float2num(double d) { long i = flt2int(d); if (fok) return mkint(i); return mkdbl(d); }
int lib_floor(int clo, int base, int nargs) {
  long v = numarg(base, nargs, 0, "floor");
  if (isint(v)) return ret1(base, v);
  return ret1(base, float2num(__builtin_floor(dval(v))));
}
int lib_ceil(int clo, int base, int nargs) {
  long v = numarg(base, nargs, 0, "ceil");
  if (isint(v)) return ret1(base, v);
  return ret1(base, float2num(__builtin_ceil(dval(v))));
}
int lib_sqrt(int clo, int base, int nargs) { return ret1(base, mkdbl(__builtin_sqrt(checknum(base, nargs, 0, "sqrt")))); }
int lib_exp(int clo, int base, int nargs) { return ret1(base, mkdbl(lexp(checknum(base, nargs, 0, "exp")))); }
int lib_log(int clo, int base, int nargs) {
  double x = checknum(base, nargs, 0, "log"); double b;
  if (arg(base, nargs, 1) == NIL) return ret1(base, mkdbl(llog(x)));
  b = checknum(base, nargs, 1, "log");
  if (b == 2) return ret1(base, mkdbl(llog(x) / llog(2)));
  if (b == 10) return ret1(base, mkdbl(llog(x) / llog(10)));
  return ret1(base, mkdbl(llog(x) / llog(b)));
}
int lib_tointeger(int clo, int base, int nargs) {
  long v = arg(base, nargs, 0); long i;
  if (isint(v)) return ret1(base, v);
  if (isdbl(v)) { i = flt2int(dval(v)); if (fok) return ret1(base, mkint(i)); }
  return ret1(base, NIL);
}
int lib_mathtype(int clo, int base, int nargs) {
  long v = arg(base, nargs, 0);
  if (isint(v)) return ret1(base, cstr("integer"));
  if (isdbl(v)) return ret1(base, cstr("float"));
  return ret1(base, NIL);
}
int lib_fmod(int clo, int base, int nargs) {
  long a = numarg(base, nargs, 0, "fmod"); long b = numarg(base, nargs, 1, "fmod");
  if (isint(a) && isint(b)) {
    if (ival(b) == 0) argerr(1, "fmod", "zero");
    if (ival(b) == -1) return ret1(base, mkint(0));
    return ret1(base, mkint(ival(a) % ival(b)));
  }
  return ret1(base, mkdbl(fmod_(todbl(a), todbl(b))));
}
int lib_modf(int clo, int base, int nargs) {
  double d = checknum(base, nargs, 0, "modf"); double ip;
  ip = d >= 0 ? __builtin_floor(d) : __builtin_ceil(d);
  stk[base] = float2num(ip);
  if (d == ip) stk[base] = mkdbl(ip);
  stk[base + 1] = mkdbl(d == ip ? (double)0 : d - ip);
  return 2;
}
int lib_ult(int clo, int base, int nargs) {
  return ret1(base, (unsigned long)checkint(base, nargs, 0, "ult") < (unsigned long)checkint(base, nargs, 1, "ult") ? TRUE : FALSE);
}

/* io and os */
char ibuf[65536]; int ipos; int ilen; int ieof;
int igetc() {
  if (ipos == ilen) {
    if (ieof) return -1;
    ilen = sys_read(ibuf, 65536); ipos = 0;
    if (ilen <= 0) { ilen = 0; ieof = 1; return -1; }
  }
  return ibuf[ipos++] & 255;
}
int ipeek() { int c = igetc(); if (c >= 0) ipos--; return c; }
int lib_write(int clo, int base, int nargs) {
  int i; long v;
  for (i = 0; i < nargs; i++) {
    v = stk[base + i];
    if (isnum(v)) { sbn = 0; sbval(v); outb(sb, sbn); }
    else if (isstr(v)) outb(sptr(v), sl(v));
    else argerr(i, "write", "string expected");
  }
  return 0;
}
int lib_read(int clo, int base, int nargs) {
  long f = arg(base, nargs, 0); char *p; int c; long v;
  if (f == NIL) f = cstr("l");
  if (isnum(f)) lerror("io.read(n) is not supported");
  p = sptr(f);
  if (p[0] == '*') p++;
  sbn = 0;
  if (p[0] == 'l' || p[0] == 'L') {
    c = igetc();
    if (c < 0) return ret1(base, NIL);
    while (c >= 0 && c != '\n') { sbput(c); c = igetc(); }
    if (c == '\n' && p[0] == 'L') sbput(c);
    return ret1(base, sbstr());
  }
  if (p[0] == 'a') {
    while ((c = igetc()) >= 0) sbput(c);
    return ret1(base, sbstr());
  }
  if (p[0] == 'n') {
    while ((c = ipeek()) == ' ' || c == '\n' || c == '\t' || c == '\r') igetc();
    while ((c = ipeek()) >= 0 && ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
           c == 'e' || c == 'E' || c == 'x' || c == 'X' || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      sbput(c); igetc();
    }
    v = str2num(sbstr());
    return ret1(base, v);
  }
  argerr(0, "read", "invalid format");
  return 0;
}
int lib_exit(int clo, int base, int nargs) {
  long v = arg(base, nargs, 0);
  flush();
  if (v == FALSE) proc_exit(1);
  if (isint(v)) proc_exit((int)ival(v));
  proc_exit(0);
  return 0;
}

/* require: modules are bundled ahead of time into package.preload */
int lib_require(int clo, int base, int nargs) {
  long name = checkstr(base, nargs, 0, "require"); long v = tget(loaded, name); long f; int n;
  if (v != NIL) return ret1(base, v);
  f = tget(preload, name);
  if (f == NIL) lerror3("module '", sptr(name), "' not found (bundle it: lua/bundle.sh)");
  top = base + nargs;
  n = lcall(f, base, 1);
  v = n && stk[base] != NIL ? stk[base] : TRUE;
  if (tget(loaded, name) == NIL) tset(loaded, name, v);
  return ret1(base, tget(loaded, name));
}

void reg(long t, char *name, int fn) { tset(t, cstr(name), mkclo(fn, 0)); }
long lib(char *name) { long t = newtable(); tset(globals, cstr(name), t); return t; }

void lua_init() {
  long t;
  TWO63 = (double)(1L << 62) * 2;
  LN2HI = __f64_from_bits(4604418534311723008L);
  LN2LO = __f64_from_bits(4461442080421002358L);
  s_index = cstr("__index"); s_newindex = cstr("__newindex"); s_call = cstr("__call");
  s_tostring = cstr("__tostring"); s_len = cstr("__len"); s_eq = cstr("__eq");
  s_add = cstr("__add"); s_sub = cstr("__sub"); s_mul = cstr("__mul"); s_div = cstr("__div");
  s_mod = cstr("__mod"); s_unm = cstr("__unm"); s_concat = cstr("__concat");
  s_lt = cstr("__lt"); s_le = cstr("__le"); s_idiv = cstr("__idiv");
  globals = newtable();
  tset(globals, cstr("_G"), globals);
  tset(globals, cstr("_VERSION"), cstr("Lua 5.4"));
  reg(globals, "print", lib_print); reg(globals, "type", lib_type);
  reg(globals, "tostring", lib_tostring); reg(globals, "tonumber", lib_tonumber);
  reg(globals, "next", lib_next); reg(globals, "pairs", lib_pairs); reg(globals, "ipairs", lib_ipairs);
  reg(globals, "select", lib_select); reg(globals, "rawget", lib_rawget); reg(globals, "rawset", lib_rawset);
  reg(globals, "rawequal", lib_rawequal); reg(globals, "rawlen", lib_rawlen);
  reg(globals, "setmetatable", lib_setmetatable); reg(globals, "getmetatable", lib_getmetatable);
  reg(globals, "assert", lib_assert); reg(globals, "error", lib_error); reg(globals, "pcall", lib_pcall);
  reg(globals, "load", lib_load);
  nextfn = tget(globals, cstr("next"));
  ipairsfn = mkclo(lib_ipairs_iter, 0);
  strlib = t = lib("string");
  reg(t, "len", lib_len); reg(t, "sub", lib_sub); reg(t, "upper", lib_upper); reg(t, "lower", lib_lower);
  reg(t, "rep", lib_rep); reg(t, "reverse", lib_reverse); reg(t, "byte", lib_byte); reg(t, "char", lib_char);
  reg(t, "find", lib_find); reg(t, "format", lib_format);
  t = lib("table");
  reg(t, "insert", lib_insert); reg(t, "remove", lib_remove); reg(t, "concat", lib_concat);
  reg(t, "sort", lib_sort); reg(t, "unpack", lib_unpack);
  t = lib("math");
  reg(t, "abs", lib_abs); reg(t, "max", lib_max); reg(t, "min", lib_min); reg(t, "floor", lib_floor);
  reg(t, "ceil", lib_ceil); reg(t, "sqrt", lib_sqrt); reg(t, "exp", lib_exp); reg(t, "log", lib_log);
  reg(t, "tointeger", lib_tointeger); reg(t, "type", lib_mathtype); reg(t, "fmod", lib_fmod);
  reg(t, "modf", lib_modf); reg(t, "ult", lib_ult);
  tset(t, cstr("maxinteger"), mkint(9223372036854775807L));
  tset(t, cstr("mininteger"), mkint(-9223372036854775807L - 1));
  tset(t, cstr("pi"), mkdbl(__f64_from_bits(4614256656552045848L)));
  tset(t, cstr("huge"), mkdbl((double)1 / (double)0));
  t = lib("io");
  reg(t, "write", lib_write); reg(t, "read", lib_read);
  t = lib("package");
  preload = newtable(); loaded = newtable();
  tset(t, cstr("preload"), preload); tset(t, cstr("loaded"), loaded);
  reg(globals, "require", lib_require);
  requirefn = tget(globals, cstr("require"));
  t = lib("os");
  reg(t, "exit", lib_exit);
}

int main() {
  lua_init();
  lua_consts();
  top = 0;
  lcall(mkclo(F0, 0), 0, 0);
  flush();
  proc_exit(0);
  return 0;
}
