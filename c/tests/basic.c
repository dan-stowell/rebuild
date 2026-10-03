int sys_write(int fd, char *buf, int n);
enum { N = 10, M = N * 2 };
int counter = 3;
char *greeting = "hello from C\n";
int arr[M];
char buf[64];

int len(char *s) { int n = 0; while (s[n]) n++; return n; }
void puts_(char *s) { sys_write(1, s, len(s)); }
void putnum(int n) {
  int i = 63;
  int neg = n < 0;
  if (neg) n = -n;
  buf[i] = 0;
  do { i--; buf[i] = '0' + n % 10; n /= 10; } while (n);
  if (neg) buf[--i] = '-';
  puts_(buf + i); puts_("\n");
}
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main() {
  int i; int sum = 0; int *p; char c = 200;
  puts_(greeting);
  for (i = 0; i < M; i++) arr[i] = i * i;
  for (i = 0; i < M; i++) { if (i == 15) break; if (i % 2) continue; sum += arr[i]; }
  putnum(sum);
  p = arr + 3; *p = 42; p++; *p += 5;
  putnum(arr[3]); putnum(arr[4]); putnum(p - arr);
  putnum(fib(20));
  putnum(c);
  putnum(-7 / 2); putnum(-7 % 2); putnum(1 << 10 | 3); putnum(-16 >> 2);
  putnum(counter-- + --counter); putnum(counter);
  putnum(sizeof(int) + sizeof(char));
  putnum(1 && 0 || 2 && 3);
  i = 0; while (1) { i++; if (i > 5) break; } putnum(i);
  buf[0] = 'a'; buf[0] += 1; putnum(buf[0]); putnum(buf[0]++); putnum(++buf[0]);
  return 0;
}
