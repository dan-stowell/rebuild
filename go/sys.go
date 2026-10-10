// sys.go -- the system interface, for building programs with the real Go
// toolchain (go run prog.go sys.go).  Programs built by gc get these from
// rt.go instead.
package main

import (
	"io"
	"os"
)

func readAll() []byte {
	b, _ := io.ReadAll(os.Stdin)
	return b
}
func writeOut(b []byte) { os.Stdout.Write(b) }
func writeErr(b []byte) { os.Stderr.Write(b) }
func exit(code int)     { os.Exit(code) }
