/* num.c -- exact conversions between doubles and decimal strings.
 *
 * Both directions are done exactly, with small bignums, so that the
 * results are correctly rounded (and so match C libraries and Lua).
 * Shared by luac.c (float literals) and lrt.c (tostring, tonumber,
 * string.format).
 */

/* ---------------------------------------------------- double -> digits */

/* a bignum in base 10^9, least significant limb first */
long bd[160]; int bdn;
void bdmul(long k) {   /* k < 2^31 */
  int i; long c = 0; long x;
  for (i = 0; i < bdn; i++) { x = bd[i] * k + c; bd[i] = x % 1000000000; c = x / 1000000000; }
  while (c) { bd[bdn++] = c % 1000000000; c = c / 1000000000; }
}

char dig[1200]; int nd; int dexp;   /* |d| = d0.d1d2... * 10^dexp */

/* all the decimal digits of |d| (finite, nonzero) */
void exactdigits(double d) {
  long bits = __f64_bits(d) & 9223372036854775807L;
  long e = bits >> 52; long m = bits & 4503599627370495L; int i; int j; long x;
  if (e == 0) e = 1; else m = m | 4503599627370496L;
  e = e - 1075;   /* d = m * 2^e */
  bdn = 0;
  while (m) { bd[bdn++] = m % 1000000000; m = m / 1000000000; }
  if (e >= 0) {
    while (e >= 30) { bdmul(1073741824L); e = e - 30; }
    if (e) bdmul(1L << e);
    e = 0;
  } else {
    i = (int)-e;
    while (i >= 13) { bdmul(1220703125L); i = i - 13; }
    x = 1; while (i > 0) { x = x * 5; i--; }
    bdmul(x);
  }
  /* to a digit string */
  nd = 0;
  x = bd[bdn - 1];
  j = 0; while (x) { dig[nd + j] = '0' + (int)(x % 10); x = x / 10; j++; }
  for (i = 0; i < j / 2; i++) { x = dig[nd + i]; dig[nd + i] = dig[nd + j - 1 - i]; dig[nd + j - 1 - i] = (int)x; }
  nd = j;
  for (i = bdn - 2; i >= 0; i--) {
    x = bd[i];
    for (j = 8; j >= 0; j--) { dig[nd + j] = '0' + (int)(x % 10); x = x / 10; }
    nd = nd + 9;
  }
  dexp = nd - 1 + (int)e;
  while (nd > 1 && dig[nd - 1] == '0') nd--;
}

/* round the digits to keep the first n (n may be <= 0), half to even */
void rounddigits(int n) {
  int up; int i;
  if (n <= 0) {   /* shift in leading zeros so that one digit is kept */
    for (i = nd - 1; i >= 0; i--) dig[i + 1 - n] = dig[i];
    for (i = 0; i < 1 - n; i++) dig[i] = '0';
    nd = nd + 1 - n; dexp = dexp + 1 - n; n = 1;
  }
  if (nd <= n) return;
  up = dig[n] > '5';
  if (dig[n] == '5') {
    up = (dig[n - 1] - '0') & 1;
    for (i = n + 1; i < nd; i++) if (dig[i] != '0') up = 1;
  }
  nd = n;
  if (up) {
    i = n - 1;
    while (i >= 0 && dig[i] == '9') { dig[i] = '0'; i--; }
    if (i >= 0) dig[i]++;
    else { for (i = nd; i > 0; i--) dig[i] = dig[i - 1]; dig[0] = '1'; dexp++; }
  }
  while (nd > 1 && dig[nd - 1] == '0') nd--;
  if (nd == 1 && dig[0] == '0') dexp = 0;
}

/* printf-style formatting of a double: conv is 'e', 'f' or 'g' (or the
 * upper-case forms), prec < 0 means the default.  alt is the '#' flag.
 * Writes to out (which must be large); returns the length. */
