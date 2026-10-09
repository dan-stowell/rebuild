int sys_write(int fd, char *buf, int n);
void put(char *s) { int n = 0; while (s[n]) n++; sys_write(1, s, n); }
char *name(int c) {
  switch (c) {
  case 0: return "zero";
  case 1: case 2: return "small";
  case 'a': return "a";
  default: return "other";
  case 1000000: return "million";
  }
}
int count(int n) {
  int r = 0; int i;
  for (i = 0; i < n; i++) {
    switch (i % 4) {
    case 0: r = r + 1;
    case 1: r = r + 10; break;
    case 2: continue;
    default: r = r + 100;
    }
    r = r + 1000;
  }
  return r;
}
int main() {
  put(name(0)); put(" "); put(name(2)); put(" "); put(name('a')); put(" ");
  put(name(7)); put(" "); put(name(1000000)); put(" "); put(name(-5)); put("\n");
  if (count(8) != 2 * (1 + 10 + 1000) + 2 * (10 + 1000) + 2 * (100 + 1000)) return 1;
  switch (3) { case 1: return 2; }
  put("ok\n");
  return 0;
}
