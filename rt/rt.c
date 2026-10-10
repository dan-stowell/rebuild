/* rt.c -- the core runtime shared by the dynamically typed languages.
 * Needs libc.c and num.c in front of it.
 *
 * A value is a long (64 bits):
 *   top 16 bits 0xffff:  an integer in the low 48 bits
 *   top 16 bits 0:       bits 32..47 0: a constant below 16 (NIL, FALSE,
 *                        TRUE, NULL, EOFV), else a pointer to an object,
 *                        whose first word says what it is;
 *                        bits 32..47 nonzero: other immediates (characters)
 *   anything else:       a double, plus 2^48
 * Integers outside 48 bits are boxed and interned, so that equal integers
 * are always the same bits.  There are no bignums: integer arithmetic that
 * overflows 64 bits is an error.  Memory is never freed.
 *
 * Calling convention: a function is int f(int clo, int nargs), with its
 * arguments in stk[0 .. nargs); it leaves its results in stk[0 ..] and
 * returns how many there are.  Callers keep their own values in locals,
 * so stk is only a place to pass arguments and results.
 */

enum { NIL = 0, FALSE = 2, TRUE = 4, NULL = 6, EOFV = 8 };
enum { OSTR = 1, OSYM, OPAIR, OVEC, OFUN, OINT, OREC, OPROM, OCONT };
enum { STKSZ = 1000000 };

long stk[STKSZ];

void proc_exit(int code);   /* WASI */

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
void outs(char *s) { outb(s, slen(s)); }
void eputs(char *s) { sys_write(2, s, slen(s)); }
void die(char *a, char *b) {
  flush();
  eputs(a); eputs(b); eputs("\n");
  proc_exit(1);
}

/* allocation: a bump pointer over malloc'd chunks */
char *hp; char *hend;
char *alloc(int n) {
  char *p;
  n = (n + 7) & ~7;
  if (hp + n > hend) {
    if (n > 65536) { p = malloc(n); if (!p) die("out of memory", ""); return p; }
    hp = malloc(1048576);
    if (!hp) die("out of memory", "");
    hend = hp + 1048576;
  }
  p = hp; hp = hp + n;
  return p;
}

/* ---------------------------------------------------------------- values */

long INTTAG = -281474976710656L;   /* 0xffff000000000000 */
long DOFF = 281474976710656L;      /* 2^48 */
long MASK48 = 281474976710655L;
long CHARTAG = 4294967296L;        /* 1 << 32 */

int isobj(long v) { return (v >> 32) == 0 && v >= 16; }
int otype(long v) { if ((v >> 32) == 0 && v >= 16) return *(int *)(int)v; return 0; }
int *op(long v) { return (int *)(int)v; }
int isfix(long v) { return (v >> 48) == -1; }
int isdbl(long v) { long t = v >> 48; return t != 0 && t != -1; }
int isint(long v) { return (v >> 48) == -1 || otype(v) == OINT; }
int isnum(long v) { return isint(v) || isdbl(v); }
int ischar(long v) { return (v >> 32) == 1; }
long mkchar(int c) { return CHARTAG | (c & 255); }
int charval(long v) { return (int)v; }

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
    o = op(p);
    if (*(long *)(o + 2) == n) return p;
    p = *(long *)(o + 4);
  }
  o = (int *)alloc(24);
  o[0] = OINT;
  *(long *)(o + 2) = n;
  *(long *)(o + 4) = ints[h];
  ints[h] = (long)(int)o;
  return ints[h];
}
long ival(long v) {
  if (isfix(v)) return (v << 16) >> 16;
  return *(long *)(op(v) + 2);
}

/* ------------------------------------------------- strings and symbols */

/* string: [OSTR, length, 0, 0] then the bytes, NUL-terminated */
long mkstr(char *p, int n) {
  int *o = (int *)alloc(17 + n);
  o[0] = OSTR; o[1] = n; o[2] = 0; o[3] = 0;
  memcopy((char *)(o + 4), p, n);
  ((char *)(o + 4))[n] = 0;
  return (int)o;
}
long cstr(char *s) { return mkstr(s, slen(s)); }
char *sptr(long v) { return (char *)(int)v + 16; }
int sl(long v) { return op(v)[1]; }

