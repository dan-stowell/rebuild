/* scc.c -- a Scheme compiler: Scheme -> C, which cc compiles to wasm.
 * Needs libc.c, num.c, rt.c and read.c in front: the program is read with
 * the runtime's own reader, into the runtime's own pairs and symbols.
 *
 *   expand    derived forms (let, cond, do, case, quasiquote, records, ...)
 *             become a small core: constants, variable references and
 *             assignments, if, sequences, lambda, calls, primitive calls,
 *             let, and fix (letrec of lambdas)
 *   loops     a named let whose name is only called in tail position
 *             becomes a C loop
 *   closures  which variables each function captures; captured variables
 *             that are assigned live in cells
 *   generate  one C function per lambda, int F(int clo, int n), with the
 *             arguments in stk (see rt.c)
 */

void numerr(char *what, long v) { die("bad number", ""); }

/* ------------------------------------------------------- lists, errors */

long car(long x) { return car_(x); }
long cdr(long x) { return cdr_(x); }
long cadr(long x) { return car_(cdr_(x)); }
long cddr(long x) { return cdr_(cdr_(x)); }
long caddr(long x) { return car_(cdr_(cdr_(x))); }
long cdddr(long x) { return cdr_(cdr_(cdr_(x))); }
long L1(long a) { return cons(a, NULL); }
long L2(long a, long b) { return cons(a, cons(b, NULL)); }
long L3(long a, long b, long c) { return cons(a, cons(b, cons(c, NULL))); }
long L4(long a, long b, long c, long d) { return cons(a, cons(b, cons(c, cons(d, NULL)))); }
int len(long l) { int n = 0; while (ispair(l)) { n++; l = cdr_(l); } return n; }
long rev(long l) { long r = NULL; while (ispair(l)) { r = cons(car_(l), r); l = cdr_(l); } return r; }
long append2(long a, long b) { if (a == NULL) return b; return cons(car(a), append2(cdr(a), b)); }
int issym(long x) { return otype(x) == OSYM; }
int symis(long x, char *s) { return otype(x) == OSYM && sl(x) == slen(s) && memeq(sptr(x), s, sl(x)); }

void sbdatum(long v) {
  int t = otype(v); int i;
  if (v == NIL) sbcstr("#<unspecified>");
  else if (v == FALSE) sbcstr("#f");
  else if (v == TRUE) sbcstr("#t");
  else if (v == NULL) sbcstr("()");
  else if (isint(v)) sbint(ival(v));
  else if (isdbl(v)) sbdblshort(dval(v));
  else if (ischar(v)) { sbcstr("#\\"); sbput(charval(v)); }
  else if (t == OSYM) sbputs(sptr(v), sl(v));
  else if (t == OSTR) { sbput('"'); sbputs(sptr(v), sl(v)); sbput('"'); }
  else if (t == OPAIR) {
    sbput('(');
    while (1) {
      sbdatum(car_(v)); v = cdr_(v);
      if (!ispair(v)) break;
      sbput(' ');
      if (sbn > 400) { sbcstr("..."); v = NULL; break; }
    }
    if (v != NULL) { sbcstr(" . "); sbdatum(v); }
    sbput(')');
  } else if (t == OVEC) {
    sbcstr("#(");
    for (i = 0; i < vlen(v); i++) { if (i) sbput(' '); sbdatum(velts(v)[i]); }
    sbput(')');
  } else sbcstr("#<object>");
}
void synerr(char *msg, long form) {
  flush();
  sbn = 0;
  sbcstr("scc: "); sbcstr(msg); sbcstr(": ");
  sbdatum(form);
  sbput('\n');
  sys_write(2, sb, sbn);
  proc_exit(1);
}
long p_vec2list(long v) { long l = NULL; int i; for (i = vlen(v) - 1; i >= 0; i--) l = cons(velts(v)[i], l); return l; }
long need(long x, long form) { if (!ispair(x)) synerr("bad syntax", form); return x; }

/* ----------------------------------------------------- the symbol table */

long S_lambda; long S_define; long S_set; long S_if; long S_begin; long S_let;
long S_letstar; long S_letrec; long S_letrecstar; long S_cond; long S_case;
long S_and; long S_or; long S_when; long S_unless; long S_do; long S_else;
long S_arrow; long S_delay; long S_delayforce; long S_defrecord; long S_letvalues;
long S_letstarvalues; long S_defvalues; long S_receive; long S_caselambda;
long S_import; long S_defsyntax; long S_letsyntax; long S_letrecsyntax;
long S_syntaxrules; long S_ellipsis; long S_underscore; long S_guard;
long S_parameterize; long S_definelibrary; long S_defmacro;

int ngensym;
long gensym() {
  sbn = 0; sbcstr(" g"); sbint(++ngensym);
  { long s = intern(sb, sbn); sbn = 0; return s; }
}

/* variables */
enum { MAXV = 200000 };
long vname[MAXV];
int vglobal[MAXV];   /* 1 global, 0 local */
int vnref[MAXV];     /* references */
int vnset[MAXV];     /* assignments (a global's define counts) */
int vlam[MAXV];      /* the lambda it is bound to, if it is never reassigned */
int vcfun[MAXV];     /* the C function it lives in */
int vcaptured[MAXV];
long vprim[MAXV];    /* a global standing for a primitive used as a value: its name */
int vundef[MAXV];    /* a global the program never defines */
int vmark[MAXV];
int nvars = 1;
int newvar(long name, int global) {
  int v = nvars++;
  if (v >= MAXV) die("scc: too many variables", "");
  vname[v] = name; vglobal[v] = global;
  return v;
}
int iscell(int v) { return !vglobal[v] && vcaptured[v] && vnset[v] > 0; }

/* the global environment: symbol -> mkint(variable) or a macro */
enum { GSZ = 32768 };
long gkey[GSZ]; long gval[GSZ];
int gslot(long s) {
  int h = (int)((s >> 3) * 2654435761L) & (GSZ - 1);
  while (gkey[h] && gkey[h] != s) h = (h + 1) & (GSZ - 1);
  return h;
}
long gget(long s) { int h = gslot(s); if (gkey[h]) return gval[h]; return FALSE; }
void gput(long s, long v) { int h = gslot(s); gkey[h] = s; gval[h] = v; }

/* primitives: the runtime's p_ and v_ functions (see srt.c) */
enum { MAXP = 600 };
long pname[MAXP]; int pnargs[MAXP]; char *pcname[MAXP]; int nprims;
void prim(char *name, int nargs, char *cname) {
  pname[nprims] = sym(name); pnargs[nprims] = nargs; pcname[nprims] = cname; nprims++;
}
/* the row for a call with n arguments: a fixed one, else a variadic one */
int primrow(long s, int n) {
  int i; int var = -1;
  for (i = 0; i < nprims; i++) if (pname[i] == s) {
    if (pnargs[i] == n) return i;
    if (pnargs[i] < 0) var = i;
  }
  return var;
}
int isprim(long s) { int i; for (i = 0; i < nprims; i++) if (pname[i] == s) return 1; return 0; }
/* %name means the primitive name, even if the program defines name */
long primname(long s) {
  if (isprim(s)) return s;
  if (issym(s) && sl(s) > 1 && sptr(s)[0] == '%') {
    s = intern(sptr(s) + 1, sl(s) - 1);
    if (isprim(s)) return s;
  }
  return FALSE;
}

/* lambdas; lambda 0 is the top level */
enum { MAXL = 50000 };
long lparams[MAXL];   /* list of variables (raw ints) */
int lrest[MAXL];      /* the rest variable, or 0 */
int lnreq[MAXL];
long lbody[MAXL];
long lfree[MAXL];     /* list of free variables */
int lloop[MAXL];      /* inlined as a C loop */
int lparent[MAXL];    /* the enclosing C function */
int lvar[MAXL];       /* the variable it is bound to, if known */
int lconst[MAXL];     /* K index of its closure, if it has no free variables */
long lname[MAXL];
int nlams = 1;

/* ------------------------------------------------------------- the core */

enum { X_CONST = 1, X_REF, X_SET, X_IF, X_SEQ, X_LAM, X_CALL, X_PRIM, X_LET, X_FIX };
long node(int tag, long a, long b, long c) {
  long v = mkvec(4, NIL);
  velts(v)[0] = tag; velts(v)[1] = a; velts(v)[2] = b; velts(v)[3] = c;
  return v;
}
int tag(long x) { return (int)velts(x)[0]; }
long n1(long x) { return velts(x)[1]; }
long n2(long x) { return velts(x)[2]; }
long n3(long x) { return velts(x)[3]; }
long xconst(long d) { return node(X_CONST, d, 0, 0); }
long xseq(long l) { if (l == NULL) return xconst(NIL); if (cdr(l) == NULL) return car(l); return node(X_SEQ, l, 0, 0); }
long xref(int v) { vnref[v]++; return node(X_REF, v, 0, 0); }

/* ------------------------------------------------------------ expansion */

long xp(long x, long env);
long xbody(long forms, long env);

/* Macro expansion renames the symbols a template introduces: an alias
 * remembers its original and the environment of the macro's definition,
 * where it means what the original means unless the expansion binds it. */
long akey[GSZ]; long aorig[GSZ]; long aenv[GSZ];
int aslot(long s) {
  int h = (int)((s >> 3) * 2654435761L) & (GSZ - 1);
  while (akey[h] && akey[h] != s) h = (h + 1) & (GSZ - 1);
  return h;
}
int isalias(long s) { return akey[aslot(s)] == s; }
long unalias(long s) { while (issym(s) && isalias(s)) s = aorig[aslot(s)]; return s; }
long envfind(long s, long env) {
  long f; long e;
  for (f = env; f != NULL; f = cdr(f))
    for (e = car(f); e != NULL; e = cdr(e))
      if (car(car(e)) == s) return cdr(car(e));
  return FALSE;
}
/* look a symbol up: mkint(variable), a macro, or FALSE if unbound */
long lookup(long s, long env) {
  long b; int h;
  while (1) {
    b = envfind(s, env);
    if (b != FALSE) return b;
    b = gget(s);
    if (b != FALSE) return b;
    if (!isalias(s)) return FALSE;
    h = aslot(s); s = aorig[h]; env = aenv[h];
  }
  return FALSE;
}
int islocal(long s, long env) {
  int h;
  while (1) {
    if (envfind(s, env) != FALSE) return 1;
    if (gget(s) != FALSE || !isalias(s)) return 0;
    h = aslot(s); s = aorig[h]; env = aenv[h];
  }
  return 0;
}
/* the keyword s stands for, if it isn't a variable */
long kwname(long s, long env) {
  if (!issym(s) || islocal(s, env)) return FALSE;
  return unalias(s);
}
/* is x a use of keyword k (not shadowed by a local variable)? */
int iskw(long x, long k, long env) { return issym(x) && kwname(x, env) == k; }
/* a quoted datum, with aliases back to their originals */
long strip(long d) {
  if (issym(d)) return unalias(d);
  if (ispair(d)) return cons(strip(car(d)), strip(cdr(d)));
  if (otype(d) == OVEC) return list2vec(strip(p_vec2list(d)));
  return d;
}
int ismacro(long b) { return ispair(b); }

