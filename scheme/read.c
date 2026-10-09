/* read.c -- the Scheme reader, shared by the compiler (to read programs)
 * and the runtime (for read).  Reads from stdin.  Needs rt.c. */

char ibuf[65536]; int ipos; int ilen; int ieof;
int iline = 1;
int igetc() {
  int c;
  if (ipos == ilen) {
    if (ieof) return -1;
    ilen = sys_read(ibuf, 65536); ipos = 0;
    if (ilen <= 0) { ilen = 0; ieof = 1; return -1; }
  }
  c = ibuf[ipos++] & 255;
  if (c == '\n') iline++;
  return c;
}
int ipeek() {
  int c = igetc();
  if (c >= 0) { ipos--; if (c == '\n') iline--; }
  return c;
}

long S_quote; long S_quasiquote; long S_unquote; long S_unqspl;
enum { RD_CLOSE = 12, RD_DOT = 14 };   /* not data: what read1 found instead */

void rderr(char *msg) {
  char *b = malloc(16); int i = 15; int n = iline;
  b[i] = 0;
  do { b[--i] = '0' + n % 10; n = n / 10; } while (n);
  flush();
  eputs("read error (line "); eputs(b + i); eputs("): "); eputs(msg); eputs("\n");
  proc_exit(1);
}

int isdelim(int c) {
  return c < 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 12 ||
         c == '(' || c == ')' || c == '[' || c == ']' || c == '"' || c == ';';
}
int digitval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

/* a token as a number, or FALSE */
long parsenum(char *p, int n, int radix) {
  int i = 0; int neg = 0; long v = 0; int d; int j; double f; long num; long den;
  if (n == 0) return FALSE;
  if (n == 6 && (memeq(p, "+inf.0", 6) || memeq(p, "-inf.0", 6)))
    return mkdbl((p[0] == '-' ? -(double)1 : (double)1) / (double)0);
  if (n == 6 && (memeq(p, "+nan.0", 6) || memeq(p, "-nan.0", 6))) return mkdbl((double)0 / (double)0);
  if (p[0] == '-' || p[0] == '+') { neg = p[0] == '-'; i = 1; }
  if (i == n) return FALSE;
  j = i;
  while (j < n && digitval(p[j]) < radix) j++;
  if (j == n) {   /* an integer */
    while (i < n) {
      d = digitval(p[i]);
      if (v > (9223372036854775807L - d) / radix) die("integer literal too big (no bignums): ", p);
      v = v * radix + d; i++;
    }
    return mkint(neg ? -v : v);
  }
  if (j > i && p[j] == '/') {   /* a ratio: exact only if it divides */
    num = ival(parsenum(p, j, radix));
    den = parsenum(p + j + 1, n - j - 1, radix);
    if (den == FALSE || !isint(den)) return FALSE;
    den = ival(den);
    if (den == 0 || num % den != 0) die("rational numbers are not supported: ", p);
    return mkint(num / den);
  }
  if (radix != 10) return FALSE;
  /* a decimal: must start like a number */
  if (!((p[i] >= '0' && p[i] <= '9') || (p[i] == '.' && i + 1 < n && p[i + 1] >= '0' && p[i + 1] <= '9'))) return FALSE;
  f = str2dbl(p, n);
  if (!numok) return FALSE;
  return mkdbl(f);
}

/* the rest of a token, after its first character c, into sb */
void rdtoken(int c) {
  sbn = 0;
  sbput(c);
  while (!isdelim(ipeek())) sbput(igetc());
  sbneed(1); sb[sbn] = 0;
}

long read1();
long readdatum() {
  long x = read1();
  if (x == RD_CLOSE) rderr("unexpected )");
  if (x == RD_DOT) rderr("unexpected .");
  return x;
}
long rdlist(int close) {
  long head = NULL; long tail = NULL; long x; long cell;
  while (1) {
    x = read1();
    if (x == EOFV) rderr("unexpected end of input in a list");
    if (x == RD_CLOSE) return head;
    if (x == RD_DOT) {
      if (head == NULL) rderr("bad dotted list");
      x = readdatum();
      *(long *)((int)tail + 16) = x;
      if (read1() != RD_CLOSE) rderr("bad dotted list");
      return head;
    }
    cell = cons(x, NULL);
    if (head == NULL) head = cell; else *(long *)((int)tail + 16) = cell;
    tail = cell;
  }
  return head;
}
long list2vec(long l) {
  int n = 0; long p = l; long v; int i;
  while (ispair(p)) { n++; p = cdr_(p); }
  v = mkvec(n, NIL);
  for (i = 0; i < n; i++) { velts(v)[i] = car_(l); l = cdr_(l); }
  return v;
}

int rdhex() {
  int c; int v = 0;
  while (1) {
    c = igetc();
    if (c == ';' || c < 0) return v;
    if (digitval(c) >= 16) rderr("bad \\x escape");
    v = v * 16 + digitval(c);
  }
  return v;
}
long rdstring() {
  int c;
  sbn = 0;
  while (1) {
    c = igetc();
    if (c < 0) rderr("unterminated string");
    if (c == '"') break;
    if (c == '\\') {
      c = igetc();
      if (c == 'n') c = '\n';
      else if (c == 't') c = '\t';
      else if (c == 'r') c = '\r';
      else if (c == 'a') c = 7;
      else if (c == 'b') c = 8;
      else if (c == '0') c = 0;
      else if (c == 'x') c = rdhex();
      else if (c == '\n' || c == ' ' || c == '\t') {   /* line continuation */
        while (c == ' ' || c == '\t') c = igetc();
        while (ipeek() == ' ' || ipeek() == '\t') igetc();
        continue;
      }
    }
    sbput(c);
  }
  return sbstr();
}

