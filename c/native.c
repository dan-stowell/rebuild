/* native.c -- dev only: lets gcc build cc.c for quick debugging. */
#include <unistd.h>
int sys_read(char *buf, int n) { return read(0, buf, n); }
int sys_write(int fd, char *buf, int n) { return write(fd, buf, n); }
#include <string.h>
long __f64_bits(double d) { long l; memcpy(&l, &d, 8); return l; }
double __f64_from_bits(long l) { double d; memcpy(&d, &l, 8); return d; }