long expandmacro(long m, long x, long env);
/* expand the macro use at the head of x, if it is one */
long macroexpand(long x, long env) {
  long b;
  while (ispair(x) && issym(car(x))) {
    b = lookup(car(x), env);
    if (!ismacro(b)) break;
    x = expandmacro(b, x, env);
  }
  return x;
}

/* the variable for a free symbol: a global, or a primitive used as a value */
int globalvar(long s) {
  long b = gget(s); int v; long p;
  if (b != FALSE && !ismacro(b)) return (int)ival(b);
  v = newvar(s, 1);
  p = primname(s);
  if (p != FALSE) vprim[v] = p; else vundef[v] = 1;
  gput(s, mkint(v));
  return v;
}

long xlambda(long formals, long body, long env, long name) {
  int l = nlams++; long frame = NULL; long ps = NULL; int v; long f = formals;
  if (l >= MAXL) die("scc: too many lambdas", "");
  while (ispair(f)) {
    if (!issym(car(f))) synerr("bad parameter", formals);
    v = newvar(car(f), 0);
    frame = cons(cons(car(f), mkint(v)), frame);
    ps = cons(v, ps);
    lnreq[l]++;
    f = cdr(f);
  }
  if (f != NULL) {
    if (!issym(f)) synerr("bad parameter", formals);
    v = newvar(f, 0);
    frame = cons(cons(f, mkint(v)), frame);
    lrest[l] = v;
  }
  lparams[l] = rev(ps);
  lfree[l] = NULL;
  lname[l] = name;
  lbody[l] = xbody(body, cons(frame, env));
  return node(X_LAM, l, 0, 0);
}

/* quasiquote, into list operations */
int hasunquote(long x, int depth) {
  int i;
  if (otype(x) == OVEC) { for (i = 0; i < vlen(x); i++) if (hasunquote(velts(x)[i], depth)) return 1; return 0; }
  if (!ispair(x)) return 0;
  if (unalias(car(x)) == S_unquote || unalias(car(x)) == S_unqspl) { if (depth == 1) return 1; return hasunquote(cdr(x), depth - 1); }
  if (unalias(car(x)) == S_quasiquote) return hasunquote(cdr(x), depth + 1);
  return hasunquote(car(x), depth) || hasunquote(cdr(x), depth);
}
long qq(long x, int depth) {
  long a;
  if (!hasunquote(x, depth)) return L2(S_quote, x);
  if (otype(x) == OVEC) return L2(sym("%list->vector"), qq(p_vec2list(x), depth));
  if (unalias(car(x)) == S_unquote) {
    if (depth == 1) return cadr(x);
    return L3(sym("%list"), L2(S_quote, S_unquote), qq(cadr(x), depth - 1));
  }
  if (unalias(car(x)) == S_quasiquote) return L3(sym("%list"), L2(S_quote, S_quasiquote), qq(cadr(x), depth + 1));
  a = car(x);
  if (ispair(a) && unalias(car(a)) == S_unqspl && depth == 1) return L3(sym("%append"), cadr(a), qq(cdr(x), depth));
  return L3(sym("%cons"), qq(a, depth), qq(cdr(x), depth));
}

/* (define-record-type name (ctor field ...) pred (field accessor [modifier]) ...) -> definitions */
long recdefs(long x) {
  long tname = cadr(x); long ctor = caddr(x); long pred = car(cdddr(x)); long fields = cdr(cdddr(x));
  long defs = NULL; long f; long fs = NULL; int nf = 0; int i; long r = sym(" r"); long o = sym(" o"); long v = sym(" v");
  long body; long a;
  for (f = fields; f != NULL; f = cdr(f)) { fs = cons(issym(car(f)) ? car(f) : car(car(f)), fs); nf++; }
  fs = rev(fs);
  defs = cons(L3(S_define, tname, L3(sym("%make-record-type"), L2(S_quote, tname), mkint(nf))), defs);
  if (issym(ctor)) ctor = cons(ctor, fs);
  if (ctor != FALSE) {
    body = L1(r);
    for (a = cdr(ctor); a != NULL; a = cdr(a)) {
      i = 0;
      for (f = fs; f != NULL && car(f) != car(a); f = cdr(f)) i++;
      if (f == NULL) synerr("record constructor: no such field", car(a));
      body = cons(cons(sym("%record-set!"), L4(r, tname, mkint(i), car(a))), body);
    }
    defs = cons(L3(S_define, car(ctor), L3(S_lambda, cdr(ctor),
      L3(S_let, L1(L2(r, L2(sym("%record-alloc"), tname))), cons(S_begin, body)))), defs);
  }
  defs = cons(L3(S_define, pred, L3(S_lambda, L1(o), L3(sym("%record?"), o, tname))), defs);
  i = 0;
  for (f = fields; f != NULL; f = cdr(f)) {
    if (ispair(car(f)) && ispair(cdr(car(f)))) {
      a = cdr(car(f));
      defs = cons(L3(S_define, car(a), L3(S_lambda, L1(o), L4(sym("%record-ref"), o, tname, mkint(i)))), defs);
      if (cdr(a) != NULL)
        defs = cons(L3(S_define, cadr(a), L3(S_lambda, L2(o, v), cons(sym("%record-set!"), L4(o, tname, mkint(i), v)))), defs);
    }
    i++;
  }
  return rev(defs);
}

/* (define-values formals expr) -> definitions and an expression */
long valuesdefs(long x) {
  long formals = cadr(x); long f; long defs = NULL; long tmps = NULL; long sets = NULL; long t; long tf;
  for (f = formals; ispair(f); f = cdr(f)) {
    t = gensym();
    defs = cons(L3(S_define, car(f), FALSE), defs);
    tmps = cons(t, tmps);
    sets = cons(L3(S_set, car(f), t), sets);
  }
  tf = rev(tmps);
  if (f != NULL) {
    t = gensym();
    defs = cons(L3(S_define, f, FALSE), defs);
    sets = cons(L3(S_set, f, t), sets);
    tf = append2(tf, t);
  }
  sets = cons(L1(sym("%unspecified")), sets);
  defs = cons(L3(sym("%call-with-values"), L3(S_lambda, NULL, caddr(x)), cons(S_lambda, cons(tf, rev(sets)))), defs);
  return rev(defs);
}

long xdefsyntax(long x, long env);

/* the definitions and expressions of a body, flattened: a list of
 * (name . expr) for definitions and (#f . expr) for expressions */
long bodyitems(long forms, long env, long acc) {
  long x; long h;
  for (; forms != NULL; forms = cdr(forms)) {
    x = macroexpand(car(forms), env);
    h = ispair(x) ? car(x) : FALSE;
    if (iskw(h, S_begin, env)) acc = bodyitems(cdr(x), env, acc);
    else if (iskw(h, S_define, env)) {
      need(cdr(x), x);
      if (ispair(cadr(x))) acc = cons(cons(car(cadr(x)), cons(S_lambda, cons(cdr(cadr(x)), cddr(x)))), acc);
      else acc = cons(cons(cadr(x), cddr(x) == NULL ? L1(sym("%unspecified")) : caddr(x)), acc);
    }
    else if (iskw(h, S_defrecord, env)) acc = bodyitems(recdefs(x), env, acc);
    else if (iskw(h, S_defvalues, env)) acc = bodyitems(valuesdefs(x), env, acc);
    else if (iskw(h, S_defsyntax, env)) xdefsyntax(x, env);
    else acc = cons(cons(FALSE, x), acc);
  }
  return acc;
}
int islambda(long x, long env) { return ispair(x) && iskw(car(x), S_lambda, env); }

long xbody(long forms, long env) {
  long items = rev(bodyitems(forms, env, NULL)); long it; long frame = NULL; long env2;
  long lvars = NULL; long lams = NULL; long ovars = NULL; long seq = NULL; long b; int v; long inits = NULL;
  int anydef = 0;
  for (it = items; it != NULL; it = cdr(it)) if (car(car(it)) != FALSE) anydef = 1;
  if (!anydef) {
    for (it = items; it != NULL; it = cdr(it)) seq = cons(xp(cdr(car(it)), env), seq);
    return xseq(rev(seq));
  }
  for (it = items; it != NULL; it = cdr(it)) if (car(car(it)) != FALSE) {
    v = newvar(car(car(it)), 0);
    frame = cons(cons(car(car(it)), mkint(v)), frame);
  }
  env2 = cons(frame, env);
  for (it = items; it != NULL; it = cdr(it)) {
    if (car(car(it)) == FALSE) { seq = cons(xp(cdr(car(it)), env2), seq); continue; }
    v = (int)ival(lookup(car(car(it)), env2));
    if (islambda(cdr(car(it)), env2)) {
      lvars = cons(v, lvars);
      lams = cons(xlambda(cadr(cdr(car(it))), cddr(cdr(car(it))), env2, car(car(it))), lams);
      vlam[v] = (int)n1(car(lams));
    } else {
      ovars = cons(v, ovars);
      inits = cons(xconst(NIL), inits);
      vnset[v]++;
      seq = cons(node(X_SET, v, xp(cdr(car(it)), env2), 0), seq);
    }
  }
  b = xseq(rev(seq));
  if (lvars != NULL) b = node(X_FIX, rev(lvars), rev(lams), b);
  if (ovars != NULL) b = node(X_LET, rev(ovars), inits, b);
  return b;
}