int fmtdbl(double d, int conv, int prec, int alt, char *out) {
  int n = 0; int i; int x; int up = conv < 'a'; int sci; int k;
  long bits = __f64_bits(d);
  if (up) conv = conv + 32;
  if (prec < 0) prec = 6;
  if (bits < 0) out[n++] = '-';
  if ((bits & 9218868437227405312L) == 9218868437227405312L) {   /* inf, nan */
    if (bits & 4503599627370495L) { out[n++] = up ? 'N' : 'n'; out[n++] = up ? 'A' : 'a'; out[n++] = up ? 'N' : 'n'; }
    else { out[n++] = up ? 'I' : 'i'; out[n++] = up ? 'N' : 'n'; out[n++] = up ? 'F' : 'f'; }
    return n;
  }
  if ((bits & 9223372036854775807L) == 0) { nd = 1; dig[0] = '0'; dexp = 0; }
  else exactdigits(d);
  if (conv == 'g') {
    if (prec == 0) prec = 1;
    rounddigits(prec);
    sci = dexp < -4 || dexp >= prec;
    if (sci) prec = prec - 1; else prec = prec - 1 - dexp;
    if (!alt) {   /* %g drops trailing zeros */
      k = sci ? nd - 1 : nd - 1 - dexp;
      if (k < prec) prec = k < 0 ? 0 : k;
    }
  } else if (conv == 'e') { rounddigits(prec + 1); sci = 1; }
  else { rounddigits(dexp + 1 + prec); sci = 0; }
  if (sci) {
    out[n++] = dig[0];
    if (prec > 0 || alt) out[n++] = '.';
    for (i = 1; i <= prec; i++) out[n++] = i < nd ? dig[i] : '0';
    out[n++] = up ? 'E' : 'e';
    x = dexp;
    if (x < 0) { out[n++] = '-'; x = -x; } else out[n++] = '+';
    if (x >= 100) out[n++] = '0' + x / 100;
    out[n++] = '0' + x / 10 % 10;
    out[n++] = '0' + x % 10;
  } else {
    if (dexp < 0) out[n++] = '0';
    for (i = 0; i <= dexp; i++) out[n++] = i < nd ? dig[i] : '0';
    if (prec > 0 || alt) out[n++] = '.';
    for (i = 1; i <= prec; i++) {
      k = dexp + i;
      out[n++] = k >= 0 && k < nd ? dig[k] : '0';
    }
  }
  return n;
}

/* ----------------------------------------------------- digits -> double */

