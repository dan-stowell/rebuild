/* libc.c -- the little runtime library for programs built with cc.
 * Compile it in front of a program: cat libc.c prog.c | cc */

int fd_read(int fd, int iovs, int n, int nread);
int fd_write(int fd, int iovs, int n, int nwritten);

int iov[4];

int sys_read(char *buf, int n) {
  iov[0] = (int)buf; iov[1] = n;
  if (fd_read(0, (int)iov, 1, (int)(iov + 2))) return -1;
  return iov[2];
}

int sys_write(int fd, char *buf, int n) {
  iov[0] = (int)buf; iov[1] = n;
  if (fd_write(fd, (int)iov, 1, (int)(iov + 2))) return -1;
  return iov[2];
}

/* memory is never freed: programs are short-lived */
char *brk_;
char *malloc(int n) {
  char *p;
  if (!brk_) brk_ = __heap_base();
  p = brk_;
  brk_ = brk_ + ((n + 7) & ~7);
  while ((int)brk_ > __memory_size() * 65536)
    if (__memory_grow(((int)brk_ >> 16) + 1 - __memory_size()) < 0) return (char *)0;
  return p;
}
void free(char *p) {}