/* (let ((v e) ...) body) */
long xlet(long binds, long body, long env) {
  long frame = NULL; long vars = NULL; long inits = NULL; long b; int v;
  for (b = binds; b != NULL; b = cdr(b)) {
    need(car(b), binds);
    inits = cons(cdr(car(b)) == NULL ? xconst(NIL) : xp(cadr(car(b)), env), inits);
  }
  for (b = binds; b != NULL; b = cdr(b)) {
    v = newvar(car(car(b)), 0);
    frame = cons(cons(car(car(b)), mkint(v)), frame);
    vars = cons(v, vars);
  }
  return node(X_LET, rev(vars), rev(inits), xbody(body, cons(frame, env)));
}

long xcond(long clauses, long env) {
  long c; long t; long rest;
  if (clauses == NULL) return L1(sym("%unspecified"));
  c = car(clauses); rest = xcond(cdr(clauses), env);
  if (!ispair(c)) synerr("bad cond clause", c);
  if (iskw(car(c), S_else, env)) return cons(S_begin, cdr(c));
  if (cdr(c) == NULL) return L3(S_or, car(c), rest);
  if (iskw(cadr(c), S_arrow, env)) {
    t = gensym();
    return L3(S_let, L1(L2(t, car(c))), L4(S_if, t, L2(caddr(c), t), rest));
  }
  return L4(S_if, car(c), cons(S_begin, cdr(c)), rest);
}

long xcase(long x, long env) {
  long t = gensym(); long cl; long c; long test; long body; long out = NULL;
  for (cl = cddr(x); cl != NULL; cl = cdr(cl)) {
    c = car(cl);
    if (iskw(car(c), S_else, env)) test = S_else;
    else if (ispair(car(c)) && cdr(car(c)) == NULL) test = L3(sym("%eqv?"), t, L2(S_quote, car(car(c))));
    else test = L3(sym("%memv"), t, L2(S_quote, car(c)));
    body = cdr(c);
    if (body != NULL && iskw(car(body), S_arrow, env)) body = L1(L2(cadr(body), t));
    out = cons(cons(test, body), out);
  }
  return L3(S_let, L1(L2(t, cadr(x))), cons(S_cond, rev(out)));
}

long xdo(long x) {
  long specs = cadr(x); long test = caddr(x); long body = cdddr(x); long g = gensym();
  long s; long binds = NULL; long steps = NULL; long res;
  for (s = specs; s != NULL; s = cdr(s)) {
    binds = cons(L2(car(car(s)), cadr(car(s))), binds);
    steps = cons(cddr(car(s)) != NULL ? caddr(car(s)) : car(car(s)), steps);
  }
  res = cdr(test) == NULL ? L1(sym("%unspecified")) : cons(S_begin, cdr(test));
  return L4(S_let, g, rev(binds),
    L4(S_if, car(test), res, cons(S_begin, append2(body, L1(cons(g, rev(steps)))))));
}

long xcaselambda(long x) {
  long args = gensym(); long n = gensym(); long cl; long out = NULL; long f; int k; int rest;
  for (cl = cdr(x); cl != NULL; cl = cdr(cl)) {
    k = 0; rest = 0;
    for (f = car(car(cl)); ispair(f); f = cdr(f)) k++;
    if (f != NULL) rest = 1;
    out = cons(L2(L3(sym(rest ? "%>=" : "%="), n, mkint(k)),
                  L3(sym("%apply"), cons(S_lambda, car(cl)), args)), out);
  }
  out = cons(L2(S_else, L2(sym("%error"), cstr("case-lambda: no clause matches the arguments"))), out);
  return L3(S_lambda, args, L3(S_let, L1(L2(n, L2(sym("%length"), args))), cons(S_cond, rev(out))));
}

/* a call to a primitive: special cases for the variadic arithmetic and list */
long xprimcall(long p, long args, long form, long env) {
  int n = len(args); int r; long l; long xs = NULL; long acc;
  if ((symis(p, "+") || symis(p, "*") || symis(p, "-")) && n > 2) {
    r = primrow(p, 2);
    acc = xp(car(args), env);
    for (l = cdr(args); l != NULL; l = cdr(l)) acc = node(X_PRIM, r, L2(acc, xp(car(l), env)), 0);
    return acc;
  }
  if (symis(p, "+") || symis(p, "*")) {
    if (n == 0) return xconst(mkint(symis(p, "+") ? 0 : 1));
    if (n == 1) return node(X_PRIM, primrow(p, 2), L2(xconst(mkint(symis(p, "+") ? 0 : 1)), xp(car(args), env)), 0);
  }
  if (symis(p, "list")) {
    for (l = args; l != NULL; l = cdr(l)) xs = cons(xp(car(l), env), xs);
    acc = xconst(NULL);
    r = primrow(sym("cons"), 2);
    for (l = xs; l != NULL; l = cdr(l)) acc = node(X_PRIM, r, L2(car(l), acc), 0);
    return acc;
  }
  r = primrow(p, n);
  if (r < 0) synerr("wrong number of arguments to a primitive", form);
  for (l = args; l != NULL; l = cdr(l)) xs = cons(xp(car(l), env), xs);
  return node(X_PRIM, r, rev(xs), 0);
}

long xp(long x, long env) {
  long b; long h; long a; long f; long g; long t; long p; int v;
  if (issym(x)) {
    b = lookup(x, env);
    if (b == FALSE) return xref(globalvar(unalias(x)));
    if (ismacro(b)) synerr("syntax keyword used as a variable", x);
    return xref((int)ival(b));
  }
  if (!ispair(x)) {
    if (x == NULL) synerr("empty combination", x);
    return xconst(x);
  }
  h = car(x);
  if (issym(h)) {
    b = lookup(h, env);
    if (ismacro(b)) return xp(expandmacro(b, x, env), env);
  }
  if (issym(h) && kwname(h, env) != FALSE) {
    h = kwname(h, env);
    if (h == S_quote) { need(cdr(x), x); return xconst(strip(cadr(x))); }
    if (h == S_quasiquote) return xp(qq(cadr(x), 1), env);
    if (h == S_lambda) { need(cdr(x), x); return xlambda(cadr(x), cddr(x), env, FALSE); }
    if (h == S_if) {
      need(cddr(x), x);
      return node(X_IF, xp(cadr(x), env), xp(caddr(x), env),
                  cdddr(x) == NULL ? xconst(NIL) : xp(car(cdddr(x)), env));
    }
    if (h == S_set) {
      need(cddr(x), x);
      b = lookup(cadr(x), env);
      v = b == FALSE ? globalvar(unalias(cadr(x))) : (int)ival(b);
      vnset[v]++;
      if (vglobal[v]) { vundef[v] = 0; vprim[v] = 0; }
      return node(X_SET, v, xp(caddr(x), env), 0);
    }
    if (h == S_define) synerr("definition in an expression context", x);
    if (h == S_begin) {
      a = NULL;
      for (f = cdr(x); f != NULL; f = cdr(f)) a = cons(xp(car(f), env), a);
      return xseq(rev(a));
    }
    if (h == S_let) {
      need(cdr(x), x);
      if (issym(cadr(x))) {   /* named let: inits outside the name's scope */
        a = NULL; g = NULL;
        for (f = caddr(x); f != NULL; f = cdr(f)) { t = gensym(); a = cons(L2(t, cadr(car(f))), a); g = cons(t, g); }
        b = NULL;
        for (f = caddr(x); f != NULL; f = cdr(f)) b = cons(car(car(f)), b);
        return xp(L3(S_let, rev(a),
          L3(S_letrec, L1(L2(cadr(x), cons(S_lambda, cons(rev(b), cdddr(x))))), cons(cadr(x), rev(g)))), env);
      }
      return xlet(cadr(x), cddr(x), env);
    }
    if (h == S_letstar) {
      if (cadr(x) == NULL || cdr(cadr(x)) == NULL) return xlet(cadr(x), cddr(x), env);
      return xlet(L1(car(cadr(x))), L1(cons(S_letstar, cons(cdr(cadr(x)), cddr(x)))), env);
    }
    if (h == S_letrec || h == S_letrecstar) {
      a = NULL;
      for (f = cadr(x); f != NULL; f = cdr(f)) a = cons(cons(S_define, car(f)), a);
      return xbody(append2(rev(a), cddr(x)), env);
    }
    if (h == S_cond) return xp(xcond(cdr(x), env), env);
    if (h == S_case) return xp(xcase(x, env), env);
    if (h == S_and) {
      if (cdr(x) == NULL) return xconst(TRUE);
      if (cddr(x) == NULL) return xp(cadr(x), env);
      return node(X_IF, xp(cadr(x), env), xp(cons(S_and, cddr(x)), env), xconst(FALSE));
    }
    if (h == S_or) {
      if (cdr(x) == NULL) return xconst(FALSE);
      if (cddr(x) == NULL) return xp(cadr(x), env);
      t = gensym();
      return xp(L3(S_let, L1(L2(t, cadr(x))), L4(S_if, t, t, cons(S_or, cddr(x)))), env);
    }
    if (h == S_when) return node(X_IF, xp(cadr(x), env), xp(cons(S_begin, cddr(x)), env), xconst(NIL));
    if (h == S_unless) return node(X_IF, xp(cadr(x), env), xconst(NIL), xp(cons(S_begin, cddr(x)), env));
    if (h == S_do) return xp(xdo(x), env);
    if (h == S_delay)
      return xp(L3(sym("%make-promise"), FALSE, L3(S_lambda, NULL, L3(sym("%make-promise"), TRUE, cadr(x)))), env);
    if (h == S_delayforce) return xp(L3(sym("%make-promise"), FALSE, L3(S_lambda, NULL, cadr(x))), env);
    if (h == S_receive)
      return xp(L3(sym("%call-with-values"), L3(S_lambda, NULL, caddr(x)), cons(S_lambda, cons(cadr(x), cdddr(x)))), env);
    if (h == S_letvalues || h == S_letstarvalues) {
      if (cadr(x) == NULL) return xlet(NULL, cddr(x), env);
      a = car(cadr(x));
      return xp(L3(sym("%call-with-values"), L3(S_lambda, NULL, cadr(a)),
        L3(S_lambda, car(a), cons(h, cons(cdr(cadr(x)), cddr(x))))), env);
    }
    if (h == S_caselambda) return xp(xcaselambda(x), env);
    if (h == S_guard) {   /* (guard (e clause ...) body ...) */
      a = cdr(cadr(x));
      for (f = a; f != NULL && !(ispair(car(f)) && unalias(car(car(f))) == S_else); f = cdr(f));
      if (f == NULL) a = append2(a, L1(L2(S_else, L2(sym("%raise-continuable"), car(cadr(x))))));
      return xp(L3(sym("%guard"), cons(S_lambda, cons(NULL, cddr(x))),
                   L3(S_lambda, L1(car(cadr(x))), cons(S_cond, a))), env);
    }
    if (h == S_parameterize) {
      a = NULL; g = NULL;
      for (f = cadr(x); f != NULL; f = cdr(f)) { a = cons(car(car(f)), a); g = cons(cadr(car(f)), g); }
      return xp(L4(sym("%parameterize"), cons(sym("%list"), rev(a)), cons(sym("%list"), rev(g)),
                   cons(S_lambda, cons(NULL, cddr(x)))), env);
    }
    if (h == S_letsyntax || h == S_letrecsyntax) {
      for (f = cadr(x); f != NULL; f = cdr(f)) xdefsyntax(cons(S_defsyntax, car(f)), env);
      return xbody(cddr(x), env);
    }
    if (h == S_defsyntax) { xdefsyntax(x, env); return xconst(NIL); }
    if (h == S_defrecord || h == S_defvalues) return xbody(L1(x), env);
    p = primname(h);
    if (p != FALSE && (b == FALSE || vprim[ival(b)])) return xprimcall(p, cdr(x), x, env);
  }
  /* a call */
  f = xp(car(x), env);
  a = NULL;
  for (g = cdr(x); g != NULL; g = cdr(g)) {
    if (!ispair(g)) synerr("bad call", x);
    a = cons(xp(car(g), env), a);
  }
  return node(X_CALL, f, rev(a), 0);
}

