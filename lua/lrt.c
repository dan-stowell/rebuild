/* lrt.c -- the runtime for Lua programs compiled by luac (Lua -> C).
 *
 * A value is one int:  nil 0, false 2, true 4;  an integer n is 2n+1
 * (so integers are 31-bit);  anything else is a pointer to an object
 * whose first word says what it is.  Memory is never freed.
 *
 * Calling convention: arguments are on the value stack stk[base ..
 * base+nargs); a function leaves its results at stk[base ..] and returns
 * how many there are.  top is the first free slot.
 */

enum { NIL = 0, FALSE = 2, TRUE = 4 };
enum { OSTR = 1, OTAB, OFUN };
enum { STKSZ = 1000000 };

int stk[STKSZ];
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

int mkint(int n) { return n << 1 | 1; }
int isobj(int v) { return !(v & 1) && v > 8; }
int otype(int v) { if (isobj(v)) return *(int *)v; return 0; }
int istab(int v) { return otype(v) == OTAB; }
int isstr(int v) { return otype(v) == OSTR; }
int truthy(int v) { return v & -3; }

char *tname(int v) {
  if (v == NIL) return "nil";
  if (v == TRUE || v == FALSE) return "boolean";
  if (v & 1) return "number";
  if (otype(v) == OSTR) return "string";
  if (otype(v) == OTAB) return "table";
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
void sbint(int x) {
  int start; int i; int j; int c;
  if (x < 0) { sbput('-'); if (x == -2147483647 - 1) { sbputs("2147483648", 10); return; } x = -x; }
  start = sbn;
  do { sbput('0' + x % 10); x = x / 10; } while (x);
  i = start; j = sbn - 1;
  while (i < j) { c = sb[i]; sb[i] = sb[j]; sb[j] = c; i++; j--; }
}
void sbhex(int x, int width) {
  int d = 28; int c;
  while (d > 0 && !((x >> d) & 15) && d >= width * 4) d = d - 4;
  while (d >= 0) { c = (x >> d) & 15; sbput(c < 10 ? '0' + c : 'a' + c - 10); d = d - 4; }
}

/* --------------------------------------------------------------- strings */

/* string: [OSTR, length, hash, next in intern chain] then the bytes */
int strtab[65536];

int lstr(char *p, int n) {
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
int cstr(char *s) { return lstr(s, slen(s)); }
char *sptr(int v) { return (char *)v + 16; }
int sl(int v) { return ((int *)v)[1]; }
int sbstr() { int s = lstr(sb, sbn); sbn = 0; return s; }

/* parse a whole string as an integer; NIL if it is not one */
int str2int(int s) {
  char *p = sptr(s); int n = sl(s); int i = 0; int neg = 0; int v = 0; int d; int any = 0;
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
  if (i < n && (p[i] == '-' || p[i] == '+')) { neg = p[i] == '-'; i++; }
  if (i + 1 < n && p[i] == '0' && (p[i + 1] == 'x' || p[i + 1] == 'X')) {
    i = i + 2;
    while (i < n) {
      d = p[i];
      if (d >= '0' && d <= '9') d = d - '0';
      else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
      else if (d >= 'A' && d <= 'F') d = d - 'A' + 10;
      else break;
      v = v * 16 + d; i++; any = 1;
    }
  } else {
    while (i < n && p[i] >= '0' && p[i] <= '9') { v = v * 10 + p[i] - '0'; i++; any = 1; }
  }
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
  if (!any || i != n) return NIL;
  return mkint(neg ? -v : v);
}

/* ---------------------------------------------------------------- tables */

/* table: [OTAB, array, n, array capacity, hash keys, hash values,
 *         hash capacity, hash count, metatable] */
int newtable() {
  int *t = (int *)malloc(36);
  t[0] = OTAB; t[1] = 0; t[2] = 0; t[3] = 0; t[4] = 0; t[5] = 0; t[6] = 0; t[7] = 0; t[8] = 0;
  return (int)t;
}
int khash(int k) { int h = k * -1640531535; return h ^ (h >> 15); }
int hslot(int *t, int k) {
  int m = t[6] - 1; int i = khash(k) & m; int *hk = (int *)t[4];
  while (hk[i] && hk[i] != k) i = (i + 1) & m;
  return i;
}
int tget(int tv, int k) {
  int *t = (int *)tv; int i;
  if (k & 1) {
    i = k >> 1;
    if (i >= 1 && i <= t[2]) return ((int *)t[1])[i - 1];
  }
  if (!t[6]) return NIL;
  i = hslot(t, k);
  if (((int *)t[4])[i] == k) return ((int *)t[5])[i];
  return NIL;
}
void hset(int *t, int k, int v);
void hresize(int *t) {
  int oc = t[6]; int *ok = (int *)t[4]; int *ov = (int *)t[5]; int i; int nc = 4;
  while (nc * 3 <= t[7] * 4 + 4) nc = nc * 2;
  t[4] = (int)malloc(nc * 4); t[5] = (int)malloc(nc * 4); t[6] = nc; t[7] = 0;
  for (i = 0; i < oc; i++) if (ok[i] && ov[i] != NIL) hset(t, ok[i], ov[i]);
}
void hset(int *t, int k, int v) {
  int i; int *hk;
  if (!t[6]) { if (v == NIL) return; hresize(t); }
  i = hslot(t, k); hk = (int *)t[4];
  if (!hk[i]) {
    if (v == NIL) return;
    if ((t[7] + 1) * 4 > t[6] * 3) { hresize(t); i = hslot(t, k); hk = (int *)t[4]; }
    hk[i] = k; t[7]++;
  }
  ((int *)t[5])[i] = v;
}
void aappend(int *t, int v) {
  int *na; int nc;
  if (t[2] == t[3]) {
    nc = t[3] ? t[3] * 2 : 4;
    na = (int *)malloc(nc * 4);
    memcopy((char *)na, (char *)t[1], t[2] * 4);
    t[1] = (int)na; t[3] = nc;
  }
  ((int *)t[1])[t[2]++] = v;
}
void tset(int tv, int k, int v) {
  int *t = (int *)tv; int i; int *a; int j;
  if (k & 1) {
    i = k >> 1;
    if (i >= 1 && i <= t[2]) {
      a = (int *)t[1];
      a[i - 1] = v;
      if (v == NIL && i == t[2]) while (t[2] > 0 && a[t[2] - 1] == NIL) t[2]--;
      return;
    }
    if (i == t[2] + 1 && v != NIL) {
      aappend(t, v);
      if (t[6]) {
        hset(t, k, NIL);
        while (1) {   /* move following keys from the hash part */
          j = hslot(t, mkint(t[2] + 1));
          if (((int *)t[4])[j] != mkint(t[2] + 1) || ((int *)t[5])[j] == NIL) break;
          aappend(t, ((int *)t[5])[j]);
          ((int *)t[5])[j] = NIL;
        }
      }
      return;
    }
  }
  if (k == NIL) lerror("table index is nil");
  hset(t, k, v);
}
int tlen(int tv) { return ((int *)tv)[2]; }

/* next(t, k): the key after k, writing key and value to stk[at] */
int tnext(int tv, int k, int at) {
  int *t = (int *)tv; int pos; int j; int *hk; int *hv;
  if (k == NIL) pos = 0;
  else if ((k & 1) && (k >> 1) >= 1 && (k >> 1) <= t[2]) pos = k >> 1;
  else {
    if (!t[6] || ((int *)t[4])[hslot(t, k)] != k) lerror("invalid key to 'next'");
    pos = t[2] + hslot(t, k) + 1;
  }
  while (pos < t[2]) {
    if (((int *)t[1])[pos] != NIL) {
      stk[at] = mkint(pos + 1); stk[at + 1] = ((int *)t[1])[pos];
      return 2;
    }
    pos++;
  }
  hk = (int *)t[4]; hv = (int *)t[5];
  for (j = pos - t[2]; j < t[6]; j++) {
    if (hk[j] && hv[j] != NIL) { stk[at] = hk[j]; stk[at + 1] = hv[j]; return 2; }
  }
  stk[at] = NIL;
  return 1;
}

/* ------------------------------------------------------ cells, closures */

int newcell(int v) { int *c = (int *)malloc(4); *c = v; return (int)c; }
int cget(int c) { return *(int *)c; }
void cset(int c, int v) { *(int *)c = v; }

/* closure: [OFUN, C function, number of upvalues, upvalue cells...] */
int mkclo(int fn, int nup) {
  int *o = (int *)malloc(12 + 4 * nup);
  o[0] = OFUN; o[1] = fn; o[2] = nup;
  return (int)o;
}
void setuv(int c, int i, int cell) { ((int *)c)[3 + i] = cell; }
int uvcell(int c, int i) { return ((int *)c)[3 + i]; }
int uvget(int c, int i) { return *(int *)(((int *)c)[3 + i]); }
void uvset(int c, int i, int v) { *(int *)(((int *)c)[3 + i]) = v; }

int s_index; int s_newindex; int s_call; int s_tostring; int s_len;
int strlib;
int globals;

int metaof(int v) { if (istab(v)) return ((int *)v)[8]; return NIL; }
int metafield(int v, int k) { int m = metaof(v); if (m == NIL) return NIL; return tget(m, k); }

int lcall(int f, int base, int nargs) {
  int fp; int h; int i;
  if (base > STKSZ - 1000) lerror("stack overflow");
  if (otype(f) == OFUN) { fp = ((int *)f)[1]; return fp(f, base, nargs); }
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
int call2(int f, int a, int b, int nargs) {
  int b0 = top; int n;
  stk[top++] = a; stk[top++] = b;
  n = lcall(f, b0, nargs);
  top = b0;
  return n ? stk[b0] : NIL;
}

int gget(int k) { return tget(globals, k); }
void gset(int k, int v) { tset(globals, k, v); }

int lindex(int o, int k) {
  int v; int h;
  if (istab(o)) {
    v = tget(o, k);
    if (v != NIL || !((int *)o)[8]) return v;
    h = tget(((int *)o)[8], s_index);
    if (h == NIL) return NIL;
    if (istab(h)) return lindex(h, k);
    return call2(h, o, k, 2);
  }
  if (isstr(o)) return tget(strlib, k);
  lerror3("attempt to index a ", tname(o), " value");
  return NIL;
}
void lsetindex(int o, int k, int v) {
  int h; int b0;
  if (!istab(o)) lerror3("attempt to index a ", tname(o), " value");
  if (((int *)o)[8] && tget(o, k) == NIL) {
    h = tget(((int *)o)[8], s_newindex);
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

int tonumber(int v) {
  if (v & 1) return v;
  if (isstr(v)) return str2int(v);
  return NIL;
}
int arithok(int a, int b) {
  if (!(tonumber(a) & 1)) lerror3("attempt to perform arithmetic on a ", tname(a), " value");
  if (!(tonumber(b) & 1)) lerror3("attempt to perform arithmetic on a ", tname(b), " value");
  return 1;
}
int ladd(int a, int b) {
  if (a & b & 1) return a + b - 1;
  arithok(a, b); return ladd(tonumber(a), tonumber(b));
}
int lsub(int a, int b) {
  if (a & b & 1) return a - b + 1;
  arithok(a, b); return lsub(tonumber(a), tonumber(b));
}
int lmul(int a, int b) {
  if (a & b & 1) return mkint((a >> 1) * (b >> 1));
  arithok(a, b); return lmul(tonumber(a), tonumber(b));
}
int lidiv(int a, int b) {
  int x; int y; int q;
  if (!(a & b & 1)) { arithok(a, b); return lidiv(tonumber(a), tonumber(b)); }
  x = a >> 1; y = b >> 1;
  if (y == 0) lerror("attempt to perform 'n//0'");
  q = x / y;
  if (x % y != 0 && (x < 0) != (y < 0)) q--;
  return mkint(q);
}
int lmod(int a, int b) {
  int x; int y; int r;
  if (!(a & b & 1)) { arithok(a, b); return lmod(tonumber(a), tonumber(b)); }
  x = a >> 1; y = b >> 1;
  if (y == 0) lerror("attempt to perform 'n%%0'");
  r = x % y;
  if (r != 0 && (r < 0) != (y < 0)) r = r + y;
  return mkint(r);
}
int ldiv(int a, int b) { lerror("floats are not supported ('/'; use '//')"); return NIL; }
int lpow(int a, int b) { lerror("floats are not supported ('^')"); return NIL; }
int lunm(int a) {
  if (a & 1) return 2 - a;
  arithok(a, a); return lunm(tonumber(a));
}
int intok(int a, int b) {
  if (!(a & b & 1)) lerror("attempt to perform bitwise operation on a non-integer value");
  return 1;
}
int lband(int a, int b) { intok(a, b); return a & b; }
int lbor(int a, int b) { intok(a, b); return a | b; }
int lbxor(int a, int b) { intok(a, b); return (a ^ b) | 1; }
int lshl(int a, int b) {
  int y; intok(a, b); y = b >> 1;
  if (y < 0) return mkint((a >> 1) >> -y);
  if (y >= 32) return 1;
  return mkint((a >> 1) << y);
}
int lshr(int a, int b) {
  int y; intok(a, b); y = b >> 1;
  if (y < 0) return mkint((a >> 1) << -y);
  if (y >= 32) return 1;
  return mkint(((a >> 1) >> y) & ~(-1 << (32 - y)));
}
int lbnot(int a) { intok(a, a); return ~a | 1; }

int lnot(int a) { return truthy(a) ? FALSE : TRUE; }
int leq(int a, int b) { return a == b ? TRUE : FALSE; }
int lne(int a, int b) { return a != b ? TRUE : FALSE; }

int strcmp_(int a, int b) {
  char *p = sptr(a); char *q = sptr(b); int n = sl(a); int m = sl(b); int i = 0;
  while (i < n && i < m) {
    if ((p[i] & 255) != (q[i] & 255)) return (p[i] & 255) - (q[i] & 255);
    i++;
  }
  return n - m;
}
int cmperr(int a, int b) {
  if (tname(a) == tname(b)) lerror3("attempt to compare two ", tname(a), " values");
  lerror3("attempt to compare ", tname(a), " with another type");
  return 0;
}
int llt(int a, int b) {
  if (a & b & 1) return a < b ? TRUE : FALSE;
  if (isstr(a) && isstr(b)) return strcmp_(a, b) < 0 ? TRUE : FALSE;
  return cmperr(a, b);
}
int lle(int a, int b) {
  if (a & b & 1) return a <= b ? TRUE : FALSE;
  if (isstr(a) && isstr(b)) return strcmp_(a, b) <= 0 ? TRUE : FALSE;
  return cmperr(a, b);
}
int lgt(int a, int b) { return llt(b, a); }
int lge(int a, int b) { return lle(b, a); }

int tostr(int v);
int llen(int v) {
  int h;
  if (isstr(v)) return mkint(sl(v));
  if (istab(v)) {
    h = metafield(v, s_len);
    if (h != NIL) return call2(h, v, NIL, 1);
    return mkint(tlen(v));
  }
  lerror3("attempt to get length of a ", tname(v), " value");
  return NIL;
}
void sbval(int v) {   /* a string or number, for concatenation */
  if (v & 1) sbint(v >> 1);
  else if (isstr(v)) sbputs(sptr(v), sl(v));
  else lerror3("attempt to concatenate a ", tname(v), " value");
}
int lconcat(int a, int b) {
  sbn = 0;
  sbval(a); sbval(b);
  return sbstr();
}

/* the values in stk[from .. from+n) are pushed at top */
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
void tappend(int t, int from, int n, int start) {
  int i;
  for (i = 0; i < n; i++) tset(t, mkint(start + i), stk[from + i]);
}
void forprep(int a, int b, int c) {
  if (!(a & b & c & 1)) lerror("'for' values must be integers (floats are not supported)");
  if (c == 1) lerror("'for' step is zero");
}

/* -------------------------------------------------------------- library */

int arg(int base, int nargs, int i) { return i < nargs ? stk[base + i] : NIL; }
int ret1(int base, int v) { stk[base] = v; return 1; }

int tostr(int v) {
  int h;
  sbn = 0;
  if (v == NIL) return cstr("nil");
  if (v == TRUE) return cstr("true");
  if (v == FALSE) return cstr("false");
  if (v & 1) { sbint(v >> 1); return sbstr(); }
  if (isstr(v)) return v;
  h = metafield(v, s_tostring);
  if (h != NIL) return call2(h, v, NIL, 1);
  sbputs(tname(v), slen(tname(v)));
  sbputs(": 0x", 4);
  sbhex(v, 8);
  return sbstr();
}

int checkint(int base, int nargs, int i, char *fname) {
  int v = tonumber(arg(base, nargs, i));
  if (!(v & 1)) lerror3("bad argument to '", fname, "' (number expected)");
  return v >> 1;
}
int optint(int base, int nargs, int i, int d, char *fname) {
  if (arg(base, nargs, i) == NIL) return d;
  return checkint(base, nargs, i, fname);
}
int checkstr(int base, int nargs, int i, char *fname) {
  int v = arg(base, nargs, i);
  if (v & 1) return tostr(v);
  if (!isstr(v)) lerror3("bad argument to '", fname, "' (string expected)");
  return v;
}
int checktab(int base, int nargs, int i, char *fname) {
  int v = arg(base, nargs, i);
  if (!istab(v)) lerror3("bad argument to '", fname, "' (table expected)");
  return v;
}

int lib_print(int clo, int base, int nargs) {
  int i; int s;
  for (i = 0; i < nargs; i++) {
    if (i) outb("\t", 1);
    s = tostr(stk[base + i]);
    outb(sptr(s), sl(s));
  }
  outb("\n", 1);
  return 0;
}
int lib_type(int clo, int base, int nargs) {
  if (!nargs) lerror("bad argument #1 to 'type' (value expected)");
  return ret1(base, cstr(tname(stk[base])));
}
int lib_tostring(int clo, int base, int nargs) { return ret1(base, tostr(arg(base, nargs, 0))); }
int lib_tonumber(int clo, int base, int nargs) {
  int v = arg(base, nargs, 0); int b; int s; char *p; int n; int i; int d; int r = 0; int neg = 0;
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
  return ret1(base, mkint(neg ? -r : r));
}
int lib_next(int clo, int base, int nargs) {
  return tnext(checktab(base, nargs, 0, "next"), arg(base, nargs, 1), base);
}
int nextfn;
int lib_pairs(int clo, int base, int nargs) {
  int t = checktab(base, nargs, 0, "pairs");
  stk[base] = nextfn; stk[base + 1] = t; stk[base + 2] = NIL;
  return 3;
}
int lib_ipairs_iter(int clo, int base, int nargs) {
  int i = stk[base + 1] + 2; int v = lindex(stk[base], i);
  if (v == NIL) return ret1(base, NIL);
  stk[base] = i; stk[base + 1] = v;
  return 2;
}
int ipairsfn;
int lib_ipairs(int clo, int base, int nargs) {
  stk[base + 1] = arg(base, nargs, 0); stk[base] = ipairsfn; stk[base + 2] = mkint(0);
  return 3;
}
int lib_select(int clo, int base, int nargs) {
  int n = arg(base, nargs, 0);
  if (isstr(n) && sl(n) == 1 && sptr(n)[0] == '#') return ret1(base, mkint(nargs - 1));
  n = checkint(base, nargs, 0, "select");
  if (n < 0) n = nargs + n;
  else if (n == 0) lerror("bad argument #1 to 'select' (index out of range)");
  if (n < 1) lerror("bad argument #1 to 'select' (index out of range)");
  if (n >= nargs) return 0;
  return lret(base, base + n, nargs - n);
}
int lib_rawget(int clo, int base, int nargs) {
  return ret1(base, tget(checktab(base, nargs, 0, "rawget"), arg(base, nargs, 1)));
}
int lib_rawset(int clo, int base, int nargs) {
  tset(checktab(base, nargs, 0, "rawset"), arg(base, nargs, 1), arg(base, nargs, 2));
  return 1;
}
int lib_rawequal(int clo, int base, int nargs) {
  return ret1(base, leq(arg(base, nargs, 0), arg(base, nargs, 1)));
}
int lib_rawlen(int clo, int base, int nargs) {
  int v = arg(base, nargs, 0);
  if (istab(v)) return ret1(base, mkint(tlen(v)));
  return ret1(base, llen(v));
}
int lib_setmetatable(int clo, int base, int nargs) {
  int t = checktab(base, nargs, 0, "setmetatable");
  ((int *)t)[8] = arg(base, nargs, 1);
  return 1;
}
int lib_getmetatable(int clo, int base, int nargs) {
  int v = arg(base, nargs, 0);
  if (isstr(v)) return ret1(base, NIL);
  return ret1(base, metaof(v));
}
int lib_assert(int clo, int base, int nargs) {
  int m;
  if (!nargs || !truthy(stk[base])) {
    if (nargs > 1) { m = tostr(stk[base + 1]); lerror(sptr(m)); }
    lerror("assertion failed!");
  }
  return nargs;
}
int lib_error(int clo, int base, int nargs) {
  int m = tostr(arg(base, nargs, 0));
  lerror(sptr(m));
  return 0;
}
/* pcall cannot catch errors here (an error stops the program), but a
 * call that succeeds behaves as it should */
int lib_pcall(int clo, int base, int nargs) {
  int n = lcall(arg(base, nargs, 0), base + 1, nargs - 1);
  stk[base] = TRUE;
  return n + 1;
}
int lib_unpack(int clo, int base, int nargs) {
  int t = arg(base, nargs, 0); int i = optint(base, nargs, 1, 1, "unpack");
  int j = arg(base, nargs, 2) == NIL ? (llen(t) >> 1) : checkint(base, nargs, 2, "unpack");
  int n = 0;
  if (base + j - i > STKSZ - 1000) lerror("too many results to unpack");
  while (i <= j) { stk[base + n] = lindex(t, mkint(i)); n++; i++; }
  return n;
}

/* string library */
int strindex(int i, int n) {   /* Lua's index into a string of length n */
  if (i < 0) i = n + i + 1;
  return i;
}
int lib_len(int clo, int base, int nargs) { return ret1(base, mkint(sl(checkstr(base, nargs, 0, "len")))); }
int lib_sub(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "sub"); int n = sl(s);
  int i = strindex(optint(base, nargs, 1, 1, "sub"), n);
  int j = strindex(optint(base, nargs, 2, -1, "sub"), n);
  if (i < 1) i = 1;
  if (j > n) j = n;
  if (i > j) return ret1(base, cstr(""));
  return ret1(base, lstr(sptr(s) + i - 1, j - i + 1));
}
int lib_upper(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "upper"); int i; int c;
  sbn = 0;
  for (i = 0; i < sl(s); i++) { c = sptr(s)[i]; sbput(c >= 'a' && c <= 'z' ? c - 32 : c); }
  return ret1(base, sbstr());
}
int lib_lower(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "lower"); int i; int c;
  sbn = 0;
  for (i = 0; i < sl(s); i++) { c = sptr(s)[i]; sbput(c >= 'A' && c <= 'Z' ? c + 32 : c); }
  return ret1(base, sbstr());
}
int lib_rep(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "rep"); int n = checkint(base, nargs, 1, "rep");
  int sep = arg(base, nargs, 2) == NIL ? NIL : checkstr(base, nargs, 2, "rep");
  sbn = 0;
  while (n > 0) {
    sbputs(sptr(s), sl(s));
    if (n > 1 && sep != NIL) sbputs(sptr(sep), sl(sep));
    n--;
  }
  return ret1(base, sbstr());
}
int lib_reverse(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "reverse"); int i;
  sbn = 0;
  for (i = sl(s) - 1; i >= 0; i--) sbput(sptr(s)[i]);
  return ret1(base, sbstr());
}
int lib_byte(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "byte"); int n = sl(s);
  int i = strindex(optint(base, nargs, 1, 1, "byte"), n);
  int j = arg(base, nargs, 2) == NIL ? i : strindex(checkint(base, nargs, 2, "byte"), n);
  int k = 0; char *p = sptr(s);
  if (i < 1) i = 1;
  if (j > n) j = n;
  while (i <= j) { stk[base + k] = mkint(p[i - 1] & 255); k++; i++; }
  return k;
}
int lib_char(int clo, int base, int nargs) {
  int i;
  sbn = 0;
  for (i = 0; i < nargs; i++) sbput(checkint(base, nargs, i, "char"));
  return ret1(base, sbstr());
}
int lib_find(int clo, int base, int nargs) {
  int s = checkstr(base, nargs, 0, "find"); int p = checkstr(base, nargs, 1, "find");
  int init = strindex(optint(base, nargs, 2, 1, "find"), sl(s));
  int i; int j; char *ps = sptr(p);
  if (!truthy(arg(base, nargs, 3)))
    for (j = 0; j < sl(p); j++)
      if (ps[j] == '^' || ps[j] == '$' || ps[j] == '*' || ps[j] == '+' || ps[j] == '?' ||
          ps[j] == '.' || ps[j] == '[' || ps[j] == '%' || ps[j] == '(' || ps[j] == '-')
        lerror("string patterns are not supported (only plain find)");
  if (init < 1) init = 1;
  for (i = init - 1; i + sl(p) <= sl(s); i++) {
    if (memeq(sptr(s) + i, ps, sl(p))) {
      stk[base] = mkint(i + 1); stk[base + 1] = mkint(i + sl(p));
      return 2;
    }
  }
  return ret1(base, NIL);
}
int lib_format(int clo, int base, int nargs) {
  int f = checkstr(base, nargs, 0, "format"); char *p = sptr(f); int n = sl(f);
  int i = 0; int a = 1; int left; int zero; int width; int prec; int c; int v; int start; int len; int pad;
  int out = 0;
  char *tmp; int tn;
  tmp = 0; tn = 0;
  sbn = 0;
  while (i < n) {
    if (p[i] != '%') { sbput(p[i]); i++; continue; }
    i++;
    if (p[i] == '%') { sbput('%'); i++; continue; }
    left = 0; zero = 0; width = 0; prec = -1;
    while (p[i] == '-' || p[i] == '0' || p[i] == '+' || p[i] == ' ' || p[i] == '#') {
      if (p[i] == '-') left = 1;
      if (p[i] == '0') zero = 1;
      i++;
    }
    while (p[i] >= '0' && p[i] <= '9') { width = width * 10 + p[i] - '0'; i++; }
    if (p[i] == '.') { i++; prec = 0; while (p[i] >= '0' && p[i] <= '9') { prec = prec * 10 + p[i] - '0'; i++; } }
    c = p[i]; i++;
    start = sbn;
    if (c == 'd' || c == 'i') {
      v = checkint(base, nargs, a++, "format");
      if (v < 0) { sbput('-'); v = -v; }
      len = 1; pad = v;
      while (pad >= 10) { pad = pad / 10; len++; }
      while (prec > len) { sbput('0'); prec--; }
      sbint(v);
    } else if (c == 'x' || c == 'X') {
      v = checkint(base, nargs, a++, "format");
      len = sbn; sbhex(v, 1);
      if (c == 'X') for (len = len; len < sbn; len++) if (sb[len] >= 'a') sb[len] = sb[len] - 32;
    } else if (c == 'c') {
      sbput(checkint(base, nargs, a++, "format"));
    } else if (c == 's') {
      v = tostr(arg(base, nargs, a++));
      tmp = sptr(v); tn = sl(v);
      if (prec >= 0 && prec < tn) tn = prec;
      sbn = start;   /* tostr used the buffer */
      sbputs(tmp, tn);
    } else if (c == 'q') {
      v = checkstr(base, nargs, a++, "format");
      sbput('"');
      for (len = 0; len < sl(v); len++) {
        c = sptr(v)[len];
        if (c == '"' || c == '\\') { sbput('\\'); sbput(c); }
        else if (c == '\n') { sbput('\\'); sbput('n'); }
        else sbput(c);
      }
      sbput('"');
    } else lerror("invalid conversion in format (floats are not supported)");
    len = sbn - start;
    if (len < width) {   /* pad */
      pad = width - len;
      sbneed(pad);
      if (left) { while (pad--) sbput(' '); }
      else {
        for (v = len - 1; v >= 0; v--) sb[start + pad + v] = sb[start + v];
        for (v = 0; v < pad; v++) sb[start + v] = zero && c != 's' ? '0' : ' ';
        if (zero && c != 's' && sb[start + pad] == '-') { sb[start] = '-'; sb[start + pad] = '0'; }
        sbn = sbn + pad;
      }
    }
  }
  return ret1(base, sbstr());
}

