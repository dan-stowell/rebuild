// A tour of the subset, checked against the real Go toolchain's output.
package main

const (
	A = iota
	B
	C
	Big     = 1 << 40
	Mask    = Big - 1
	Letters = "abc"
)

const Typed byte = 200

var counter int
var names []string
var table = makeTable(5)

func makeTable(n int) []int {
	t := make([]int, n)
	for i := range n {
		t[i] = i * i
	}
	return t
}

func divmod(a int, b int) (int, int) { return a / b, a % b }

func swap(a string, b string) (string, string) { return b, a }

func sum(xs []int) int {
	s := 0
	for _, x := range xs {
		s += x
	}
	return s
}

func classify(n int) string {
	switch {
	case n < 0:
		return "negative"
	case n == 0:
		return "zero"
	case n < 10:
		return "small"
	}
	return "large"
}

func day(d int) string {
	switch d {
	case 0, 6:
		return "weekend"
	case 1, 2, 3, 4, 5:
		return "weekday"
	default:
		return "?"
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var b []byte
	for n > 0 {
		b = append(b, byte('0'+n%10))
		n /= 10
	}
	if neg {
		b = append(b, '-')
	}
	for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
		b[i], b[j] = b[j], b[i]
	}
	return string(b)
}

func say(s string) { writeOut([]byte(s + "\n")) }

func main() {
	say("consts: " + itoa(A) + " " + itoa(B) + " " + itoa(C) + " " + itoa(Big) + " " + itoa(Mask) + " " + Letters + " " + itoa(int(Typed)))
	q, r := divmod(17, 5)
	say("divmod: " + itoa(q) + " " + itoa(r))
	x, y := swap("first", "second")
	say("swap: " + x + " " + y)
	say("table: " + itoa(sum(table)) + " " + itoa(len(table)) + " " + itoa(table[4]))
	for _, n := range []int{-5, 0, 7, 100} {
		say(itoa(n) + " is " + classify(n))
	}
	say("days: " + day(0) + " " + day(3) + " " + day(9))

	// bytes wrap; int32 wraps; shifts
	var b byte = 250
	b += 10
	var i32 int32 = 2147483647
	i32++
	say("wrap: " + itoa(int(b)) + " " + itoa(int(i32)) + " " + itoa(-7/2) + " " + itoa(-7%2) + " " + itoa(1<<62>>60))
	say("bits: " + itoa(0xff&^0x0f) + " " + itoa(5^3) + " " + itoa(^0) + " " + itoa(6|9))

	// strings
	s := "hello, world"
	say("strings: " + s[0:5] + "|" + s[7:] + "|" + itoa(len(s)) + "|" + string(s[1]))
	if "abc" < "abd" && "b" > "a" && s == "hello, world" && s != "x" {
		say("comparisons ok")
	}
	bs := []byte("xyz")
	bs[0] = 'X'
	bs = append(bs, "!!"...)
	say(string(bs))

	// slices
	var nums []int
	for i := 0; i < 10; i++ {
		nums = append(nums, i*i)
	}
	part := nums[2:5]
	part[0] = 100
	say("slices: " + itoa(nums[2]) + " " + itoa(len(part)) + " " + itoa(cap(part)) + " " + itoa(sum(nums[:3])))
	cp := make([]int, 3)
	n := copy(cp, nums[5:])
	say("copy: " + itoa(n) + " " + itoa(cp[0]) + " " + itoa(cp[2]))
	names = append(names, "ann", "bob")
	names = append(names, names...)
	say("names: " + names[0] + names[1] + names[2] + names[3] + " " + itoa(len(names)))
	grid := make([][]int, 3)
	for i := range grid {
		grid[i] = make([]int, 3)
		grid[i][i] = 1
	}
	say("grid: " + itoa(grid[0][0]+grid[1][1]+grid[2][2]+grid[0][1]))

	// loops
	total := 0
	for i := 0; i < 100; i++ {
		if i%2 == 0 {
			continue
		}
		if i > 50 {
			break
		}
		total += i
	}
	k := 0
	for k < 5 {
		k++
	}
	for {
		counter++
		if counter == 3 {
			break
		}
	}
	say("loops: " + itoa(total) + " " + itoa(k) + " " + itoa(counter))
	if v := total * 2; v > 1000 {
		say("big " + itoa(v))
	} else if v > 100 {
		say("medium " + itoa(v))
	} else {
		say("small")
	}
	println("println:", 42, true, "done")
}
