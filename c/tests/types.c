int sys_write(int fd, char *buf, int n);
long __f64_bits(double d);
double __f64_from_bits(long l);
char buf[64];
void puts_(char *s) { int n = 0; while (s[n]) n++; sys_write(1, s, n); }
void putl(long v) {
  int i = 63; unsigned long u;
  int neg = v < 0;
  u = neg ? -(unsigned long)v : (unsigned long)v;
  buf[i] = 0;
  do { i--; buf[i] = '0' + (int)(u % 10); u = u / 10; } while (u);
  if (neg) buf[--i] = '-';
  puts_(buf + i); puts_("\n");
}
long fact(long n) { return n <= 1 ? 1 : n * fact(n - 1); }
double half(double x) { return x / 2; }
unsigned char uc;
long g = 1234567890123L;
double dg;
int main() {
  long a = 1L << 40;
  unsigned u = 4000000000U;
  int i = -5;
  unsigned long ul = 18446744073709551615UL;
  double d = 10;
  char c = 200;
  putl(a); putl(a * 3 + 7); putl(fact(20)); putl(g); putl(-g / 7); putl(-g % 7);
  putl(u); putl(u / 3); putl((long)(u > 5)); putl((long)(i < u));
  putl((long)(ul >> 60)); putl((long)(ul / 1000000000000UL));
  putl(i); putl(i >> 1); putl((unsigned)i >> 28);
  putl((long)(d * d)); putl((long)half(d)); putl((long)__builtin_sqrt(d * d + 21));
  putl((long)(d == d)); d = d / 3; putl((long)(d * 1000000));
  putl(__f64_bits(d)); putl((long)__f64_from_bits(4611686018427387904L));
  dg = 3; dg += 1; putl((long)dg);
  putl(c); uc = 250; uc += 10; putl(uc);
  putl((long)(char)300); putl((long)(unsigned char)-1);
  putl(a > 5 && d > 2); putl(!a); putl(!dg); putl(~a);
  putl(i++ + ++i); putl(a--); putl(--a);
  putl((long)-d); putl(sizeof(long) + sizeof(double) * 100);
  return 0;
}