/* --------------------------------------------------------- syntax-rules */

/* A macro is (definition-env literals ellipsis . rules).  Expansion is
 * hygienic only in one direction: symbols the template introduces that
 * mean nothing where the macro was defined (its temporaries) are renamed
 * at each use, so they can't capture the user's variables. */
int iskeyword(long s) {
  return s == S_quote || s == S_quasiquote || s == S_lambda || s == S_define || s == S_set ||
    s == S_if || s == S_begin || s == S_let || s == S_letstar || s == S_letrec ||
    s == S_letrecstar || s == S_cond || s == S_case || s == S_and || s == S_or ||
    s == S_when || s == S_unless || s == S_do || s == S_else || s == S_arrow ||
    s == S_delay || s == S_delayforce || s == S_defrecord || s == S_letvalues ||
    s == S_letstarvalues || s == S_defvalues || s == S_receive || s == S_caselambda ||
    s == S_defsyntax || s == S_letsyntax || s == S_letrecsyntax || s == S_syntaxrules ||
    s == S_guard || s == S_parameterize || s == S_unquote || s == S_unqspl;
}

long xdefsyntax(long x, long env) {
  long name = cadr(x); long sr = caddr(x); long ell = S_ellipsis; long lits;
  if (!ispair(sr) || !iskw(car(sr), S_syntaxrules, env)) synerr("only syntax-rules macros are supported", x);
  sr = cdr(sr);
  if (issym(car(sr))) { ell = car(sr); sr = cdr(sr); }
  lits = car(sr);
  gput(name, cons(env, cons(lits, cons(ell, cdr(sr)))));
  return NIL;
}

long mlits; long mell; long MFAIL;
long assq_(long x, long l);
long patvars(long p, long acc);
int memq_(long x, long l) { while (ispair(l)) { if (car(l) == x) return 1; l = cdr(l); } return 0; }
/* match form x against pattern p, adding to bindings b; MFAIL if no match */
long match(long p, long x, long b) {
  long q; int np; int nx; int i; long items; long subs; long vars; long v; long r;
  if (issym(p)) {
    if (p == S_underscore) return b;
    if (memq_(p, mlits)) return unalias(x) == unalias(p) ? b : MFAIL;
    return cons(cons(p, x), b);
  }
  if (otype(p) == OVEC) {
    if (otype(x) != OVEC) return MFAIL;
    return match(p_vec2list(p), p_vec2list(x), b);
  }
  if (ispair(p)) {
    if (ispair(cdr(p)) && cadr(p) == mell) {   /* p0 ... rest */
      q = cddr(p); np = 0;
      while (ispair(q)) { np++; q = cdr(q); }
      nx = 0; q = x;
      while (ispair(q)) { nx++; q = cdr(q); }
      if (nx < np) return MFAIL;
      /* match nx - np items against p0 */
      subs = NULL;
      for (i = 0; i < nx - np; i++) {
        r = match(car(p), car(x), NULL);
        if (r == MFAIL) return MFAIL;
        subs = cons(r, subs);
        x = cdr(x);
      }
      subs = rev(subs);
      vars = patvars(car(p), NULL);
      for (; vars != NULL; vars = cdr(vars)) {
        v = car(vars); items = NULL;
        for (q = subs; q != NULL; q = cdr(q)) items = cons(cdr(assq_(v, car(q))), items);
        b = cons(cons(v, cons(mell, rev(items))), b);
      }
      return match(cddr(p), x, b);
    }
    if (!ispair(x)) return MFAIL;
    b = match(car(p), car(x), b);
    if (b == MFAIL) return MFAIL;
    return match(cdr(p), cdr(x), b);
  }
  if (p == NULL) return x == NULL ? b : MFAIL;
  if (p == x || (otype(p) == OSTR && otype(x) == OSTR && sl(p) == sl(x) && memeq(sptr(p), sptr(x), sl(p)))) return b;
  return MFAIL;
}
long assq_(long x, long l) { while (ispair(l)) { if (car(car(l)) == x) return car(l); l = cdr(l); } return FALSE; }
long patvars(long p, long acc) {
  if (issym(p)) { if (p == mell || p == S_underscore || memq_(p, mlits)) return acc; return cons(p, acc); }
  if (otype(p) == OVEC) return patvars(p_vec2list(p), acc);
  if (ispair(p)) return patvars(cdr(p), patvars(car(p), acc));
  return acc;
}

long menv; long renames;
long instantiate(long t, long b) {
  long r; long vars; long out; long seqs; long q; long nb; int n; int i; int j; long tail;
  if (issym(t)) {
    r = assq_(t, b);
    if (r != FALSE) return cdr(r);
    if (t == mell) return t;
    r = assq_(t, renames);
    if (r != FALSE) return cdr(r);
    sbn = 0; sbputs(sptr(t), sl(t)); sbput(' '); sbint(++ngensym);
    r = intern(sb, sbn); sbn = 0;
    renames = cons(cons(t, r), renames);
    j = aslot(r); akey[j] = r; aorig[j] = t; aenv[j] = menv;
    return r;
  }
  if (otype(t) == OVEC) return list2vec(instantiate(p_vec2list(t), b));
  if (!ispair(t)) return t;
  if (car(t) == mell && ispair(cdr(t))) return cadr(t);   /* (... template): literal */
  if (ispair(cdr(t)) && cadr(t) == mell) {
    /* the variables in car(t) bound to sequences drive the repetition */
    vars = patvars(car(t), NULL);
    seqs = NULL; n = -1;
    for (; vars != NULL; vars = cdr(vars)) {
      r = assq_(car(vars), b);
      if (r != FALSE && ispair(cdr(r)) && car(cdr(r)) == mell) {
        seqs = cons(r, seqs);
        if (n < 0 || len(cddr(r)) < n) n = len(cddr(r));
      }
    }
    if (n < 0) synerr("ellipsis without pattern variables in template", t);
    out = NULL;
    for (i = 0; i < n; i++) {
      nb = b;
      for (q = seqs; q != NULL; q = cdr(q)) {
        r = cddr(car(q));
        for (j = 0; j < i; j++) r = cdr(r);
        nb = cons(cons(car(car(q)), car(r)), nb);
      }
      out = cons(instantiate(car(t), nb), out);
    }
    tail = cddr(t);
    while (ispair(tail) && car(tail) == mell) tail = cdr(tail);   /* x ... ... : flatten */
    return append2(rev(out), instantiate(tail, b));
  }
  return cons(instantiate(car(t), b), instantiate(cdr(t), b));
}

long expandmacro(long m, long x, long env) {
  long rules = cdddr(m); long b; long saveenv = menv; long r;
  mlits = cadr(m); mell = caddr(m);
  for (; rules != NULL; rules = cdr(rules)) {
    b = match(cdr(car(car(rules))), cdr(x), NULL);   /* the keyword position is ignored */
    if (b != MFAIL) {
      menv = car(m); renames = NULL;
      r = instantiate(cadr(car(rules)), b);
      menv = saveenv;
      return r;
    }
  }
  synerr("no syntax-rules pattern matches", x);
  return NIL;
}

/* ------------------------------------------------------------ top level */

int known(int v) { return vlam[v] && (vglobal[v] ? vnset[v] == 1 : vnset[v] == 0); }

void toplevel(long forms) {
  long items = rev(bodyitems(forms, NULL, NULL)); long it; long seq = NULL; long name; long e; long b; int v; long x;
  for (it = items; it != NULL; it = cdr(it)) {
    name = car(car(it));
    if (name == FALSE) continue;
    b = gget(name);
    if (b == FALSE || ismacro(b)) gput(name, mkint(newvar(name, 1)));
    else { v = (int)ival(b); vprim[v] = 0; vundef[v] = 0; }
  }
  for (it = items; it != NULL; it = cdr(it)) {
    name = car(car(it)); e = cdr(car(it));
    if (name == FALSE) {
      if (ispair(e) && (car(e) == S_import || car(e) == S_definelibrary)) continue;
      seq = cons(xp(e, NULL), seq);
      continue;
    }
    v = (int)ival(gget(name));
    vnset[v]++;
    if (islambda(e, NULL)) {
      x = xlambda(cadr(e), cddr(e), NULL, name);
      vlam[v] = (int)n1(x);
    } else x = xp(e, NULL);
    seq = cons(node(X_SET, v, x, 0), seq);
  }
  lbody[0] = xseq(rev(seq));
}

/* ---------------------------------------------------------------- loops */