int namedchar(char *p, int n) {
  if (n == 5 && memeq(p, "space", 5)) return ' ';
  if (n == 7 && memeq(p, "newline", 7)) return '\n';
  if (n == 8 && memeq(p, "linefeed", 8)) return '\n';
  if (n == 3 && memeq(p, "tab", 3)) return '\t';
  if (n == 3 && memeq(p, "nul", 3)) return 0;
  if (n == 4 && memeq(p, "null", 4)) return 0;
  if (n == 6 && memeq(p, "return", 6)) return '\r';
  if (n == 6 && memeq(p, "escape", 6)) return 27;
  if (n == 7 && memeq(p, "altmode", 7)) return 27;
  if (n == 9 && memeq(p, "backspace", 9)) return 8;
  if (n == 6 && memeq(p, "delete", 6)) return 127;
  if (n == 6 && memeq(p, "rubout", 6)) return 127;
  if (n == 5 && memeq(p, "alarm", 5)) return 7;
  return -1;
}

long read1() {
  int c; int d; int depth; long x;
  while (1) {
    c = igetc();
    if (c < 0) return EOFV;
    if (c == ';') { while (c >= 0 && c != '\n') c = igetc(); continue; }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 12) continue;
    if (c == '(' || c == '[') return rdlist(c);
    if (c == ')' || c == ']') return RD_CLOSE;
    if (c == '\'') return cons(S_quote, cons(readdatum(), NULL));
    if (c == '`') return cons(S_quasiquote, cons(readdatum(), NULL));
    if (c == ',') {
      if (ipeek() == '@') { igetc(); return cons(S_unqspl, cons(readdatum(), NULL)); }
      return cons(S_unquote, cons(readdatum(), NULL));
    }
    if (c == '"') return rdstring();
    if (c == '|') {
      sbn = 0;
      while ((c = igetc()) != '|') { if (c < 0) rderr("unterminated |symbol|"); sbput(c); }
      x = intern(sb, sbn); sbn = 0;
      return x;
    }
    if (c == '#') {
      c = igetc();
      if (c == '|') {   /* block comment, nested */
        depth = 1; d = 0;
        while (depth > 0) {
          c = igetc();
          if (c < 0) rderr("unterminated #| comment");
          if (d == '|' && c == '#') { depth--; c = 0; }
          else if (d == '#' && c == '|') { depth++; c = 0; }
          d = c;
        }
        continue;
      }
      if (c == ';') { readdatum(); continue; }
      if (c == '(') return list2vec(rdlist(')'));
      if (c == '\\') {
        c = igetc();
        if (isdelim(ipeek())) return mkchar(c);
        rdtoken(c);
        if ((sb[0] == 'x' || sb[0] == 'X') && sbn > 1) {
          d = 0; c = 1;
          while (c < sbn && digitval(sb[c]) < 16) { d = d * 16 + digitval(sb[c]); c++; }
          if (c == sbn) return mkchar(d);
        }
        d = namedchar(sb, sbn);
        if (d < 0) { sbput(0); rderr("unknown character name"); }
        return mkchar(d);
      }
      if (c == '!') { while (c >= 0 && c != '\n') c = igetc(); continue; }   /* #!fold-case etc. */
      rdtoken(c);
      if (sb[0] == 't' && (sbn == 1 || (sbn == 4 && memeq(sb, "true", 4)))) return TRUE;
      if (sb[0] == 'f' && (sbn == 1 || (sbn == 5 && memeq(sb, "false", 5)))) return FALSE;
      d = 0;
      if (sb[0] == 'x' || sb[0] == 'X') d = 16;
      if (sb[0] == 'b' || sb[0] == 'B') d = 2;
      if (sb[0] == 'o' || sb[0] == 'O') d = 8;
      if (sb[0] == 'd' || sb[0] == 'D' || sb[0] == 'e' || sb[0] == 'E') d = 10;
      if (d) {
        x = parsenum(sb + 1, sbn - 1, d);
        if (x == FALSE) rderr("bad number");
        sbn = 0;
        return x;
      }
      if (sb[0] == 'i' || sb[0] == 'I') {
        x = parsenum(sb + 1, sbn - 1, 10);
        if (x == FALSE) rderr("bad number");
        sbn = 0;
        return isdbl(x) ? x : mkdbl((double)ival(x));
      }
      sbput(0);
      rderr("unknown # syntax");
    }
    rdtoken(c);
    if (sbn == 1 && sb[0] == '.') { sbn = 0; return RD_DOT; }
    x = parsenum(sb, sbn, 10);
    if (x == FALSE) x = intern(sb, sbn);
    sbn = 0;
    return x;
  }
  return EOFV;
}

void read_init() {
  S_quote = sym("quote"); S_quasiquote = sym("quasiquote");
  S_unquote = sym("unquote"); S_unqspl = sym("unquote-splicing");
}
