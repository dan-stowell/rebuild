// rt.go -- the runtime for programs compiled by gc, compiled in front of
// each of them.  The __ functions are gc's intrinsics (raw memory access),
// and functions without bodies are WASI imports.  Memory is never freed.
package main

func fd_read(fd int32, iovs int32, n int32, nread int32) int32
func fd_write(fd int32, iovs int32, n int32, nwritten int32) int32
func proc_exit(code int32)

var rt_heap int32
var rt_iov int32

func rt_alloc(n int32) int32 {
	if rt_heap == 0 {
		rt_heap = __heap_base()
	}
	p := (rt_heap + 7) &^ 7
	if n < 0 || int(p)+int(n) > 4294967295 {
		rt_fail("out of memory")
	}
	rt_heap = p + n
	top := int(rt_heap) & 4294967295
	for top > int(__memory_size())*65536 {
		if __memory_grow(int32((top>>16)+1-int(__memory_size()))) < 0 {
			rt_fail("out of memory")
		}
	}
	return p
}

func rt_fail(msg string) {
	writeErr([]byte("panic: " + msg + "\n"))
	proc_exit(2)
}
func rt_bounds() { rt_fail("index out of range") }

/* room for need elements of size size: the new data pointer and capacity */
func rt_grow(p int32, l int32, c int32, need int32, size int32) (int32, int32) {
	if need <= c {
		return p, c
	}
	nc := c * 2
	if nc < need {
		nc = need
	}
	if nc < 4 {
		nc = 4
	}
	np := rt_alloc(nc * size)
	__memcopy(np, p, l*size)
	return np, nc
}

func rt_concat(a string, b string) string {
	n := int32(len(a) + len(b))
	p := rt_alloc(n)
	__memcopy(p, __sptr(a), int32(len(a)))
	__memcopy(p+int32(len(a)), __sptr(b), int32(len(b)))
	return __mkstring(p, n)
}

func rt_strcmp(a string, b string) int {
	n := len(a)
	if len(b) < n {
		n = len(b)
	}
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			if a[i] < b[i] {
				return -1
			}
			return 1
		}
	}
	if len(a) < len(b) {
		return -1
	}
	if len(a) > len(b) {
		return 1
	}
	return 0
}

// string(r): r in UTF-8
func rt_runestr(r int) string {
	if r < 0 || r > 1114111 || r >= 55296 && r < 57344 {
		r = 65533
	}
	b := make([]byte, 0, 4)
	if r < 128 {
		b = append(b, byte(r))
	} else if r < 2048 {
		b = append(b, byte(192|r>>6), byte(128|r&63))
	} else if r < 65536 {
		b = append(b, byte(224|r>>12), byte(128|r>>6&63), byte(128|r&63))
	} else {
		b = append(b, byte(240|r>>18), byte(128|r>>12&63), byte(128|r>>6&63), byte(128|r&63))
	}
	return string(b)
}

func rt_iovec() int32 {
	if rt_iov == 0 {
		rt_iov = rt_alloc(16)
	}
	return rt_iov
}

func rt_write(fd int32, p int32, n int32) {
	iov := rt_iovec()
	for n > 0 {
		__store32(iov, p)
		__store32(iov+4, n)
		if fd_write(fd, iov, 1, iov+8) != 0 {
			return
		}
		w := __load32(iov + 8)
		p += w
		n -= w
	}
}

func writeOut(b []byte) { rt_write(1, __bptr(b), int32(len(b))) }
func writeErr(b []byte) { rt_write(2, __bptr(b), int32(len(b))) }
func exit(code int)     { proc_exit(int32(code)) }

func readAll() []byte {
	iov := rt_iovec()
	buf := make([]byte, 0, 65536)
	for {
		if len(buf) == cap(buf) {
			nb := make([]byte, len(buf), 2*cap(buf))
			copy(nb, buf)
			buf = nb
		}
		__store32(iov, __bptr(buf)+int32(len(buf)))
		__store32(iov+4, int32(cap(buf)-len(buf)))
		if fd_read(0, iov, 1, iov+8) != 0 {
			break
		}
		n := __load32(iov + 8)
		if n == 0 {
			break
		}
		buf = buf[:len(buf)+int(n)]
	}
	return buf
}

/* print and println: to stderr, as in Go */
func rt_printstr(s string) { rt_write(2, __sptr(s), int32(len(s))) }
func rt_printint(n int) {
	if n < 0 {
		rt_printstr("-")
		if n < -9 {
			rt_printint(-(n / 10))
		}
		rt_printdigit(-(n % 10))
		return
	}
	if n > 9 {
		rt_printint(n / 10)
	}
	rt_printdigit(n % 10)
}
func rt_printdigit(d int) {
	b := make([]byte, 1)
	b[0] = byte('0' + d)
	writeErr(b)
}
func rt_printbool(b bool) {
	if b {
		rt_printstr("true")
	} else {
		rt_printstr("false")
	}
}