int okref;
void tscan(long x, int v, int tail, int inlam) {
  int t = tag(x); long l; long f;
  if (t == X_REF) { if (n1(x) == v) okref = 0; return; }
  if (t == X_SET) { if (n1(x) == v) okref = 0; tscan(n2(x), v, 0, inlam); return; }
  if (t == X_IF) { tscan(n1(x), v, 0, inlam); tscan(n2(x), v, tail, inlam); tscan(n3(x), v, tail, inlam); return; }
  if (t == X_SEQ) {
    for (l = n1(x); l != NULL; l = cdr(l)) tscan(car(l), v, cdr(l) == NULL ? tail : 0, inlam);
    return;
  }
  if (t == X_LAM) { tscan(lbody[n1(x)], v, 0, 1); return; }
  if (t == X_CALL) {
    f = n1(x);
    if (tag(f) == X_REF && n1(f) == v) { if (!tail || inlam) okref = 0; }
    else tscan(f, v, 0, inlam);
    for (l = n2(x); l != NULL; l = cdr(l)) tscan(car(l), v, 0, inlam);
    return;
  }
  if (t == X_PRIM) { for (l = n2(x); l != NULL; l = cdr(l)) tscan(car(l), v, 0, inlam); return; }
  if (t == X_LET) {
    for (l = n2(x); l != NULL; l = cdr(l)) tscan(car(l), v, 0, inlam);
    tscan(n3(x), v, tail, inlam);
    return;
  }
  if (t == X_FIX) {
    for (l = n2(x); l != NULL; l = cdr(l)) tscan(lbody[n1(car(l))], v, 0, 1);
    tscan(n3(x), v, tail, inlam);
  }
}

void findloops(long x) {
  int t = tag(x); long l; int v; int lam; long c;
  if (t == X_SET) findloops(n2(x));
  else if (t == X_IF) { findloops(n1(x)); findloops(n2(x)); findloops(n3(x)); }
  else if (t == X_SEQ) { for (l = n1(x); l != NULL; l = cdr(l)) findloops(car(l)); }
  else if (t == X_LAM) findloops(lbody[n1(x)]);
  else if (t == X_CALL) { findloops(n1(x)); for (l = n2(x); l != NULL; l = cdr(l)) findloops(car(l)); }
  else if (t == X_PRIM) { for (l = n2(x); l != NULL; l = cdr(l)) findloops(car(l)); }
  else if (t == X_LET) { for (l = n2(x); l != NULL; l = cdr(l)) findloops(car(l)); findloops(n3(x)); }
  else if (t == X_FIX) {
    for (l = n2(x); l != NULL; l = cdr(l)) findloops(lbody[n1(car(l))]);
    findloops(n3(x));
    if (cdr(n1(x)) != NULL) return;
    v = (int)car(n1(x)); lam = (int)n1(car(n2(x))); c = n3(x);
    if (!known(v) || lrest[lam] || tag(c) != X_CALL || tag(n1(c)) != X_REF || n1(n1(c)) != v) return;
    if (len(n2(c)) != lnreq[lam]) return;
    okref = 1;
    tscan(lbody[lam], v, 1, 0);
    for (l = n2(c); l != NULL; l = cdr(l)) tscan(car(l), v, 0, 1);
    if (okref) lloop[lam] = 1;
  }
}

/* ------------------------------------------------------------- closures */

void addfree(int f, int v) {
  long l;
  for (l = lfree[f]; l != NULL; l = cdr(l)) if (car(l) == v) return;
  lfree[f] = append2(lfree[f], L1(v));
}
void params(int lam, int cf) {
  long l;
  for (l = lparams[lam]; l != NULL; l = cdr(l)) vcfun[car(l)] = cf;
  if (lrest[lam]) vcfun[lrest[lam]] = cf;
}
void ana(long x, int cf) {
  int t = tag(x); long l; int v; int f; int lam;
  if (t == X_REF || t == X_SET) {
    v = (int)n1(x);
    if (!vglobal[v] && vcfun[v] != cf) {
      vcaptured[v] = 1;
      for (f = cf; f != vcfun[v]; f = lparent[f]) addfree(f, v);
    }
    if (t == X_SET) ana(n2(x), cf);
  }
  else if (t == X_IF) { ana(n1(x), cf); ana(n2(x), cf); ana(n3(x), cf); }
  else if (t == X_SEQ) { for (l = n1(x); l != NULL; l = cdr(l)) ana(car(l), cf); }
  else if (t == X_LAM) {
    lam = (int)n1(x);
    lparent[lam] = cf; params(lam, lam);
    ana(lbody[lam], lam);
  }
  else if (t == X_CALL) { ana(n1(x), cf); for (l = n2(x); l != NULL; l = cdr(l)) ana(car(l), cf); }
  else if (t == X_PRIM) { for (l = n2(x); l != NULL; l = cdr(l)) ana(car(l), cf); }
  else if (t == X_LET) {
    for (l = n2(x); l != NULL; l = cdr(l)) ana(car(l), cf);
    for (l = n1(x); l != NULL; l = cdr(l)) vcfun[car(l)] = cf;
    ana(n3(x), cf);
  }
  else if (t == X_FIX) {
    for (l = n1(x); l != NULL; l = cdr(l)) vcfun[car(l)] = cf;
    for (l = n2(x); l != NULL; l = cdr(l)) {
      lam = (int)n1(car(l));
      if (lloop[lam]) { params(lam, cf); ana(lbody[lam], cf); }
      else { lparent[lam] = cf; params(lam, lam); ana(lbody[lam], lam); }
    }
    ana(n3(x), cf);
  }
}

/* --------------------------------------------------------------- output */

/* growable text buffers */
enum { B_BODY, B_PRO, B_FUNS, B_INIT, NBUF };
char *bp[NBUF]; int bn[NBUF]; int bcap[NBUF];
int cur;
void Eb(char *s, int n) {
  char *nb;
  if (bn[cur] + n > bcap[cur]) {
    while (bn[cur] + n > bcap[cur]) bcap[cur] = bcap[cur] ? bcap[cur] * 2 : 65536;
    nb = malloc(bcap[cur]); memcopy(nb, bp[cur], bn[cur]); bp[cur] = nb;
  }
  memcopy(bp[cur] + bn[cur], s, n);
  bn[cur] = bn[cur] + n;
}
void E(char *s) { Eb(s, slen(s)); }
char numbuf[32];
void Ei(long x) {
  int i = 31; int neg = x < 0; unsigned long u = neg ? -(unsigned long)x : (unsigned long)x;
  numbuf[i] = 0;
  do { numbuf[--i] = '0' + (int)(u % 10); u = u / 10; } while (u);
  if (neg) numbuf[--i] = '-';
  E(numbuf + i);
}
void El(long x) {   /* a long literal */
  if (x == -9223372036854775807L - 1) { E("(-9223372036854775807L - 1)"); return; }
  if (x < 0) { E("(-"); Ei(-x); E("L)"); return; }
  Ei(x); E("L");
}
char hexd[17];
void Ecstr(char *p, int n) {   /* a C string literal */
  int i; int c;
  E("\"");
  for (i = 0; i < n; i++) {
    c = p[i] & 255;
    if (c < 32 || c > 126 || c == '"' || c == '\\') {
      E("\\x");
      numbuf[0] = hexd[c >> 4]; numbuf[1] = hexd[c & 15]; numbuf[2] = 0;
      E(numbuf);
    } else { numbuf[0] = c; numbuf[1] = 0; E(numbuf); }
  }
  E("\"");
}
void Ename(long s) {   /* a symbol, for a comment */
  int i; int c;
  E("/* ");
  for (i = 0; i < sl(s); i++) {
    c = sptr(s)[i];
    if (c == '*' || c < 32 || c > 126) c = '_';
    numbuf[0] = c; numbuf[1] = 0; E(numbuf);
  }
  E(" */");
}

/* ------------------------------------------------------------ constants */

/* Constants live in K[], made by sc_init: compound data are read from
 * their written form. */
int nk;
long ksymk[GSZ]; long ksyms[GSZ];   /* symbol -> its K slot */
int compound(long d) { return ispair(d) || otype(d) == OVEC; }
int symplain(long s) {
  int i; int c;
  if (sl(s) == 0 || parsenum(sptr(s), sl(s), 10) != FALSE) return 0;
  for (i = 0; i < sl(s); i++) {
    c = sptr(s)[i];
    if (isdelim(c) || c == '|' || c == '\'' || c == '`' || c == ',' || c == '#' && i == 0) return 0;
  }
  return !(sl(s) == 1 && sptr(s)[0] == '.');
}
void sbwritten(long d) {   /* the written form of d, for the reader */
  int t = otype(d); int i; int c;
  if (t == OPAIR) {
    sbput('(');
    while (1) { sbwritten(car(d)); d = cdr(d); if (!ispair(d)) break; sbput(' '); }
    if (d != NULL) { sbcstr(" . "); sbwritten(d); }
    sbput(')');
  } else if (t == OVEC) {
    sbcstr("#(");
    for (i = 0; i < vlen(d); i++) { if (i) sbput(' '); sbwritten(velts(d)[i]); }
    sbput(')');
  } else if (t == OSTR) {
    sbput('"');
    for (i = 0; i < sl(d); i++) {
      c = sptr(d)[i] & 255;
      if (c == '"' || c == '\\') { sbput('\\'); sbput(c); }
      else if (c < 32 || c > 126) { sbcstr("\\x"); sbintradix(c, 16); sbput(';'); }
      else sbput(c);
    }
    sbput('"');
  } else if (t == OSYM) {
    if (symplain(d)) sbputs(sptr(d), sl(d));
    else { sbput('|'); sbputs(sptr(d), sl(d)); sbput('|'); }
  } else if (ischar(d)) { sbcstr("#\\x"); sbintradix(charval(d), 16); }
  else if (isdbl(d)) {
    if (dval(d) != dval(d)) sbcstr("+nan.0");
    else if (dval(d) - dval(d) != dval(d) - dval(d)) sbcstr(dval(d) > 0 ? "+inf.0" : "-inf.0");
    else {
      c = sbn; sbdblshort(dval(d));
      for (i = c; i < sbn && sb[i] != '.' && sb[i] != 'e'; i++);
      if (i == sbn) sbcstr(".0");
    }
  }
  else if (isint(d)) sbint(ival(d));
  else if (d == TRUE) sbcstr("#t");
  else if (d == FALSE) sbcstr("#f");
  else if (d == NULL) sbcstr("()");
  else die("scc: a constant that can't be written", "");
}
void kexpr(long d) {   /* the C expression for a non-compound constant */
  int t = otype(d);
  if (t == OSTR) { E("mkstr("); Ecstr(sptr(d), sl(d)); E(", "); Ei(sl(d)); E(")"); }
  else if (t == OSYM) { E("intern("); Ecstr(sptr(d), sl(d)); E(", "); Ei(sl(d)); E(")"); }
  else if (t == OINT) { E("mkint("); El(ival(d)); E(")"); }
  else El(d);
}
int kbuild(long d) {
  int k; int i; int save = cur;
  cur = B_INIT;
  if (otype(d) == OSYM) {
    i = (int)((d >> 3) * 2654435761L) & (GSZ - 1);
    while (ksyms[i] && ksyms[i] != d) i = (i + 1) & (GSZ - 1);
    if (ksyms[i]) { cur = save; return (int)ksymk[i]; }
    ksyms[i] = d; ksymk[i] = nk;
  }
  k = nk++;
  E("  K["); Ei(k); E("] = ");
  if (compound(d)) {
    sbn = 0; sbwritten(d);
    E("kread("); Ecstr(sb, sbn); E(")");
    sbn = 0;
  } else kexpr(d);
  E(";\n");
  cur = save;
  return k;
}