/* symbol: [OSYM, length, hash, next in intern chain] then the bytes */
int symtab[16384];
long intern(char *p, int n) {
  int h = n; int i = 0; int s; int *o;
  while (i < n) { h = h * 31 + (p[i] & 255); i++; }
  s = symtab[h & 16383];
  while (s) {
    o = (int *)s;
    if (o[1] == n && o[2] == h && memeq((char *)s + 16, p, n)) return s;
    s = o[3];
  }
  o = (int *)alloc(17 + n);
  o[0] = OSYM; o[1] = n; o[2] = h; o[3] = symtab[h & 16383];
  memcopy((char *)(o + 4), p, n);
  ((char *)(o + 4))[n] = 0;
  symtab[h & 16383] = (int)o;
  return (int)o;
}
long sym(char *s) { return intern(s, slen(s)); }

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
void sbcstr(char *s) { sbputs(s, slen(s)); }
void sbint(long x) {
  int start; int i; int j; int c; unsigned long u;
  if (x < 0) { sbput('-'); u = -(unsigned long)x; } else u = (unsigned long)x;
  start = sbn;
  do { sbput('0' + (int)(u % 10)); u = u / 10; } while (u);
  i = start; j = sbn - 1;
  while (i < j) { c = sb[i]; sb[i] = sb[j]; sb[j] = c; i++; j--; }
}
void sbintradix(long x, int radix) {
  int start; int i; int j; int c; unsigned long u;
  if (x < 0) { sbput('-'); u = -(unsigned long)x; } else u = (unsigned long)x;
  start = sbn;
  do { c = (int)(u % radix); sbput(c < 10 ? '0' + c : 'a' + c - 10); u = u / radix; } while (u);
  i = start; j = sbn - 1;
  while (i < j) { c = sb[i]; sb[i] = sb[j]; sb[j] = c; i++; j--; }
}
char fbuf[1400];
void sbdbl(double d, int conv, int prec) { sbputs(fbuf, fmtdbl(d, conv, prec, 0, fbuf)); }
/* the fewest of 15, 16 or 17 significant digits that read back the same */
void sbdblshort(double d) {
  int p = 15; int n;
  while (p < 17) {
    n = fmtdbl(d, 'g', p, 0, fbuf);
    if (str2dbl(fbuf, n) == d) break;
    p++;
  }
  sbdbl(d, 'g', p);
}
long sbstr() { long s = mkstr(sb, sbn); sbn = 0; return s; }

/* ---------------------------------------------------- pairs and vectors */

/* pair: [OPAIR, 0, car, cdr] */
long cons(long a, long d) {
  int *o = (int *)alloc(24);
  o[0] = OPAIR;
  *(long *)(o + 2) = a; *(long *)(o + 4) = d;
  return (int)o;
}
int ispair(long v) { return otype(v) == OPAIR; }
long car_(long p) { return *(long *)((int)p + 8); }
long cdr_(long p) { return *(long *)((int)p + 16); }

/* vector: [OVEC, length] then the elements */
long mkvec(int n, long fill) {
  int *o = (int *)alloc(8 + 8 * n); int i;
  o[0] = OVEC; o[1] = n;
  for (i = 0; i < n; i++) *(long *)(o + 2 + 2 * i) = fill;
  return (int)o;
}
long *velts(long v) { return (long *)((int)v + 8); }
int vlen(long v) { return op(v)[1]; }

/* ------------------------------------------------- closures and calling */

/* closure: [OFUN, function index, number of free values, 0] then those */
long mkclo(int fn, int nfree) {
  int *o = (int *)alloc(16 + 8 * nfree);
  o[0] = OFUN; o[1] = fn; o[2] = nfree; o[3] = 0;
  return (int)o;
}
long *cfree(long c) { return (long *)((int)c + 16); }
int isproc(long v) { int t = otype(v); return t == OFUN || t == OCONT; }

/* cells hold variables that are captured and assigned */
long newcell(long v) { long *c = (long *)alloc(8); *c = v; return (int)c; }

/* Non-local exits (escape continuations, exceptions) unwind by returning:
 * unw is set, and every caller passes it on.  stk[0] holds the payload. */
int unw;

/* --------------------------------------------------------------- numbers */

void numerr(char *what, long v);   /* defined by each language */
void overflow() { die("integer overflow (no bignums)", ""); }

double TWO63;   /* 2^63: set by rt_init */
int fok;        /* did flt2int succeed? */
/* d as an integer, if it has an exact integer value */
long flt2int(double d) {
  fok = 0;
  if (d != d || d < -TWO63 || d >= TWO63) return 0;
  if (__builtin_floor(d) != d) return 0;
  fok = 1;
  return (long)d;
}
double todbl(long v) { if (isdbl(v)) return dval(v); return (double)ival(v); }
double numd(long v, char *what) {
  if (isdbl(v)) return dval(v);
  if (isint(v)) return (double)ival(v);
  numerr(what, v);
  return 0;
}
long numi(long v, char *what) { if (!isint(v)) numerr(what, v); return ival(v); }

