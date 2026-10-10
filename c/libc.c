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
char *malloc(int n) {   /* in ints only: the bootstrap compiler builds this too */
  char *p; int top; int pages;
  if (!brk_) brk_ = __heap_base();
  p = brk_;
  top = (int)brk_ + ((n + 7) & ~7);
  pages = ((top >> 16) & 65535) + 1;   /* 64K pages up to top, for up to 4G */
  if (n < 0 || pages <= (((int)brk_ >> 16) & 65535)) return (char *)0;   /* wrapped */
  if (pages > __memory_size() && __memory_grow(pages - __memory_size()) < 0) return (char *)0;
  brk_ = (char *)top;
  return p;
}
void free(char *p) {}