/* ------------------------------------------------------------- operands */

/* an operand: a kind and an index, as one int */
enum { O_T = 1, O_L, O_CELL, O_G, O_K, O_IMM };
int mkop(int k, int i) { return (k << 26) | i; }
int opk(int o) { return o >> 26; }
int opi(int o) { return o & 67108863; }
long imms[1000000]; int nimm;
int imm(long x) { imms[nimm] = x; return mkop(O_IMM, nimm++); }
int isimmediate(long d) { return d == NIL || d == FALSE || d == TRUE || d == NULL || d == EOFV || isfix(d) || isdbl(d) || ischar(d); }
int constop(long d) { if (isimmediate(d)) return imm(d); return mkop(O_K, kbuild(d)); }
void Eop(int o) {
  int k = opk(o); int i = opi(o);
  if (k == O_T) { E("T"); Ei(i); }
  else if (k == O_L) { E("L"); Ei(i); }
  else if (k == O_CELL) { E("(*(long *)(int)L"); Ei(i); E(")"); }
  else if (k == O_G) { E("G"); Ei(i); }
  else if (k == O_K) { E("K["); Ei(i); E("]"); }
  else El(imms[i]);
}
int varop(int v) {
  if (vglobal[v]) return mkop(O_G, v);
  if (iscell(v)) return mkop(O_CELL, v);
  return mkop(O_L, v);
}

/* ---------------------------------------------------------- generation */

enum { C_VAL, C_EFF, C_TAIL, C_LOOP };
int curfun; int ntemps; int curloop; int loopres; int usedwhile;
long decls;
int newtemp() { return ntemps++; }
int stable(int o) {
  int k = opk(o); int v = opi(o);
  if (k == O_T || k == O_K || k == O_IMM) return 1;
  if (k == O_L) return vnset[v] == 0;
  if (k == O_G) return (vnset[v] <= 1 && !vundef[v]) || vprim[v];
  return 0;
}
int totemp(int o) {
  int t = newtemp();
  E("T"); Ei(t); E(" = "); Eop(o); E(";\n");
  return mkop(O_T, t);
}
int stab(int o) { if (stable(o)) return o; return totemp(o); }
void declare(int v) {
  if (vmark[v] == curfun + 1) return;
  vmark[v] = curfun + 1;
  decls = cons(v, decls);
}
void bind(int v, int o) {
  declare(v);
  E("L"); Ei(v); E(" = ");
  if (iscell(v)) { E("newcell("); Eop(o); E(")"); } else Eop(o);
  E(";\n");
}
int deliver(int o, int ctx) {
  if (ctx == C_VAL) return o;
  if (ctx == C_TAIL) { E("stk[0] = "); Eop(o); E("; return 1;\n"); }
  if (ctx == C_LOOP) { E("T"); Ei(loopres); E(" = "); Eop(o); E("; break;\n"); }
  return -1;
}

int cg(long x, int ctx);
/* evaluate arguments to operands, kept on an operand stack */
int aops[100000]; int asp;
int cgargs(long args, int all) {
  int base = asp; long l; int o;
  for (l = args; l != NULL; l = cdr(l)) {
    o = cg(car(l), C_VAL);
    if (all || cdr(l) != NULL) o = stab(o);
    aops[asp++] = o;
  }
  return base;
}
void storeargs(int base, int n) {
  int i;
  for (i = 0; i < n; i++) { E("stk["); Ei(i); E("] = "); Eop(aops[base + i]); E(";\n"); }
}
/* after the call expression: pass on unwinding, take the result */
void callpre(int ctx) { if (ctx == C_TAIL) E("return "); else E("if ("); }
int callpost(int ctx) {
  int t;
  if (ctx == C_TAIL) { E(";\n"); return -1; }
  E(" < 0) return -1;\n");
  if (ctx == C_EFF) return -1;
  if (ctx == C_LOOP) { E("T"); Ei(loopres); E(" = stk[0]; break;\n"); return -1; }
  t = newtemp();
  E("T"); Ei(t); E(" = stk[0];\n");
  return mkop(O_T, t);
}

/* the closure for lambda lam: a constant if it has no free variables */
int closure(int lam) {
  long l; int t; int i = 0;
  if (lfree[lam] == NULL) {
    if (!lconst[lam]) {
      cur = B_INIT;
      E("  K["); Ei(nk); E("] = mkclo(F"); Ei(lam); E(", 0);\n");
      lconst[lam] = ++nk;
      cur = B_BODY;
    }
    return mkop(O_K, lconst[lam] - 1);
  }
  t = newtemp();
  E("T"); Ei(t); E(" = mkclo(F"); Ei(lam); E(", "); Ei(len(lfree[lam])); E(");\n");
  return mkop(O_T, t);
}
void fillfree(int lam, int t) {
  long l; int i = 0;
  for (l = lfree[lam]; l != NULL; l = cdr(l)) {
    E("*(long *)((int)"); Eop(t); E(" + "); Ei(16 + 8 * i); E(") = L"); Ei(car(l)); E(";\n");
    i++;
  }
}

int cgloop(long x, int ctx) {
  int lam = (int)n1(car(n2(x))); long c = n3(x); int base; int i; long l;
  int sl_ = curloop; int sr = loopres; int r;
  base = cgargs(n2(c), 0);
  i = 0;
  for (l = lparams[lam]; l != NULL; l = cdr(l)) bind((int)car(l), aops[base + i++]);
  asp = base;
  curloop = lam;
  if (ctx == C_TAIL) {
    E("while (1) {\n"); cg(lbody[lam], C_TAIL); E("}\n");
    curloop = sl_;
    return -1;
  }
  r = newtemp(); loopres = r;
  E("while (1) {\n"); cg(lbody[lam], C_LOOP); E("}\n");
  curloop = sl_; loopres = sr;
  return deliver(mkop(O_T, r), ctx);
}

int cgcall(long x, int ctx) {
  long f = n1(x); long args = n2(x); int n = len(args); int v; int lam = 0; int fo = 0; int base; int i; long l; int o;
  if (tag(f) == X_REF && known((int)n1(f))) {
    v = (int)n1(f); lam = vlam[v];
    if (lnreq[lam] != n && !(lrest[lam] && n >= lnreq[lam])) lam = 0;
    if (lam && lam == curloop && (ctx == C_TAIL || ctx == C_LOOP) && !lrest[lam]) {
      /* a self tail call: a jump back to the top */
      base = cgargs(args, 1);
      for (i = 0; i < n; i++) {
        o = aops[base + i];
        if (opk(o) != O_T && opk(o) != O_IMM && opk(o) != O_K) aops[base + i] = totemp(o);
      }
      i = 0;
      for (l = lparams[lam]; l != NULL; l = cdr(l)) bind((int)car(l), aops[base + i++]);
      asp = base;
      E("continue;\n");
      if (curloop == curfun) usedwhile = 1;
      return -1;
    }
    if (lam) fo = vglobal[v] ? imm(0) : varop(v);
  }
  if (!lam) fo = stab(cg(f, C_VAL));
  base = cgargs(args, 0);
  storeargs(base, n);
  asp = base;
  callpre(ctx);
  if (lam) { E("F"); Ei(lam); E("((int)"); Eop(fo); E(", "); Ei(n); E(")"); }
  else { E("call("); Eop(fo); E(", "); Ei(n); E(")"); }
  return callpost(ctx);
}

int cgprim(long x, int ctx) {
  int r = (int)n1(x); long args = n2(x); int n = len(args); int base; int i; int t;
  base = cgargs(args, 0);
  if (pcname[r][0] == 'v') {   /* the calling convention of procedures */
    storeargs(base, n);
    asp = base;
    callpre(ctx);
    E(pcname[r]); E("(0, "); Ei(n); E(")");
    return callpost(ctx);
  }
  t = -1;
  if (ctx != C_EFF) { t = newtemp(); E("T"); Ei(t); E(" = "); }
  E(pcname[r]); E("(");
  for (i = 0; i < n; i++) { if (i) E(", "); Eop(aops[base + i]); }
  E(");\n");
  asp = base;
  if (t < 0) return -1;
  return deliver(mkop(O_T, t), ctx);
}