long addi(long x, long y) {
  long r = (long)((unsigned long)x + (unsigned long)y);
  if (((x ^ r) & (y ^ r)) < 0) overflow();
  return r;
}
long subi(long x, long y) {
  long r = (long)((unsigned long)x - (unsigned long)y);
  if (((x ^ y) & (x ^ r)) < 0) overflow();
  return r;
}
long muli(long x, long y) {
  long r;
  if (((x << 32) >> 32) == x && ((y << 32) >> 32) == y) return x * y;
  r = (long)((unsigned long)x * (unsigned long)y);
  if (x == -1) { if (y == -9223372036854775807L - 1) overflow(); }
  else if (x != 0 && r / x != y) overflow();
  return r;
}
long n_add(long a, long b) {
  if (isfix(a) && isfix(b)) return mkint(((a << 16) >> 16) + ((b << 16) >> 16));
  if (isint(a) && isint(b)) return mkint(addi(ival(a), ival(b)));
  return mkdbl(numd(a, "+") + numd(b, "+"));
}
long n_sub(long a, long b) {
  if (isfix(a) && isfix(b)) return mkint(((a << 16) >> 16) - ((b << 16) >> 16));
  if (isint(a) && isint(b)) return mkint(subi(ival(a), ival(b)));
  return mkdbl(numd(a, "-") - numd(b, "-"));
}
long n_mul(long a, long b) {
  if (isint(a) && isint(b)) return mkint(muli(ival(a), ival(b)));
  return mkdbl(numd(a, "*") * numd(b, "*"));
}
long n_neg(long a) {
  if (isfix(a)) return mkint(-((a << 16) >> 16));
  if (isint(a)) return mkint(subi(0, ival(a)));
  return mkdbl(-numd(a, "-"));
}
/* truncating and flooring integer division and remainder */
long divzero() { die("division by zero", ""); return 0; }
long quoi(long x, long y) {
  if (y == 0) divzero();
  if (y == -1) return subi(0, x);
  return x / y;
}
long remi(long x, long y) {
  if (y == 0) divzero();
  if (y == -1) return 0;
  return x % y;
}
long modi(long x, long y) {
  long r = remi(x, y);
  if (r != 0 && (r < 0) != (y < 0)) r = r + y;
  return r;
}
long fdivi(long x, long y) {
  long q = quoi(x, y);
  if (x % y != 0 && (x < 0) != (y < 0)) q--;
  return q;
}

/* comparisons, exact even between integers and floats */
int n_lt(long a, long b) {
  long i; double f;
  if (isfix(a) && isfix(b)) return (a << 16) < (b << 16);
  if (isint(a) && isint(b)) return ival(a) < ival(b);
  if (isdbl(a) && isdbl(b)) return dval(a) < dval(b);
  if (isint(a)) {   /* i < f */
    if (!isdbl(b)) numerr("<", b);
    i = ival(a); f = dval(b);
    if (f != f) return 0;
    if (f >= TWO63) return 1;
    if (f < -TWO63) return 0;
    return i < (long)__builtin_ceil(f);
  }
  if (!isdbl(a)) numerr("<", a);
  if (!isint(b)) numerr("<", b);
  f = dval(a); i = ival(b);   /* f < i */
  if (f != f) return 0;
  if (f >= TWO63) return 0;
  if (f < -TWO63) return 1;
  return (long)__builtin_floor(f) < i;
}
int n_eq(long a, long b) {
  long i;
  if (isint(a) && isint(b)) return a == b;
  if (isdbl(a) && isdbl(b)) return dval(a) == dval(b);
  if (isdbl(a) && isint(b)) { i = flt2int(dval(a)); return fok && i == ival(b); }
  if (isint(a) && isdbl(b)) { i = flt2int(dval(b)); return fok && i == ival(a); }
  if (!isnum(a)) numerr("=", a);
  numerr("=", b);
  return 0;
}
int n_le(long a, long b) {
  if (isfix(a) && isfix(b)) return (a << 16) <= (b << 16);
  return n_lt(a, b) || n_eq(a, b);
}

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

double powi(double x, long n) {
  double r = 1; int neg = n < 0;
  if (neg) n = -n;
  while (n) { if (n & 1) r = r * x; x = x * x; n = n >> 1; }
  return neg ? 1 / r : r;
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

double pow_(double x, double y) {
  long n = flt2int(y);
  if (fok && n >= -1024 && n <= 1024) return powi(x, n);
  if (y + y == 1) return __builtin_sqrt(x);
  if (x < 0) return (double)0 / (double)0;
  return lexp(y * llog(x));
}

void rt_init() {
  TWO63 = (double)4611686018427387904L * 2;
  LN2HI = __f64_from_bits(4604418534311723008L);
  LN2LO = __f64_from_bits(4461442080421002358L);
}
