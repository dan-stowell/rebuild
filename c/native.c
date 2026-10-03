/* native.c -- dev only: lets gcc build cc.c for quick debugging. */
#include <unistd.h>
int sys_read(char *buf, int n) { return read(0, buf, n); }
int sys_write(int fd, char *buf, int n) { return write(fd, buf, n); }