int cg(long x, int ctx) {
  int t = tag(x); int v; int o; int r; long l; int base; int i; int lam;
  if (t == X_CONST) { if (ctx == C_EFF) return -1; return deliver(constop(n1(x)), ctx); }
  if (t == X_REF) {
    v = (int)n1(x);
    if (vundef[v]) {
      E("unbound("); Ecstr(sptr(vname[v]), sl(vname[v])); E(");\n");
      return deliver(imm(NIL), ctx);
    }
    if (ctx == C_EFF) return -1;
    return deliver(varop(v), ctx);
  }
  if (t == X_SET) {
    v = (int)n1(x);
    o = cg(n2(x), C_VAL);
    Eop(varop(v)); E(" = "); Eop(o); E(";\n");
    if (ctx == C_EFF) return -1;
    return deliver(imm(NIL), ctx);
  }
  if (t == X_IF) {
    if (tag(n1(x)) == X_CONST) return cg(n1(n1(x)) != FALSE ? n2(x) : n3(x), ctx);
    o = cg(n1(x), C_VAL);
    E("if ("); Eop(o); E(" != 2) {\n");
    if (ctx == C_VAL) {
      r = newtemp();
      o = cg(n2(x), C_VAL); E("T"); Ei(r); E(" = "); Eop(o); E(";\n");
      E("} else {\n");
      o = cg(n3(x), C_VAL); E("T"); Ei(r); E(" = "); Eop(o); E(";\n");
      E("}\n");
      return mkop(O_T, r);
    }
    cg(n2(x), ctx);
    E("} else {\n");
    cg(n3(x), ctx);
    E("}\n");
    return -1;
  }
  if (t == X_SEQ) {
    for (l = n1(x); cdr(l) != NULL; l = cdr(l)) cg(car(l), C_EFF);
    return cg(car(l), ctx);
  }
  if (t == X_LAM) {
    lam = (int)n1(x);
    o = closure(lam);
    if (lfree[lam] != NULL) fillfree(lam, o);
    if (ctx == C_EFF) return -1;
    return deliver(o, ctx);
  }
  if (t == X_CALL) return cgcall(x, ctx);
  if (t == X_PRIM) return cgprim(x, ctx);
  if (t == X_LET) {
    base = cgargs(n2(x), 0);
    i = 0;
    for (l = n1(x); l != NULL; l = cdr(l)) bind((int)car(l), aops[base + i++]);
    asp = base;
    return cg(n3(x), ctx);
  }
  if (t == X_FIX) {
    if (lloop[n1(car(n2(x)))]) return cgloop(x, ctx);
    base = asp;
    for (l = n2(x); l != NULL; l = cdr(l)) aops[asp++] = closure((int)n1(car(l)));
    i = 0;
    for (l = n1(x); l != NULL; l = cdr(l)) bind((int)car(l), aops[base + i++]);
    i = 0;
    for (l = n2(x); l != NULL; l = cdr(l)) {
      lam = (int)n1(car(l));
      if (lfree[lam] != NULL) fillfree(lam, aops[base + i]);
      i++;
    }
    asp = base;
    return cg(n3(x), ctx);
  }
  die("scc: bad node", "");
  return -1;
}

/* one C function for lambda lam */
void genfun(int lam) {
  long l; int i; long d;
  curfun = lam; ntemps = 0; decls = NULL; usedwhile = 0; asp = 0;
  curloop = lvar[lam] && !lrest[lam] ? lam : 0;
  bn[B_PRO] = 0; bn[B_BODY] = 0;
  cur = B_PRO;
  if (lam) {
    if (lrest[lam]) { E("if (n < "); Ei(lnreq[lam]); E(") return arityerr(clo, n);\n"); }
    else { E("if (n != "); Ei(lnreq[lam]); E(") return arityerr(clo, n);\n"); }
  }
  i = 0;
  for (l = lfree[lam]; l != NULL; l = cdr(l)) {
    declare((int)car(l));
    E("L"); Ei(car(l)); E(" = *(long *)(clo + "); Ei(16 + 8 * i); E(");\n");
    i++;
  }
  i = 0;
  for (l = lparams[lam]; l != NULL; l = cdr(l)) {
    declare((int)car(l));
    E("L"); Ei(car(l)); E(" = ");
    if (iscell((int)car(l))) { E("newcell(stk["); Ei(i); E("]);\n"); }
    else { E("stk["); Ei(i); E("];\n"); }
    i++;
  }
  if (lrest[lam]) {
    declare(lrest[lam]);
    E("L"); Ei(lrest[lam]); E(" = ");
    if (iscell(lrest[lam])) { E("newcell(restlist("); Ei(lnreq[lam]); E(", n));\n"); }
    else { E("restlist("); Ei(lnreq[lam]); E(", n);\n"); }
  }
  cur = B_BODY;
  cg(lbody[lam], C_TAIL);
  cur = B_FUNS;
  E("\nint F"); Ei(lam); E("(int clo, int n) {");
  if (lname[lam] != FALSE) Ename(lname[lam]);
  E("\n");
  for (d = decls; d != NULL; d = cdr(d)) { E("long L"); Ei(car(d)); E(";"); Ename(vname[car(d)]); E("\n"); }
  for (i = 0; i < ntemps; i++) { E("long T"); Ei(i); E(";\n"); }
  Eb(bp[B_PRO], bn[B_PRO]);
  if (usedwhile) E("while (1) {\n");
  Eb(bp[B_BODY], bn[B_BODY]);
  if (usedwhile) E("}\n");
  E("}\n");
}

/* ----------------------------------------------------------- primitives */

