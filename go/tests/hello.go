package main

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	writeOut([]byte("hello from Go\n"))
	println("fib", 20, "=", fib(20))
	s := "abc"
	b := []byte(s)
	b = append(b, 'd')
	writeOut(b)
	writeOut([]byte("\n"))
}
