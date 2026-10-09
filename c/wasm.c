/* wasm.c -- a WebAssembly interpreter, written in the C subset.
 *
 * stdin: the length of the guest module in decimal, a newline, the
 * module's bytes, and then whatever the guest itself reads.
 *
 * Each function is first translated into an internal code in which
 * branches are resolved: a branch says where to go, how many values it
 * carries and how high the stack is there, so no label stack is needed at
 * run time.  Each wasm instruction is executed by the C expression that cc
 * compiles to that same instruction.
 *
 * Supports wasm 1.0 without f32, plus sign extension, the saturating
 * float-to-int conversions, memory.copy and memory.fill; WASI enough for
 * stdin, stdout, stderr and exit.
 */

int sys_read(char *buf, int n);
int sys_write(int fd, char *buf, int n);
int fd_read(int fd, int iovs, int n, int nread);
int fd_write(int fd, int iovs, int n, int nwritten);
void proc_exit(int code);

enum { VSZ = 1048576, FSZ = 200000, MAXIOV = 64 };

/* ---------------------------------------------------------------- basics */

int slen(char *s) { int n = 0; while (s[n]) n++; return n; }
void eputs(char *s) { sys_write(2, s, slen(s)); }
char hexbuf[16];
void eputhex(int v) {
  int i;
  for (i = 0; i < 8; i++) hexbuf[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
  hexbuf[8] = 0;
  eputs("0x"); eputs(hexbuf);
}
void die(char *msg, int v) {
  eputs("wasm: "); eputs(msg); eputs(" "); eputhex(v); eputs("\n");
  __builtin_trap();
}

/* ---------------------------------------------------------------- module */

char *mod; int modlen; int rp;   /* the module, and where we are reading */

int u8() { if (rp >= modlen) die("unexpected end of module", rp); return mod[rp++] & 255; }
int uleb() {
  int r = 0; int s = 0; int b;
  do { b = u8(); if (s < 32) r = r | (b & 127) << s; s = s + 7; } while (b & 128);
  return r;
}
long sleb() {
  long r = 0; int s = 0; int b;
  do { b = u8(); if (s < 64) r = r | (long)(b & 127) << s; s = s + 7; } while (b & 128);
  if (s < 64 && (b & 64)) r = r | (-1L << s);
  return r;
}
int sec[13];   /* where each section's contents start */

int ntypes; int *tnp; int *tnr;
int nfuncs; int nimports; int *ftype; int *fhost; int *fcode; int *fnloc;
int nglobals; long *gval;
int *tab; int tabsize;
char *mem; long msize; long mmax;   /* memory: its size in bytes, its limit in pages */
int startfn = -1; int mainfn = -1;

int *ic; int icn; int iccap;   /* the internal code */
void emit(int x) {
  int *n; int i;
  if (icn == iccap) {
    iccap = iccap ? iccap * 2 : 65536;
    n = (int *)malloc(iccap * 4);
    for (i = 0; i < icn; i++) n[i] = ic[i];
    ic = n;
  }
  ic[icn++] = x;
}

int streq(int p, int n, char *s) {
  int i;
  if (n != slen(s)) return 0;
  for (i = 0; i < n; i++) if (mod[p + i] != s[i]) return 0;
  return 1;
}

char *hostnames = "fd_read fd_write proc_exit args_sizes_get args_get environ_sizes_get environ_get fd_close fd_fdstat_get fd_seek fd_prestat_get fd_prestat_dir_name ";
int hostid(int p, int n) {
  char *h = hostnames; int id = 1; int i;
  while (*h) {
    i = 0;
    while (h[i] != ' ') i++;
    if (i == n) {
      int j = 0;
      while (j < n && mod[p + j] == h[j]) j++;
      if (j == n) return id;
    }
    h = h + i + 1; id++;
  }
  return 1000;   /* unknown: an error if it is called */
}

long constexpr() {
  int op = u8(); long v = 0;
  if (op == 0x41 || op == 0x42) v = sleb();
  else if (op == 0x44) { v = 0; for (op = 0; op < 8; op++) v = v | (long)u8() << (8 * op); op = 0x44; }
  else if (op == 0x23) v = gval[uleb()];
  else die("unsupported constant expression", op);
  if (u8() != 0x0b) die("bad constant expression", op);
  return v;
}

int bnp; int bnr;   /* set by blocktype */
void blocktype(long bt) {
  if (bt == -64) { bnp = 0; bnr = 0; }
  else if (bt < 0) { bnp = 0; bnr = 1; }
  else { bnp = tnp[(int)bt]; bnr = tnr[(int)bt]; }
}

/* ------------------------------------------------------------ translate */

/* the control stack, while translating a function */
enum { K_BLOCK, K_LOOP, K_IF, K_FUNC };
int ckind[1024]; int cheight[1024]; int carity[1024]; int cnres[1024]; int cstart[1024];
int cparams[1024];
int cpatch[1024];   /* forward branches to fix up: a chain through ic */
int celse[1024];    /* an if's jump to its else, or -1 */
int cd;             /* the top */
int h;              /* the operand stack height */
int unreach;

void pop(int n) { h = h - n; if (h < cheight[cd]) h = cheight[cd]; }
/* the branch to label l: target, arity, height.  The function's own
 * label is the return at its end. */
void branchto(int l) {
  int e = cd - l;
  if (ckind[e] == K_LOOP) { emit(cstart[e]); }
  else { emit(cpatch[e]); cpatch[e] = icn - 1; }
  emit(carity[e]); emit(cheight[e]);
}
void patch(int e) {
  int p = cpatch[e]; int q;
  while (p >= 0) { q = ic[p]; ic[p] = icn; p = q; }
  cpatch[e] = -1;
}

int binop(int op) {   /* is op a binary numeric operator? */
  return (op >= 0x46 && op <= 0x4f) || (op >= 0x51 && op <= 0x66) || (op >= 0x6a && op <= 0x78) ||
         (op >= 0x7c && op <= 0x8a) || (op >= 0x92 && op <= 0x98) || (op >= 0xa0 && op <= 0xa6);
}

void translate(int f, int end) {
  int op; int i; int n; int np; int nr; int t; long v; int ti;
  t = ftype[f];
  cd = 0; ckind[0] = K_FUNC; cheight[0] = 0; carity[0] = tnr[t]; cnres[0] = tnr[t]; cpatch[0] = -1;
  h = 0; unreach = 0;
  fcode[f] = icn;
  while (rp < end) {
    op = u8();
    if (op == 0x02 || op == 0x03 || op == 0x04) {   /* block loop if */
      blocktype(sleb()); np = bnp; nr = bnr;
      if (op == 0x04) { pop(1); emit(0x04); emit(-1); }
      if (cd >= 1023) die("blocks nested too deeply", 0);
      cd++;
      ckind[cd] = op == 0x02 ? K_BLOCK : op == 0x03 ? K_LOOP : K_IF;
      cheight[cd] = h - np; if (cheight[cd] < 0) cheight[cd] = 0;
      carity[cd] = op == 0x03 ? np : nr; cnres[cd] = nr; cstart[cd] = icn; cpatch[cd] = -1;
      cparams[cd] = np;
      celse[cd] = op == 0x04 ? icn - 1 : -1;
      continue;
    }
    if (op == 0x05) {   /* else */
      emit(0x05); emit(cpatch[cd]); cpatch[cd] = icn - 1;
      if (celse[cd] >= 0) ic[celse[cd]] = icn;
      celse[cd] = -1;
      h = cheight[cd] + cparams[cd];
      unreach = 0;
      continue;
    }
    if (op == 0x0b) {   /* end */
      if (cd == 0) {
        patch(0); emit(0x0f);
        if (rp != end) die("code after the end", rp);
        break;
      }
      if (celse[cd] >= 0) ic[celse[cd]] = icn;
      patch(cd);
      h = cheight[cd] + cnres[cd];
      cd--;
      unreach = 0;
      continue;
    }
    if (op == 0x0c || op == 0x0d) {   /* br br_if */
      n = uleb();
      if (op == 0x0d) pop(1);
      emit(op); branchto(n);
      if (op == 0x0c) unreach = 1;
      continue;
    }
    if (op == 0x0e) {   /* br_table */
      n = uleb();
      pop(1);
      emit(0x0e); emit(n);
      for (i = 0; i <= n; i++) {
        ti = uleb();
        branchto(ti);
      }
      unreach = 1;
      continue;
    }
    if (op == 0x0f) { emit(0x0f); unreach = 1; continue; }
    if (op == 0x00) { emit(0x00); unreach = 1; continue; }
    if (op == 0x01) continue;
    if (op == 0x10) {
      n = uleb();
      if (n >= nfuncs) die("bad function index", n);
      pop(tnp[ftype[n]]); h = h + tnr[ftype[n]];
      emit(0x10); emit(n);
      continue;
    }
    if (op == 0x11) {
      n = uleb(); uleb();
      pop(1 + tnp[n]); h = h + tnr[n];
      emit(0x11); emit(n);
      continue;
    }
    if (op == 0x1a) { pop(1); emit(op); continue; }
    if (op == 0x1b || op == 0x1c) {
      if (op == 0x1c) { n = uleb(); rp = rp + n; }
      pop(3); h++; emit(0x1b); continue;
    }
    if (op >= 0x20 && op <= 0x24) {
      n = uleb();
      if (op == 0x20 || op == 0x23) h++;
      else if (op == 0x21 || op == 0x24) pop(1);
      emit(op); emit(n);
      continue;
    }
    if (op >= 0x28 && op <= 0x3e) {   /* loads and stores */
      uleb(); n = uleb();
      if (op <= 0x35) { pop(1); h++; } else pop(2);
      emit(op); emit(n);
      continue;
    }
    if (op == 0x3f || op == 0x40) {
      u8();
      if (op == 0x3f) h++; else { pop(1); h++; }
      emit(op);
      continue;
    }
    if (op == 0x41) { v = sleb(); h++; emit(op); emit((int)v); continue; }
    if (op == 0x42 || op == 0x44) {
      if (op == 0x42) v = sleb();
      else { v = 0; for (i = 0; i < 8; i++) v = v | (long)u8() << (8 * i); }
      h++; emit(op); emit((int)v); emit((int)(v >> 32));
      continue;
    }
    if (op == 0x43) { rp = rp + 4; h++; emit(op); continue; }   /* f32: unsupported when run */
    if (op >= 0x45 && op <= 0xc4) {
      if (binop(op)) { pop(2); h++; } else { pop(1); h++; }
      emit(op);
      continue;
    }
    if (op == 0xfc) {
      n = uleb();
      if (n <= 7) { pop(1); h++; }
      else if (n == 10) { u8(); u8(); pop(3); }
      else if (n == 11) { u8(); pop(3); }
      else die("unsupported instruction 0xfc", n);
      emit(0x100 + n);
      continue;
    }
    die("unsupported instruction", op);
  }
}

/* ----------------------------------------------------------------- parse */

void parse() {
  int id; int size; int n; int i; int j; int l; int p; int kind; int t; int f; int end; int m; int off;
  if (modlen < 8 || mod[0] != 0 || mod[1] != 'a' || mod[2] != 's' || mod[3] != 'm') die("not a wasm module", 0);
  rp = 8;
  while (rp < modlen) {
    id = u8(); size = uleb();
    if (id > 12) die("bad section", id);
    if (id) sec[id] = rp;
    rp = rp + size;
  }
  if (sec[1]) {
    rp = sec[1]; ntypes = uleb();
    tnp = (int *)malloc(ntypes * 4 + 4); tnr = (int *)malloc(ntypes * 4 + 4);
    for (i = 0; i < ntypes; i++) {
      if (u8() != 0x60) die("bad type", i);
      tnp[i] = uleb(); rp = rp + tnp[i];
      tnr[i] = uleb(); rp = rp + tnr[i];
    }
  }
  n = 0;
  if (sec[2]) { rp = sec[2]; n = uleb(); }
  m = 0;
  if (sec[3]) { rp = sec[3]; m = uleb(); }
  nfuncs = n + m;   /* at most */
  ftype = (int *)malloc(nfuncs * 4 + 4); fhost = (int *)malloc(nfuncs * 4 + 4);
  fcode = (int *)malloc(nfuncs * 4 + 4); fnloc = (int *)malloc(nfuncs * 4 + 4);
  if (sec[2]) {
    rp = sec[2]; n = uleb();
    for (i = 0; i < n; i++) {
      l = uleb(); rp = rp + l;
      l = uleb(); p = rp; rp = rp + l;
      kind = u8();
      if (kind != 0) die("unsupported import kind", kind);
      ftype[nimports] = uleb();
      fhost[nimports] = hostid(p, l);
      fcode[nimports] = p; fnloc[nimports] = l;   /* the name, for errors */
      nimports++;
    }
  }
  nfuncs = nimports;
  if (sec[3]) {
    rp = sec[3]; m = uleb();
    for (i = 0; i < m; i++) { ftype[nfuncs] = uleb(); fhost[nfuncs] = 0; nfuncs++; }
  }
  if (sec[4]) {
    rp = sec[4];
    if (uleb() > 1) die("too many tables", 0);
    u8();
    i = u8(); tabsize = uleb(); if (i & 1) uleb();
    tab = (int *)malloc(tabsize * 4 + 4);
    for (i = 0; i < tabsize; i++) tab[i] = -1;
  }
  mmax = 65536;
  if (sec[5]) {
    rp = sec[5];
    if (uleb()) { i = u8(); msize = (long)uleb() << 16; if (i & 1) mmax = uleb(); }
  }
  if (sec[6]) {
    rp = sec[6]; nglobals = uleb();
    gval = (long *)malloc(nglobals * 8 + 8);
    for (i = 0; i < nglobals; i++) { u8(); u8(); gval[i] = constexpr(); }
  }
  if (sec[7]) {
    rp = sec[7]; n = uleb();
    for (i = 0; i < n; i++) {
      l = uleb(); p = rp; rp = rp + l;
      kind = u8(); j = uleb();
      if (kind == 0 && streq(p, l, "_start")) mainfn = j;
    }
  }
  if (sec[8]) { rp = sec[8]; startfn = uleb(); }
  if (sec[9]) {
    rp = sec[9]; n = uleb();
    for (i = 0; i < n; i++) {
      if (uleb() != 0) die("unsupported element segment", i);
      off = (int)constexpr(); m = uleb();
      for (j = 0; j < m; j++) {
        if (off + j >= tabsize) die("element segment out of bounds", off);
        tab[off + j] = uleb();
      }
    }
  }
  if (sec[10]) {
    rp = sec[10]; n = uleb();
    for (i = 0; i < n; i++) {
      size = uleb(); end = rp + size; f = nimports + i;
      m = uleb(); l = 0;
      for (j = 0; j < m; j++) { l = l + uleb(); u8(); }
      fnloc[f] = l;
      translate(f, end);
      rp = end;
    }
  }
  /* the memory comes last, so that it can grow in place */
  mem = malloc((int)msize);
  if (sec[11]) {
    rp = sec[11]; n = uleb();
    for (i = 0; i < n; i++) {
      kind = uleb();
      if (kind == 1) { l = uleb(); rp = rp + l; continue; }
      if (kind != 0) die("unsupported data segment", kind);
      off = (int)constexpr(); l = uleb();
      if ((long)(unsigned)off + l > msize) die("data segment out of bounds", off);
      for (j = 0; j < l; j++) mem[off + j] = mod[rp + j];
      rp = rp + l;
    }
  }
}

/* -------------------------------------------------------------- execute */

long vs[VSZ]; int sp;   /* the value stack */
int sff[FSZ]; int sfpc[FSZ]; int sffp[FSZ]; int sfob[FSZ]; int nfr;   /* the frame stack */

void trap(char *msg) { eputs("wasm: trap: "); eputs(msg); eputs("\n"); __builtin_trap(); }

/* the address of a guest memory access of size n */
char *at(long e, int n) {
  if (e + n > msize) trap("out of bounds memory access");
  return mem + (int)e;
}

int iovs[2 * MAXIOV];
int hostio(int id, int fd, int iov, int n, int out) {
  int i; int p; int l; int r;
  if (n > MAXIOV) n = MAXIOV;
  for (i = 0; i < n; i++) {
    p = *(int *)at((unsigned)iov + 8 * i, 4); l = *(int *)at((unsigned)iov + 8 * i + 4, 4);
    at((unsigned)p, 0); at((long)(unsigned)p + (unsigned)l, 0);
    iovs[2 * i] = (int)(mem + p); iovs[2 * i + 1] = l;
  }
  if (id == 1) r = fd_read(fd, (int)iovs, n, (int)(iovs + 2 * MAXIOV - 1));
  else r = fd_write(fd, (int)iovs, n, (int)(iovs + 2 * MAXIOV - 1));
  *(int *)at((unsigned)out, 4) = iovs[2 * MAXIOV - 1];
  return r;
}
void host(int f) {
  int t = ftype[f]; int a0; int a1; int a2; int a3; int r = 0; int id = fhost[f];
  sp = sp - tnp[t];
  a0 = (int)vs[sp]; a1 = (int)vs[sp + 1]; a2 = (int)vs[sp + 2]; a3 = (int)vs[sp + 3];
  if (id == 1 || id == 2) r = hostio(id, a0, a1, a2, a3);
  else if (id == 3) proc_exit(a0);
  else if (id == 4 || id == 6) { *(int *)at((unsigned)a0, 4) = 0; *(int *)at((unsigned)a1, 4) = 0; }
  else if (id == 5 || id == 7 || id == 8) r = 0;
  else if (id == 9) { for (r = 0; r < 24; r++) *at((unsigned)a1 + r, 1) = 0; *at((unsigned)a1, 1) = 2; r = 0; }
  else if (id == 10) r = 70;   /* ESPIPE */
  else if (id == 11 || id == 12) r = 8;   /* EBADF: no preopened directories */
  else {
    eputs("wasm: unsupported import ");
    sys_write(2, mod + fcode[f], fnloc[f]);
    trap("");
  }
  if (tnr[t]) vs[sp++] = r;
}

void run(int entry) {
  int f = entry; int pc; int fp; int ob; int op; int i; int n; int x; int y; int base = nfr;
  long a; long b; long e; double d; double g; int *c;
  if (fhost[f]) { host(f); return; }
  fp = sp - tnp[ftype[f]];
  ob = sp + fnloc[f];
  for (i = sp; i < ob; i++) vs[i] = 0;
  sp = ob; pc = fcode[f];
  while (1) {
    c = ic;
    op = c[pc++];
    switch (op) {
    case 0x00: trap("unreachable");
    case 0x04:   /* if: jump if false */
      if ((int)vs[--sp]) pc++; else pc = c[pc];
      break;
    case 0x05: pc = c[pc]; break;
    case 0x0d:
      if (!(int)vs[--sp]) { pc = pc + 3; break; }
      /* fall through: br */
    case 0x0c:
      n = c[pc + 1]; e = ob + c[pc + 2];
      for (i = 0; i < n; i++) vs[(int)e + i] = vs[sp - n + i];
      sp = (int)e + n;
      pc = c[pc];
      break;
    case 0x0e:
      n = c[pc++];
      x = (int)vs[--sp];
      if ((unsigned)x > (unsigned)n) x = n;
      pc = pc + 3 * x;
      n = c[pc + 1]; e = ob + c[pc + 2];
      for (i = 0; i < n; i++) vs[(int)e + i] = vs[sp - n + i];
      sp = (int)e + n;
      pc = c[pc];
      break;
    case 0x0f:
      n = tnr[ftype[f]];
      for (i = 0; i < n; i++) vs[fp + i] = vs[sp - n + i];
      sp = fp + n;
      if (nfr == base) return;
      nfr--;
      f = sff[nfr]; pc = sfpc[nfr]; fp = sffp[nfr]; ob = sfob[nfr];
      break;
    case 0x10: case 0x11:
      if (op == 0x10) n = c[pc++];
      else {
        x = (int)vs[--sp];
        if ((unsigned)x >= (unsigned)tabsize) trap("undefined table element");
        n = tab[x];
        if (n < 0) trap("uninitialized table element");
        if (tnp[ftype[n]] != tnp[c[pc]] || tnr[ftype[n]] != tnr[c[pc]]) trap("indirect call type mismatch");
        pc++;
      }
      if (fhost[n]) { host(n); break; }
      if (nfr >= FSZ) trap("call stack exhausted");
      sff[nfr] = f; sfpc[nfr] = pc; sffp[nfr] = fp; sfob[nfr] = ob; nfr++;
      f = n;
      fp = sp - tnp[ftype[f]];
      ob = sp + fnloc[f];
      if (ob + 4096 > VSZ) trap("value stack exhausted");
      for (i = sp; i < ob; i++) vs[i] = 0;
      sp = ob; pc = fcode[f];
      break;
    case 0x1a: sp--; break;
    case 0x1b: sp = sp - 2; if (!(int)vs[sp + 1]) vs[sp - 1] = vs[sp]; break;
    case 0x20: vs[sp++] = vs[fp + c[pc++]]; break;
    case 0x21: vs[fp + c[pc++]] = vs[--sp]; break;
    case 0x22: vs[fp + c[pc++]] = vs[sp - 1]; break;
    case 0x23: vs[sp++] = gval[c[pc++]]; break;
    case 0x24: gval[c[pc++]] = vs[--sp]; break;

    /* memory: e is the effective address */
    case 0x28: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *(int *)at(e, 4); break;
    case 0x29: case 0x2b: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *(long *)at(e, 8); break;
    case 0x2c: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *at(e, 1); break;
    case 0x2d: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *(unsigned char *)at(e, 1); break;
    case 0x2e: case 0x2f: case 0x32: case 0x33:
      e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++];
      x = (*(unsigned char *)at(e, 2)) | (*(unsigned char *)at(e + 1, 1)) << 8;
      if (op == 0x2e || op == 0x32) x = (x << 16) >> 16;
      vs[sp - 1] = x;
      break;
    case 0x30: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *at(e, 1); break;
    case 0x31: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *(unsigned char *)at(e, 1); break;
    case 0x34: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = *(int *)at(e, 4); break;
    case 0x35: e = (unsigned)(int)vs[sp - 1] + (unsigned)c[pc++]; vs[sp - 1] = (unsigned)*(int *)at(e, 4); break;
    case 0x36: case 0x3e: sp = sp - 2; e = (unsigned)(int)vs[sp] + (unsigned)c[pc++]; *(int *)at(e, 4) = (int)vs[sp + 1]; break;
    case 0x37: case 0x39: sp = sp - 2; e = (unsigned)(int)vs[sp] + (unsigned)c[pc++]; *(long *)at(e, 8) = vs[sp + 1]; break;
    case 0x3a: case 0x3c: sp = sp - 2; e = (unsigned)(int)vs[sp] + (unsigned)c[pc++]; *at(e, 1) = (char)vs[sp + 1]; break;
    case 0x3b: case 0x3d:
      sp = sp - 2; e = (unsigned)(int)vs[sp] + (unsigned)c[pc++];
      x = (int)vs[sp + 1];
      *at(e, 2) = (char)x; *at(e + 1, 1) = (char)(x >> 8);
      break;
    case 0x3f: vs[sp++] = msize >> 16; break;
    case 0x40:
      x = (int)vs[sp - 1];
      a = msize >> 16;
      if ((unsigned)x > mmax - a || (unsigned)x > 32768) { vs[sp - 1] = -1; break; }
      if (x) {
        if (malloc(x * 65536) != mem + (int)msize) { vs[sp - 1] = -1; break; }
        msize = msize + ((long)x << 16);
      }
      vs[sp - 1] = a;
      break;

    case 0x41: vs[sp++] = c[pc++]; break;
    case 0x42: case 0x44: vs[sp++] = (unsigned)c[pc] | (long)c[pc + 1] << 32; pc = pc + 2; break;

    /* i32 */
    case 0x45: vs[sp - 1] = !(int)vs[sp - 1]; break;
    case 0x46: sp--; vs[sp - 1] = (int)vs[sp - 1] == (int)vs[sp]; break;
    case 0x47: sp--; vs[sp - 1] = (int)vs[sp - 1] != (int)vs[sp]; break;
    case 0x48: sp--; vs[sp - 1] = (int)vs[sp - 1] < (int)vs[sp]; break;
    case 0x49: sp--; vs[sp - 1] = (unsigned)vs[sp - 1] < (unsigned)vs[sp]; break;
    case 0x4a: sp--; vs[sp - 1] = (int)vs[sp - 1] > (int)vs[sp]; break;
    case 0x4b: sp--; vs[sp - 1] = (unsigned)vs[sp - 1] > (unsigned)vs[sp]; break;
    case 0x4c: sp--; vs[sp - 1] = (int)vs[sp - 1] <= (int)vs[sp]; break;
    case 0x4d: sp--; vs[sp - 1] = (unsigned)vs[sp - 1] <= (unsigned)vs[sp]; break;
    case 0x4e: sp--; vs[sp - 1] = (int)vs[sp - 1] >= (int)vs[sp]; break;
    case 0x4f: sp--; vs[sp - 1] = (unsigned)vs[sp - 1] >= (unsigned)vs[sp]; break;
    /* i64 */
    case 0x50: vs[sp - 1] = !vs[sp - 1]; break;
    case 0x51: sp--; vs[sp - 1] = vs[sp - 1] == vs[sp]; break;
    case 0x52: sp--; vs[sp - 1] = vs[sp - 1] != vs[sp]; break;
    case 0x53: sp--; vs[sp - 1] = vs[sp - 1] < vs[sp]; break;
    case 0x54: sp--; vs[sp - 1] = (unsigned long)vs[sp - 1] < (unsigned long)vs[sp]; break;
    case 0x55: sp--; vs[sp - 1] = vs[sp - 1] > vs[sp]; break;
    case 0x56: sp--; vs[sp - 1] = (unsigned long)vs[sp - 1] > (unsigned long)vs[sp]; break;
    case 0x57: sp--; vs[sp - 1] = vs[sp - 1] <= vs[sp]; break;
    case 0x58: sp--; vs[sp - 1] = (unsigned long)vs[sp - 1] <= (unsigned long)vs[sp]; break;
    case 0x59: sp--; vs[sp - 1] = vs[sp - 1] >= vs[sp]; break;
    case 0x5a: sp--; vs[sp - 1] = (unsigned long)vs[sp - 1] >= (unsigned long)vs[sp]; break;
    /* f64 comparisons */
    case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:
      sp--; d = __f64_from_bits(vs[sp - 1]); g = __f64_from_bits(vs[sp]);
      if (op == 0x61) x = d == g; else if (op == 0x62) x = d != g; else if (op == 0x63) x = d < g;
      else if (op == 0x64) x = d > g; else if (op == 0x65) x = d <= g; else x = d >= g;
      vs[sp - 1] = x;
      break;
    case 0x67: vs[sp - 1] = __builtin_clz((int)vs[sp - 1]); break;
    case 0x68: vs[sp - 1] = __builtin_ctz((int)vs[sp - 1]); break;
    case 0x69: vs[sp - 1] = __builtin_popcount((int)vs[sp - 1]); break;
    case 0x6a: sp--; vs[sp - 1] = (int)vs[sp - 1] + (int)vs[sp]; break;
    case 0x6b: sp--; vs[sp - 1] = (int)vs[sp - 1] - (int)vs[sp]; break;
    case 0x6c: sp--; vs[sp - 1] = (int)vs[sp - 1] * (int)vs[sp]; break;
    case 0x6d: sp--; vs[sp - 1] = (int)vs[sp - 1] / (int)vs[sp]; break;
    case 0x6e: sp--; vs[sp - 1] = (int)((unsigned)vs[sp - 1] / (unsigned)vs[sp]); break;
    case 0x6f: sp--; vs[sp - 1] = (int)vs[sp - 1] % (int)vs[sp]; break;
    case 0x70: sp--; vs[sp - 1] = (int)((unsigned)vs[sp - 1] % (unsigned)vs[sp]); break;
    case 0x71: sp--; vs[sp - 1] = (int)vs[sp - 1] & (int)vs[sp]; break;
    case 0x72: sp--; vs[sp - 1] = (int)vs[sp - 1] | (int)vs[sp]; break;
    case 0x73: sp--; vs[sp - 1] = (int)vs[sp - 1] ^ (int)vs[sp]; break;
    case 0x74: sp--; vs[sp - 1] = (int)vs[sp - 1] << (int)vs[sp]; break;
    case 0x75: sp--; vs[sp - 1] = (int)vs[sp - 1] >> (int)vs[sp]; break;
    case 0x76: sp--; vs[sp - 1] = (int)((unsigned)vs[sp - 1] >> (int)vs[sp]); break;
    case 0x77: case 0x78:
      sp--; x = (int)vs[sp - 1]; y = (int)vs[sp] & 31;
      if (op == 0x78) y = (32 - y) & 31;
      vs[sp - 1] = y ? x << y | (int)((unsigned)x >> (32 - y)) : x;
      break;
    case 0x79: vs[sp - 1] = __builtin_clzll(vs[sp - 1]); break;
    case 0x7a: vs[sp - 1] = __builtin_ctzll(vs[sp - 1]); break;
    case 0x7b: vs[sp - 1] = __builtin_popcountll(vs[sp - 1]); break;
    case 0x7c: sp--; vs[sp - 1] = vs[sp - 1] + vs[sp]; break;
    case 0x7d: sp--; vs[sp - 1] = vs[sp - 1] - vs[sp]; break;
    case 0x7e: sp--; vs[sp - 1] = vs[sp - 1] * vs[sp]; break;
    case 0x7f: sp--; vs[sp - 1] = vs[sp - 1] / vs[sp]; break;
    case 0x80: sp--; vs[sp - 1] = (long)((unsigned long)vs[sp - 1] / (unsigned long)vs[sp]); break;
    case 0x81: sp--; vs[sp - 1] = vs[sp - 1] % vs[sp]; break;
    case 0x82: sp--; vs[sp - 1] = (long)((unsigned long)vs[sp - 1] % (unsigned long)vs[sp]); break;
    case 0x83: sp--; vs[sp - 1] = vs[sp - 1] & vs[sp]; break;
    case 0x84: sp--; vs[sp - 1] = vs[sp - 1] | vs[sp]; break;
    case 0x85: sp--; vs[sp - 1] = vs[sp - 1] ^ vs[sp]; break;
    case 0x86: sp--; vs[sp - 1] = vs[sp - 1] << vs[sp]; break;
    case 0x87: sp--; vs[sp - 1] = vs[sp - 1] >> vs[sp]; break;
    case 0x88: sp--; vs[sp - 1] = (long)((unsigned long)vs[sp - 1] >> vs[sp]); break;
    case 0x89: case 0x8a:
      sp--; a = vs[sp - 1]; y = (int)vs[sp] & 63;
      if (op == 0x8a) y = (64 - y) & 63;
      vs[sp - 1] = y ? a << y | (long)((unsigned long)a >> (64 - y)) : a;
      break;
    /* f64 */
    case 0x99: vs[sp - 1] = __f64_bits(__builtin_fabs(__f64_from_bits(vs[sp - 1]))); break;
    case 0x9a: vs[sp - 1] = __f64_bits(-__f64_from_bits(vs[sp - 1])); break;
    case 0x9b: vs[sp - 1] = __f64_bits(__builtin_ceil(__f64_from_bits(vs[sp - 1]))); break;
    case 0x9c: vs[sp - 1] = __f64_bits(__builtin_floor(__f64_from_bits(vs[sp - 1]))); break;
    case 0x9d: vs[sp - 1] = __f64_bits(__builtin_trunc(__f64_from_bits(vs[sp - 1]))); break;
    case 0x9e: vs[sp - 1] = __f64_bits(__builtin_nearbyint(__f64_from_bits(vs[sp - 1]))); break;
    case 0x9f: vs[sp - 1] = __f64_bits(__builtin_sqrt(__f64_from_bits(vs[sp - 1]))); break;
    case 0xa0: sp--; vs[sp - 1] = __f64_bits(__f64_from_bits(vs[sp - 1]) + __f64_from_bits(vs[sp])); break;
    case 0xa1: sp--; vs[sp - 1] = __f64_bits(__f64_from_bits(vs[sp - 1]) - __f64_from_bits(vs[sp])); break;
    case 0xa2: sp--; vs[sp - 1] = __f64_bits(__f64_from_bits(vs[sp - 1]) * __f64_from_bits(vs[sp])); break;
    case 0xa3: sp--; vs[sp - 1] = __f64_bits(__f64_from_bits(vs[sp - 1]) / __f64_from_bits(vs[sp])); break;
    case 0xa4: sp--; vs[sp - 1] = __f64_bits(__builtin_fmin(__f64_from_bits(vs[sp - 1]), __f64_from_bits(vs[sp]))); break;
    case 0xa5: sp--; vs[sp - 1] = __f64_bits(__builtin_fmax(__f64_from_bits(vs[sp - 1]), __f64_from_bits(vs[sp]))); break;
    case 0xa6: sp--; vs[sp - 1] = __f64_bits(__builtin_copysign(__f64_from_bits(vs[sp - 1]), __f64_from_bits(vs[sp]))); break;
    /* conversions */
    case 0xa7: vs[sp - 1] = (int)vs[sp - 1]; break;
    case 0xaa: vs[sp - 1] = (int)__f64_from_bits(vs[sp - 1]); break;
    case 0xab: vs[sp - 1] = (int)(unsigned)__f64_from_bits(vs[sp - 1]); break;
    case 0xac: vs[sp - 1] = (int)vs[sp - 1]; break;
    case 0xad: vs[sp - 1] = (unsigned)vs[sp - 1]; break;
    case 0xb0: vs[sp - 1] = (long)__f64_from_bits(vs[sp - 1]); break;
    case 0xb1: vs[sp - 1] = (long)(unsigned long)__f64_from_bits(vs[sp - 1]); break;
    case 0xb7: vs[sp - 1] = __f64_bits((double)(int)vs[sp - 1]); break;
    case 0xb8: vs[sp - 1] = __f64_bits((double)(unsigned)vs[sp - 1]); break;
    case 0xb9: vs[sp - 1] = __f64_bits((double)vs[sp - 1]); break;
    case 0xba: vs[sp - 1] = __f64_bits((double)(unsigned long)vs[sp - 1]); break;
    case 0xbd: case 0xbf: break;   /* reinterpret: the bits are the bits */
    case 0xc0: vs[sp - 1] = (char)vs[sp - 1]; break;
    case 0xc1: x = (int)vs[sp - 1]; vs[sp - 1] = (x << 16) >> 16; break;
    case 0xc2: vs[sp - 1] = (vs[sp - 1] << 56) >> 56; break;
    case 0xc3: vs[sp - 1] = (vs[sp - 1] << 48) >> 48; break;
    case 0xc4: vs[sp - 1] = (int)vs[sp - 1]; break;
    /* saturating conversions (0xfc 2, 3, 6, 7 are from f64) */
    case 0x102: case 0x103: case 0x106: case 0x107:
      d = __f64_from_bits(vs[sp - 1]);
      g = (double)(1L << 62) * 2;   /* 2^63 */
      if (d != d) a = 0;
      else if (op == 0x102) a = d <= (double)(-2147483647 - 1) - 1 ? -2147483647 - 1 : d >= (double)2147483647 + 1 ? 2147483647 : (int)d;
      else if (op == 0x103) a = d <= -1 ? 0 : d >= (double)4294967295U + 1 ? -1 : (int)(unsigned)d;
      else if (op == 0x106) a = d < -g ? -9223372036854775807L - 1 : d >= g ? 9223372036854775807L : (long)d;
      else a = d <= -1 ? 0 : d >= g * 2 ? -1 : (long)(unsigned long)d;
      vs[sp - 1] = a;
      break;
    case 0x10a:   /* memory.copy */
      sp = sp - 3; a = (unsigned)(int)vs[sp]; b = (unsigned)(int)vs[sp + 1]; e = (unsigned)(int)vs[sp + 2];
      at(a + e, 0); at(b + e, 0);
      if (a < b) for (i = 0; i < (int)e; i++) mem[(int)a + i] = mem[(int)b + i];
      else for (i = (int)e - 1; i >= 0; i--) mem[(int)a + i] = mem[(int)b + i];
      break;
    case 0x10b:   /* memory.fill */
      sp = sp - 3; a = (unsigned)(int)vs[sp]; x = (int)vs[sp + 1]; e = (unsigned)(int)vs[sp + 2];
      at(a + e, 0);
      for (i = 0; i < (int)e; i++) mem[(int)a + i] = (char)x;
      break;
    default:
      die("unsupported instruction", op);
    }
  }
}

/* ------------------------------------------------------------------ main */

char lenbuf[4];
int readn(char *buf, int n) {
  int got = 0; int r;
  while (got < n) {
    r = sys_read(buf + got, n - got);
    if (r <= 0) die("unexpected end of input", got);
    got = got + r;
  }
  return got;
}

int main() {
  int n = 0;
  while (1) {
    readn(lenbuf, 1);
    if (lenbuf[0] == '\n') break;
    if (lenbuf[0] >= '0' && lenbuf[0] <= '9') n = n * 10 + lenbuf[0] - '0';
  }
  modlen = n;
  mod = malloc(n + 8);
  readn(mod, n);
  parse();
  if (startfn >= 0) run(startfn);
  if (mainfn >= 0) run(mainfn);
  return 0;
}
