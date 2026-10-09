/* tail calls: return f(...) is a wasm return_call, so this runs in constant stack */
int count(int n, int acc) { if (n == 0) return acc; return count(n - 1, acc + 1); }
int even(int n);
int odd(int n) { if (n == 0) return 0; return even(n - 1); }
int even(int n) { if (n == 0) return 1; return odd(n - 1); }
int viaptr(int f, int n) { return f(n); }
char b[12];
int putnum(int n) {
  int i = 11;
  b[i] = '\n';
  do { b[--i] = '0' + n % 10; n = n / 10; } while (n);
  return sys_write(1, b + i, 12 - i);
}
int main() {
  putnum(count(10000000, 0));
  putnum(even(10000001));
  putnum(viaptr(even, 3000000));
  return 0;
}
