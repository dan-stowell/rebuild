int sys_write(int fd, char *buf, int n);
char *msg = "\x68i\x0a";
int twice(int x) { return 2 * x; }
int add(int a, int b) { return a + b; }
int apply(int f, int x) { return f(x); }
int g;
int main() {
  int f = add;
  g = twice;
  sys_write(1, msg, 3);
  if (apply(twice, 21) != 42) return 1;
  if (f(40, 2) != 42) return 2;
  if (g(f(1, 2)) != 6) return 3;
  sys_write(1, "ok\n", 3);
  return 0;
}
