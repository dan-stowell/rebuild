#!/usr/bin/env python3
# Dev-only helper (NOT part of the bootstrap): assemble the hex format with
# `{ }` size brackets using *minimal* LEB128, to help compute explicit sizes.
import sys
src = sys.stdin.read()
out = bytearray(); stack = []; i = 0; acc = 1
def leb(n):
    b = bytearray()
    while True:
        x = n & 127; n >>= 7
        if n: b.append(x | 128)
        else: b.append(x); return b
lines = [l.split(';')[0] for l in src.split('\n')]
for ch in '\n'.join(lines):
    if ch == '{': stack.append(len(out))
    elif ch == '}':
        p = stack.pop(); n = len(out) - p
        sys.stderr.write(f"size at {p:#x}: {n} = {leb(n).hex(' ')}\n")
        out[p:p] = leb(n)
    elif ch in '0123456789abcdef':
        acc = acc << 4 | int(ch, 16)
        if acc > 255: out.append(acc & 255); acc = 1
sys.stdout.buffer.write(out)
