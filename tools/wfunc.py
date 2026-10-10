#!/usr/bin/env python3
# usage: tools/wfunc.py module.wasm offset  -- which function's code contains offset (dev helper)
import sys
b = open(sys.argv[1], 'rb').read()
off = int(sys.argv[2], 0)
def uleb(i):
    r = 0; s = 0
    while True:
        x = b[i]; i += 1; r |= (x & 127) << s; s += 7
        if x < 128: return r, i
def name(i):
    n, i = uleb(i); return b[i:i+n].decode(), i + n
i = 8; nimp = 0; names = {}; bodies = []
while i < len(b):
    sid = b[i]; n, j = uleb(i + 1); end = j + n
    if sid == 2:
        cnt, k = uleb(j)
        for _ in range(cnt):
            _, k = name(k); _, k = name(k); kind = b[k]; k += 1
            _, k = uleb(k)
            if kind == 0: nimp += 1
    if sid == 10:
        cnt, k = uleb(j)
        for f in range(cnt):
            sz, k2 = uleb(k); bodies.append((k, k2 + sz)); k = k2 + sz
    if sid == 0:
        nm, k = name(j)
        if nm == 'name':
            while k < end:
                sub = b[k]; sz, k = uleb(k + 1); e2 = k + sz
                if sub == 1:
                    cnt, k = uleb(k)
                    for _ in range(cnt):
                        idx, k = uleb(k); s, k = name(k); names[idx] = s
                k = e2
    i = end
for f, (s, e) in enumerate(bodies):
    if s <= off < e:
        print(f + nimp, names.get(f + nimp, '?'), 'body at', hex(s), 'offset in body', off - s)