/* table library */
int lib_insert(int clo, int base, int nargs) {
  int t = checktab(base, nargs, 0, "insert"); int n = tlen(t); int pos; int i;
  if (nargs == 2) { tset(t, mkint(n + 1), stk[base + 1]); return 0; }
  if (nargs != 3) lerror("wrong number of arguments to 'insert'");
  pos = checkint(base, nargs, 1, "insert");
  if (pos < 1 || pos > n + 1) lerror("bad argument #2 to 'insert' (position out of bounds)");
  for (i = n; i >= pos; i--) tset(t, mkint(i + 1), tget(t, mkint(i)));
  tset(t, mkint(pos), stk[base + 2]);
  return 0;
}
int lib_remove(int clo, int base, int nargs) {
  int t = checktab(base, nargs, 0, "remove"); int n = tlen(t); int pos = n; int v; int i;
  if (nargs > 1) pos = checkint(base, nargs, 1, "remove");
  if (n == 0 && nargs < 2) return ret1(base, NIL);
  v = tget(t, mkint(pos));
  for (i = pos; i < n; i++) tset(t, mkint(i), tget(t, mkint(i + 1)));
  if (pos <= n) tset(t, mkint(n), NIL);
  return ret1(base, v);
}
int lib_concat(int clo, int base, int nargs) {
  int t = checktab(base, nargs, 0, "concat");
  int sep = arg(base, nargs, 1) == NIL ? NIL : checkstr(base, nargs, 1, "concat");
  int i = optint(base, nargs, 2, 1, "concat");
  int j = arg(base, nargs, 3) == NIL ? tlen(t) : checkint(base, nargs, 3, "concat");
  int v;
  sbn = 0;
  for (; i <= j; i++) {
    v = tget(t, mkint(i));
    if (!(v & 1) && !isstr(v)) lerror("invalid value (at index) in table for 'concat'");
    sbval(v);
    if (i < j && sep != NIL) sbputs(sptr(sep), sl(sep));
  }
  return ret1(base, sbstr());
}
int sortcmp;
int lessthan(int a, int b) {
  if (sortcmp == NIL) return llt(a, b) == TRUE;
  return truthy(call2(sortcmp, a, b, 2));
}
void msort(int *a, int *tmp, int n) {
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
  int t = checktab(base, nargs, 0, "sort"); int n = tlen(t); int *a; int *tmp; int i; int old = sortcmp;
  int ot = top;
  sortcmp = arg(base, nargs, 1);
  top = base + nargs;
  a = (int *)malloc(n * 4 + 4); tmp = (int *)malloc(n * 4 + 4);
  for (i = 0; i < n; i++) a[i] = tget(t, mkint(i + 1));
  msort(a, tmp, n);
  for (i = 0; i < n; i++) tset(t, mkint(i + 1), a[i]);
  sortcmp = old; top = ot;
  return 0;
}