void prims() {
  prim("+", 2, "p_add"); prim("+", -1, "v_add");
  prim("-", 2, "p_sub"); prim("-", 1, "p_neg"); prim("-", -1, "v_sub");
  prim("*", 2, "p_mul"); prim("*", -1, "v_mul");
  prim("/", 2, "p_div"); prim("/", -1, "v_div");
  prim("=", 2, "p_numeq"); prim("=", -1, "v_numeq");
  prim("<", 2, "p_lt"); prim("<", -1, "v_lt");
  prim(">", 2, "p_gt"); prim(">", -1, "v_gt");
  prim("<=", 2, "p_le"); prim("<=", -1, "v_le");
  prim(">=", 2, "p_ge"); prim(">=", -1, "v_ge");
  prim("max", 2, "p_max"); prim("max", -1, "v_max");
  prim("min", 2, "p_min"); prim("min", -1, "v_min");
  prim("gcd", 2, "p_gcd"); prim("gcd", -1, "v_gcd");
  prim("lcm", 2, "p_lcm"); prim("lcm", -1, "v_lcm");
  prim("zero?", 1, "p_zerop"); prim("positive?", 1, "p_positivep"); prim("negative?", 1, "p_negativep");
  prim("odd?", 1, "p_oddp"); prim("even?", 1, "p_evenp");
  prim("quotient", 2, "p_quotient"); prim("remainder", 2, "p_remainder"); prim("modulo", 2, "p_modulo");
  prim("truncate-quotient", 2, "p_quotient"); prim("truncate-remainder", 2, "p_remainder");
  prim("floor-remainder", 2, "p_modulo");
  prim("abs", 1, "p_abs"); prim("magnitude", 1, "p_abs");
  prim("number?", 1, "p_numberp"); prim("complex?", 1, "p_numberp"); prim("real?", 1, "p_numberp");
  prim("rational?", 1, "p_numberp"); prim("integer?", 1, "p_integerp");
  prim("exact?", 1, "p_exactp"); prim("inexact?", 1, "p_inexactp");
  prim("exact-integer?", 1, "p_exactintegerp"); prim("nan?", 1, "p_nanp");
  prim("exact", 1, "p_exact"); prim("inexact", 1, "p_inexact");
  prim("exact->inexact", 1, "p_inexact"); prim("inexact->exact", 1, "p_exact");
  prim("floor", 1, "p_floor"); prim("ceiling", 1, "p_ceiling");
  prim("truncate", 1, "p_truncate"); prim("round", 1, "p_round");
  prim("square", 1, "p_square"); prim("sqrt", 1, "p_sqrt"); prim("expt", 2, "p_expt");
  prim("exp", 1, "p_exp"); prim("log", 1, "p_log"); prim("log", 2, "p_log2");
  prim("number->string", 1, "p_number2string"); prim("number->string", 2, "p_number2string2");
  prim("string->number", 1, "p_string2number"); prim("string->number", 2, "p_string2number2");

  prim("not", 1, "p_not"); prim("boolean?", 1, "p_booleanp");
  prim("eq?", 2, "p_eqp"); prim("eqv?", 2, "p_eqvp"); prim("equal?", 2, "p_equalp");

  prim("cons", 2, "p_cons"); prim("car", 1, "p_car"); prim("cdr", 1, "p_cdr");
  prim("set-car!", 2, "p_setcar"); prim("set-cdr!", 2, "p_setcdr");
  prim("caar", 1, "p_caar"); prim("cadr", 1, "p_cadr"); prim("cdar", 1, "p_cdar"); prim("cddr", 1, "p_cddr");
prim("caaar", 1, "p_caaar"); prim("caadr", 1, "p_caadr");
  prim("cadar", 1, "p_cadar"); prim("cdaar", 1, "p_cdaar");
  prim("cdadr", 1, "p_cdadr"); prim("cddar", 1, "p_cddar");
  prim("caaaar", 1, "p_caaaar"); prim("caaadr", 1, "p_caaadr");
  prim("caadar", 1, "p_caadar"); prim("caaddr", 1, "p_caaddr");
  prim("cadaar", 1, "p_cadaar"); prim("cadadr", 1, "p_cadadr");
  prim("caddar", 1, "p_caddar"); prim("cdaaar", 1, "p_cdaaar");
  prim("cdaadr", 1, "p_cdaadr"); prim("cdadar", 1, "p_cdadar");
  prim("cdaddr", 1, "p_cdaddr"); prim("cddaar", 1, "p_cddaar");
  prim("cddadr", 1, "p_cddadr"); prim("cdddar", 1, "p_cdddar");
  prim("cddddr", 1, "p_cddddr");
  prim("caddr", 1, "p_caddr"); prim("cdddr", 1, "p_cdddr"); prim("cadddr", 1, "p_cadddr");
  prim("pair?", 1, "p_pairp"); prim("null?", 1, "p_nullp"); prim("list?", 1, "p_listp");
  prim("list", -1, "v_list"); prim("length", 1, "p_length"); prim("reverse", 1, "p_reverse");
  prim("append", 2, "p_append"); prim("append", -1, "v_append");
  prim("list-tail", 2, "p_listtail"); prim("list-ref", 2, "p_listref");
  prim("last-pair", 1, "p_lastpair"); prim("list-copy", 1, "p_listcopy");
  prim("make-list", 1, "p_makelist1"); prim("make-list", 2, "p_makelist");
  prim("memq", 2, "p_memq"); prim("memv", 2, "p_memq"); prim("member", 2, "p_member"); prim("member", 3, "v_member3");
  prim("assq", 2, "p_assq"); prim("assv", 2, "p_assq"); prim("assoc", 2, "p_assoc"); prim("assoc", 3, "v_assoc3");

  prim("symbol?", 1, "p_symbolp"); prim("symbol->string", 1, "p_symbol2string");
  prim("string->symbol", 1, "p_string2symbol"); prim("symbol=?", 2, "p_symboleq");

  prim("char?", 1, "p_charp"); prim("char->integer", 1, "p_char2int"); prim("integer->char", 1, "p_int2char");
  prim("char=?", 2, "p_chareq"); prim("char<?", 2, "p_charlt"); prim("char>?", 2, "p_chargt");
  prim("char<=?", 2, "p_charle"); prim("char>=?", 2, "p_charge"); prim("char-ci=?", 2, "p_charcieq");
  prim("char-upcase", 1, "p_charupcase"); prim("char-downcase", 1, "p_chardowncase");
  prim("char-alphabetic?", 1, "p_charalphap"); prim("char-numeric?", 1, "p_charnumericp");
  prim("char-whitespace?", 1, "p_charwhitespacep"); prim("char-upper-case?", 1, "p_charupperp");
  prim("char-lower-case?", 1, "p_charlowerp"); prim("digit-value", 1, "p_digitvalue");

  prim("string?", 1, "p_stringp"); prim("make-string", 1, "p_makestring1"); prim("make-string", 2, "p_makestring");
  prim("string", -1, "v_string"); prim("string-length", 1, "p_stringlength");
  prim("string-ref", 2, "p_stringref"); prim("string-set!", 3, "p_stringset");
  prim("substring", 3, "p_substring"); prim("string-copy", 1, "p_stringcopy");
  prim("string-copy", 2, "p_substring2"); prim("string-copy", 3, "p_substring");
  prim("string-append", 2, "p_stringappend"); prim("string-append", -1, "v_stringappend");
  prim("string->list", 1, "p_string2list"); prim("list->string", 1, "p_list2string");
  prim("string=?", 2, "p_streq"); prim("string<?", 2, "p_strlt"); prim("string>?", 2, "p_strgt");
  prim("string<=?", 2, "p_strle"); prim("string>=?", 2, "p_strge");
  prim("string-fill!", 2, "p_stringfill");
  prim("string-upcase", 1, "p_stringupcase"); prim("string-downcase", 1, "p_stringdowncase");
  prim("string-for-each", -1, "v_stringforeach");

  prim("vector?", 1, "p_vectorp"); prim("make-vector", 1, "p_makevector1"); prim("make-vector", 2, "p_makevector");
  prim("vector", -1, "v_vector"); prim("vector-length", 1, "p_vectorlength");
  prim("vector-ref", 2, "p_vectorref"); prim("vector-set!", 3, "p_vectorset");
  prim("vector->list", 1, "p_vector2list"); prim("list->vector", 1, "p_list2vector");
  prim("vector-fill!", 2, "p_vectorfill"); prim("vector-copy", 1, "p_vectorcopy");
  prim("vector-map", -1, "v_vectormap"); prim("vector-for-each", -1, "v_vectorforeach");

  prim("procedure?", 1, "p_procedurep");
  prim("apply", -1, "v_apply"); prim("map", -1, "v_map"); prim("for-each", -1, "v_foreach");
  prim("values", -1, "v_values"); prim("call-with-values", -1, "v_callwithvalues");
  prim("call-with-current-continuation", -1, "v_callcc"); prim("call/cc", -1, "v_callcc");
  prim("dynamic-wind", -1, "v_dynamicwind");
  prim("force", -1, "v_force"); prim("promise?", 1, "p_promisep");
  prim("make-parameter", -1, "v_makeparameter");
  prim("raise", -1, "v_raise"); prim("raise-continuable", -1, "v_raisecontinuable");
  prim("error", -1, "v_error"); prim("with-exception-handler", -1, "v_withexceptionhandler");
  prim("error-object?", 1, "p_errorobjectp"); prim("error-object-message", 1, "p_errorobjectmessage");
  prim("error-object-irritants", 1, "p_errorobjectirritants");
  prim("exit", -1, "v_exit"); prim("emergency-exit", -1, "v_exit");

  prim("display", 1, "p_display"); prim("display", 2, "p_display2");
  prim("write", 1, "p_write"); prim("write", 2, "p_write2");
  prim("write-shared", 1, "p_write"); prim("write-simple", 1, "p_write");
  prim("newline", 0, "p_newline"); prim("newline", 1, "p_newline1");
  prim("write-char", 1, "p_writechar"); prim("write-char", 2, "p_writechar2");
  prim("write-string", 1, "p_writestring"); prim("write-string", 2, "p_writestring2");
  prim("current-output-port", 0, "p_currentoutputport"); prim("current-error-port", 0, "p_currenterrorport");
  prim("current-input-port", 0, "p_currentinputport");
  prim("flush-output-port", 0, "p_flushoutputport"); prim("flush-output-port", 1, "p_flushoutputport1");
  prim("open-output-string", 0, "p_openoutputstring"); prim("get-output-string", 1, "p_getoutputstring");
  prim("open-input-string", 1, "p_openinputstring");
  prim("read-char", 0, "p_readchar"); prim("read-char", 1, "p_readchar1");
  prim("peek-char", 0, "p_peekchar"); prim("peek-char", 1, "p_peekchar1");
  prim("read-line", 0, "p_readline"); prim("read-line", 1, "p_readline1");
  prim("read", 0, "p_read"); prim("char-ready?", 0, "p_charreadyp");
  prim("eof-object", 0, "p_eofobject"); prim("eof-object?", 1, "p_eofobjectp");

  /* used by the expansions of derived forms */
  prim("%make-record-type", 2, "p_mkrtd"); prim("%record-alloc", 1, "p_recalloc");
  prim("%record?", 2, "p_recp"); prim("%record-ref", 3, "p_recref"); prim("%record-set!", 4, "p_recset");
  prim("%make-promise", 2, "p_makepromise"); prim("%unspecified", 0, "p_unspecified");
  prim("%guard", -1, "v_guard"); prim("%parameterize", -1, "v_parameterize");
}

/* ------------------------------------------------------------------ main */

void wrapper(int v) {   /* a primitive as a procedure */
  int r; int any = 0; int i;
  for (r = 0; r < nprims; r++) if (pname[r] == vprim[v] && pnargs[r] < 0) {
    cur = B_INIT; E("  G"); Ei(v); E(" = mkclo("); E(pcname[r]); E(", 0);\n");
    return;
  }
  cur = B_FUNS;
  E("\nint W"); Ei(v); E("(int clo, int n) {\n");
  for (r = 0; r < nprims; r++) if (pname[r] == vprim[v] && pcname[r][0] == 'v') {
    E("if (n == "); Ei(pnargs[r]); E(") return "); E(pcname[r]); E("(clo, n);\n");
  }
  for (r = 0; r < nprims; r++) if (pname[r] == vprim[v] && pcname[r][0] != 'v') {
    E("if (n == "); Ei(pnargs[r]); E(") { stk[0] = "); E(pcname[r]); E("(");
    for (i = 0; i < pnargs[r]; i++) { if (i) E(", "); E("stk["); Ei(i); E("]"); }
    E("); return 1; }\n");
  }
  E("return arityerr(clo, n);\n}\n");
  cur = B_INIT; E("  G"); Ei(v); E(" = mkclo(W"); Ei(v); E(", 0);\n");
}

void initsyms() {
  S_lambda = sym("lambda"); S_define = sym("define"); S_set = sym("set!"); S_if = sym("if");
  S_begin = sym("begin"); S_let = sym("let"); S_letstar = sym("let*"); S_letrec = sym("letrec");
  S_letrecstar = sym("letrec*"); S_cond = sym("cond"); S_case = sym("case"); S_and = sym("and");
  S_or = sym("or"); S_when = sym("when"); S_unless = sym("unless"); S_do = sym("do");
  S_else = sym("else"); S_arrow = sym("=>"); S_delay = sym("delay"); S_delayforce = sym("delay-force");
  S_defrecord = sym("define-record-type"); S_letvalues = sym("let-values");
  S_letstarvalues = sym("let*-values"); S_defvalues = sym("define-values"); S_receive = sym("receive");
  S_caselambda = sym("case-lambda"); S_import = sym("import"); S_defsyntax = sym("define-syntax");
  S_letsyntax = sym("let-syntax"); S_letrecsyntax = sym("letrec-syntax");
  S_syntaxrules = sym("syntax-rules"); S_ellipsis = sym("..."); S_underscore = sym("_");
  S_guard = sym("guard"); S_parameterize = sym("parameterize"); S_definelibrary = sym("define-library");
  MFAIL = cons(NIL, NIL);
  hexd[0] = 0;
}

int main() {
  long forms = NULL; long x; int v; int l; long d;
  rt_init(); read_init(); initsyms(); prims();
  lparams[0] = NULL; lfree[0] = NULL; lname[0] = FALSE;
  memcopy(hexd, "0123456789abcdef", 16);
  while ((x = readdatum()) != EOFV) forms = cons(x, forms);
  toplevel(rev(forms));
  for (v = 1; v < nvars; v++) if (known(v)) lvar[vlam[v]] = v;
  findloops(lbody[0]);
  for (l = 1; l < nlams; l++) ;
  ana(lbody[0], 0);
  for (l = 0; l < nlams; l++) if (!lloop[l]) genfun(l);
  for (v = 1; v < nvars; v++) if (vglobal[v] && vprim[v] && vnref[v]) wrapper(v);
  /* the program: globals, constants, prototypes, functions, initialization */
  cur = B_PRO; bn[B_PRO] = 0;
  E("/* generated by scc */\n");
  for (v = 1; v < nvars; v++) if (vglobal[v]) { E("long G"); Ei(v); E(";"); Ename(vname[v]); E("\n"); }
  E("long K["); Ei(nk + 1); E("];\n");
  for (l = 1; l < nlams; l++) if (!lloop[l]) { E("int F"); Ei(l); E("(int clo, int n);\n"); }
  for (v = 1; v < nvars; v++) if (vglobal[v] && vprim[v] && vnref[v]) { E("int W"); Ei(v); E("(int clo, int n);\n"); }
  outb(bp[B_PRO], bn[B_PRO]);
  outb(bp[B_FUNS], bn[B_FUNS]);
  outs("\nvoid sc_init() {\n");
  outb(bp[B_INIT], bn[B_INIT]);
  outs("}\n");
  flush();
  return 0;
}