/* a bignum in base 2^32, least significant limb first */
unsigned long bx[100]; int bxn;   /* the number */
unsigned long bp[100]; int bpn;   /* the divisor, a power of ten */
void bmul(unsigned long *b, int *n, unsigned long k) {
  int i; unsigned long c = 0; unsigned long x;
  for (i = 0; i < *n; i++) { x = b[i] * k + c; b[i] = x & 4294967295UL; c = x >> 32; }
  if (c) { b[*n] = c; *n = *n + 1; }
}
int bbits(unsigned long *b, int n) {
  int k = 0; unsigned long x;
  while (n > 0 && !b[n - 1]) n--;
  if (!n) return 0;
  x = b[n - 1];
  while (x) { k++; x = x >> 1; }
  return (n - 1) * 32 + k;
}
int bbit(unsigned long *b, int n, int i) {   /* bit i */
  if (i < 0 || i / 32 >= n) return 0;
  return (int)(b[i / 32] >> (i % 32)) & 1;
}
void bshl1(unsigned long *b, int *n) {
  int i; unsigned long c = 0; unsigned long x;
  for (i = 0; i < *n; i++) { x = b[i] << 1 | c; c = x >> 32; b[i] = x & 4294967295UL; }
  if (c) { b[*n] = c; *n = *n + 1; }
}
int bcmp(unsigned long *a, int an, unsigned long *b, int bn) {
  int i;
  while (an > 0 && !a[an - 1]) an--;
  while (bn > 0 && !b[bn - 1]) bn--;
  if (an != bn) return an < bn ? -1 : 1;
  for (i = an - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
void bsub(unsigned long *a, int an, unsigned long *b, int bn) {   /* a -= b */
  int i; long c = 0; long x;
  for (i = 0; i < an; i++) {
    x = (long)a[i] - (i < bn ? (long)b[i] : 0) - c;
    if (x < 0) { x = x + 4294967296L; c = 1; } else c = 0;
    a[i] = (unsigned long)x;
  }
}

/* the double nearest q * 2^e, where sticky says whether anything nonzero
 * was below q's lowest bit */
double mkdouble(unsigned long q, int e, int sticky, int neg) {
  int nb = 0; unsigned long x = q; int sh; unsigned long r; unsigned long half; long bits; int be;
  if (!q) return neg ? -(double)0 : (double)0;
  while (x) { nb++; x = x >> 1; }
  /* the leading bit has value 2^(nb - 1 + e) */
  be = nb - 1 + e;
  sh = nb - 53;   /* bits to drop */
  if (be < -1022) sh = sh + (-1022 - be);   /* subnormal */
  if (sh > 0) {
    if (sh > 63) { sticky = sticky || q; q = 0; }
    else {
      r = q & ((1UL << sh) - 1); half = 1UL << (sh - 1);
      q = q >> sh;
      if (r > half || (r == half && (sticky || (q & 1)))) q++;
      else if (r == half && !sticky && !(q & 1)) {}
    }
    e = e + sh;
  } else if (sh < 0) { q = q << -sh; e = e + sh; }
  /* now q < 2^54 (it may have carried to 2^53) and value = q * 2^e */
  if (q >> 53) { q = q >> 1; e++; }
  if (!q) return neg ? -(double)0 : (double)0;
  nb = 0; x = q; while (x) { nb++; x = x >> 1; }
  be = nb - 1 + e;
  if (be > 1023) bits = 9218868437227405312L;   /* inf */
  else if (nb < 53) bits = (long)q;              /* subnormal */
  else bits = ((long)(be + 1023) << 52) | ((long)q & 4503599627370495L);
  if (neg) bits = bits | (-9223372036854775807L - 1);
  return __f64_from_bits(bits);
}

int numok;   /* did str2dbl see a whole number? */

/* parse a decimal numeral s[0..n) (with optional sign, fraction and
 * exponent; surrounding spaces allowed) */
double str2dbl(char *s, int n) {
  int i = 0; int neg = 0; int any = 0; int sticky = 0; int e10 = 0; int ex = 0; int eneg = 0;
  int ndig = 0; int k; int b; unsigned long q; int j;
  numok = 0;
  bxn = 1; bx[0] = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    any = 1;
    if (ndig < 40) { bmul(bx, &bxn, 10); bx[0] = bx[0] + s[i] - '0'; if (bx[0] || bxn > 1) ndig++; }
    else { e10++; if (s[i] != '0') sticky = 1; }
    i++;
  }
  if (i < n && s[i] == '.') {
    i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      any = 1;
      if (ndig < 40) { bmul(bx, &bxn, 10); bx[0] = bx[0] + s[i] - '0'; if (bx[0] || bxn > 1) ndig++; e10--; }
      else if (s[i] != '0') sticky = 1;
      i++;
    }
  }
  if (!any) return 0;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < n && (s[i] == '-' || s[i] == '+')) { eneg = s[i] == '-'; i++; }
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { if (ex < 100000) ex = ex * 10 + s[i] - '0'; i++; }
  }
  while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
  if (i != n) return 0;
  numok = 1;
  e10 = e10 + (eneg ? -ex : ex);
  if (!bbits(bx, bxn)) return neg ? -(double)0 : (double)0;
  if (e10 > 400) return mkdouble(1, 2000, 0, neg);
  if (e10 < -400) return mkdouble(1, -2000, 0, neg);
  if (e10 >= 0) {
    for (k = 0; k < e10; k++) bmul(bx, &bxn, 10);
    b = bbits(bx, bxn);
    q = 0;
    for (j = b - 1; j >= b - 64 && j >= 0; j--) q = q << 1 | bbit(bx, bxn, j);
    for (j = b - 65; j >= 0; j--) if (bbit(bx, bxn, j)) sticky = 1;
    return mkdouble(q, b > 64 ? b - 64 : 0, sticky, neg);
  }
  /* q = floor(x * 2^k / 10^-e10), with about 60 bits */
  bpn = 1; bp[0] = 1;
  for (j = 0; j < -e10; j++) bmul(bp, &bpn, 10);
  k = 60 + bbits(bp, bpn) - bbits(bx, bxn);
  if (k < 0) k = 0;
  for (j = 0; j < k; j++) bshl1(bx, &bxn);
  /* long division, a bit at a time */
  q = 0;
  for (j = 0; j < 63; j++) bshl1(bp, &bpn);
  for (j = 63; j >= 0; j--) {
    q = q << 1;
    if (bcmp(bx, bxn, bp, bpn) >= 0) { bsub(bx, bxn, bp, bpn); q = q | 1; }
    if (j) {   /* bp = bp >> 1 */
      for (b = 0; b < bpn; b++) {
        bp[b] = bp[b] >> 1;
        if (b + 1 < bpn) bp[b] = bp[b] | (bp[b + 1] & 1) << 31;
      }
    }
  }
  if (bbits(bx, bxn)) sticky = 1;
  while (q >> 63) { sticky = sticky || (q & 1); q = q >> 1; k--; }
  return mkdouble(q, -k, sticky, neg);
}