/* math library: integers only */
int lib_abs(int clo, int base, int nargs) {
  int v = checkint(base, nargs, 0, "abs");
  return ret1(base, mkint(v < 0 ? -v : v));
}
int lib_max(int clo, int base, int nargs) {
  int m = checkint(base, nargs, 0, "max"); int i; int v;
  for (i = 1; i < nargs; i++) { v = checkint(base, nargs, i, "max"); if (v > m) m = v; }
  return ret1(base, mkint(m));
}
int lib_min(int clo, int base, int nargs) {
  int m = checkint(base, nargs, 0, "min"); int i; int v;
  for (i = 1; i < nargs; i++) { v = checkint(base, nargs, i, "min"); if (v < m) m = v; }
  return ret1(base, mkint(m));
}
int lib_floor(int clo, int base, int nargs) { return ret1(base, mkint(checkint(base, nargs, 0, "floor"))); }
int lib_tointeger(int clo, int base, int nargs) {
  int v = arg(base, nargs, 0);
  return ret1(base, (v & 1) ? v : NIL);
}
int lib_mathtype(int clo, int base, int nargs) {
  int v = arg(base, nargs, 0);
  return ret1(base, (v & 1) ? cstr("integer") : NIL);
}
int lib_fmod(int clo, int base, int nargs) {
  int a = checkint(base, nargs, 0, "fmod"); int b = checkint(base, nargs, 1, "fmod");
  if (b == 0) lerror("bad argument #2 to 'fmod' (zero)");
  return ret1(base, mkint(a % b));
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
  int i; int v;
  for (i = 0; i < nargs; i++) {
    v = stk[base + i];
    if (v & 1) { sbn = 0; sbint(v >> 1); outb(sb, sbn); }
    else if (isstr(v)) outb(sptr(v), sl(v));
    else lerror3("bad argument to 'write' (string expected, got ", tname(v), ")");
  }
  return 0;
}
int lib_read(int clo, int base, int nargs) {
  int f = arg(base, nargs, 0); char *p; int c; int neg = 0; int v = 0; int any = 0;
  if (f == NIL) f = cstr("l");
  if (f & 1) lerror("io.read(n) is not supported");
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
    if (ipeek() == '-') { neg = 1; igetc(); }
    while ((c = ipeek()) >= '0' && c <= '9') { v = v * 10 + c - '0'; igetc(); any = 1; }
    if (!any) return ret1(base, NIL);
    return ret1(base, mkint(neg ? -v : v));
  }
  lerror("bad argument #1 to 'read' (invalid format)");
  return 0;
}
int lib_exit(int clo, int base, int nargs) {
  int v = arg(base, nargs, 0);
  flush();
  if (v == FALSE) proc_exit(1);
  if (v & 1) proc_exit(v >> 1);
  proc_exit(0);
  return 0;
}

void reg(int t, char *name, int fn) { tset(t, cstr(name), mkclo(fn, 0)); }
int lib(char *name) { int t = newtable(); tset(globals, cstr(name), t); return t; }

void lua_init() {
  int t;
  s_index = cstr("__index"); s_newindex = cstr("__newindex"); s_call = cstr("__call");
  s_tostring = cstr("__tostring"); s_len = cstr("__len");
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
  reg(t, "ceil", lib_floor); reg(t, "tointeger", lib_tointeger); reg(t, "type", lib_mathtype);
  reg(t, "fmod", lib_fmod);
  tset(t, cstr("maxinteger"), mkint(1073741823)); tset(t, cstr("mininteger"), mkint(-1073741824));
  t = lib("io");
  reg(t, "write", lib_write); reg(t, "read", lib_read);
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
