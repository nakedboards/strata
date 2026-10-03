#include "strata.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace strata {

struct Tok { const char* s; bool q; };

namespace {

const uint8_t S_GATEOFF = 250;  // internal pseudo-target for a scheduled "note off" event
// Minimum gap between a note-off and the next note-on, so a gate-triggered envelope on the host
// side has time to see gate=LOW and retrigger cleanly instead of missing the edge.
const uint32_t MIN_GATE_GAP_US = 1000;

struct ScaleDef { const char* n; int8_t d[12]; };
const ScaleDef SCALES[] = {
  {"major",    {0, 2, 4, 5, 7, 9, 11, -1}},
  {"minor",    {0, 2, 3, 5, 7, 8, 10, -1}},
  {"dorian",   {0, 2, 3, 5, 7, 9, 10, -1}},
  {"phrygian", {0, 1, 3, 5, 7, 8, 10, -1}},
  {"lydian",   {0, 2, 4, 6, 7, 9, 11, -1}},
  {"mixo",     {0, 2, 4, 5, 7, 9, 10, -1}},
  {"harmin",   {0, 2, 3, 5, 7, 8, 11, -1}},
  {"penta",    {0, 2, 4, 7, 9, -1}},
  {"minpenta", {0, 3, 5, 7, 10, -1}},
  {"blues",    {0, 3, 5, 6, 7, 10, -1}},
  {"whole",    {0, 2, 4, 6, 8, 10, -1}},
  {"chrom",    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};
const int NSCALES = sizeof(SCALES) / sizeof(SCALES[0]);

// Core language keywords that can never be used as a pattern name. Host-registered parameter
// names are ALSO reserved -- checked dynamically in isReserved(), since the core doesn't know
// them at compile time.
const char* const RESERVED_CORE[] = {"tempo", "bpm", "quant", "root", "gate", "seed", "hit", "play",
                                      "lane", "stop", "hush", "panic", "ls", "del", "status", "help",
                                      "echo", "free", "note", "rev", "fast", "slow", "tr", "oct", "q",
                                      "params"};

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { return c >= 'a' && c <= 'z'; }
inline bool isSpace(char c) { return c == ' ' || c == '\t'; }

void fmtF(char* b, size_t n, float v) {
  bool neg = v < 0;
  if (neg) v = -v;
  int i = (int)v;
  int f = (int)((v - i) * 100 + 0.5f);
  if (f >= 100) { i++; f -= 100; }
  snprintf(b, n, "%s%d.%02d", neg ? "-" : "", i, f);
}

// ------------------------------------------------------------------ calculator for COMMAND arguments
// + - * / (usual precedence), unary +/-, parens. Whitespace between tokens is skipped.
// NOT used inside pattern text -- see the comment on evalExpr()'s declaration in strata.h.
struct ExprParser {
  const char* s;
  bool err = false;
  void ws() { while (*s == ' ' || *s == '\t') s++; }
  float number() {
    ws();
    char* e;
    float v = strtof(s, &e);
    if (e == s) { err = true; return 0; }
    s = e;
    return v;
  }
  float factor() {
    ws();
    if (*s == '-') { s++; return -factor(); }
    if (*s == '+') { s++; return factor(); }
    if (*s == '(') {
      s++;
      float v = expr();
      ws();
      if (*s != ')') { err = true; return v; }
      s++;
      return v;
    }
    return number();
  }
  float term() {
    float v = factor();
    for (;;) {
      ws();
      if (*s == '*') { s++; v *= factor(); }
      else if (*s == '/') { s++; float d = factor(); if (err) return v; if (d == 0) { err = true; return v; } v /= d; }
      else break;
      if (err) return v;
    }
    return v;
  }
  float expr() {
    float v = term();
    for (;;) {
      ws();
      if (*s == '+') { s++; v += term(); }
      else if (*s == '-') { s++; v -= term(); }
      else break;
      if (err) return v;
    }
    return v;
  }
};
}  // namespace

bool evalExpr(const char* s, float& out) {
  ExprParser p; p.s = s;
  float v = p.expr();
  p.ws();
  if (p.err || *p.s != 0) return false;
  out = v;
  return true;
}

namespace {
bool parseNum(const char* s, float& v) { return evalExpr(s, v); }

// A note like c4, d#3, eb2, a (octave defaults to defOct). C4 = 60.
bool parseNote(const char*& s, int defOct, float& out) {
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  if (!isAlpha(*s) || *s > 'g') return false;
  int pc = base[*s - 'a'];
  s++;
  if (*s == '#') { pc++; s++; }
  else if (*s == 'b') { pc--; s++; }
  int oct = defOct;
  if (isDigit(*s)) {
    oct = *s++ - '0';
    if (isDigit(*s)) oct = oct * 10 + (*s++ - '0');
  }
  int m = 12 * (oct + 1) + pc;
  if (m < 0 || m > 127) return false;
  out = (float)m;
  return true;
}

// ---------------------------------------------------------------- pattern parser
struct Parser {
  const char* s;
  Pattern* p;
  const char* err;
  int defOct;
  // Optional: named values for whichever single parameter this pattern is being compiled for (see
  // the comment on the word-handling branch in parseItem() below). Null if none apply.
  const NamedValue* values;

  int alloc(uint8_t type) {
    if (p->nnodes >= MAX_NODES) { err = "pattern too long"; return -1; }
    Node& n = p->n[p->nnodes];
    n.type = type; n.repeat = 1; n.prob = 100; n.pad = 0; n.first = -1; n.next = -1; n.val = 0;
    return p->nnodes++;
  }
  void ws() { while (isSpace(*s)) s++; }
  static bool isEnd(char c) { return c == 0 || c == ']' || c == '>' || c == ')' || c == '|'; }

  int readInt() {
    int v = 0;
    while (isDigit(*s)) { v = v * 10 + (*s - '0'); s++; if (v > 1000) v = 1000; }
    return v;
  }

  // A list of items up to a closing delimiter. -1 = empty, otherwise the head node index.
  int parseList() {
    int head = -1, tail = -1;
    for (;;) {
      ws();
      if (isEnd(*s)) break;
      int it = parseItem();
      if (err) return -1;
      if (head < 0) head = it; else p->n[tail].next = it;
      tail = it;
    }
    return head;
  }

  int parseItem() {
    int id = -1;
    char c = *s;
    if (c == '~' || (c == '.' && !isDigit(s[1]))) { s++; id = alloc(N_REST); }
    else if (c == '_') { s++; id = alloc(N_TIE); }
    else if (c == '[') {
      s++;
      int head = parseList();
      if (err) return -1;
      if (*s != ']') { err = "missing ]"; return -1; }
      if (head < 0) { err = "empty []"; return -1; }
      s++;
      id = alloc(N_SEQ);
      if (id < 0) return -1;
      p->n[id].first = head;
    } else if (c == '<') {
      s++;
      int head = parseList();
      if (err) return -1;
      if (*s != '>') { err = "missing >"; return -1; }
      if (head < 0) { err = "empty <>"; return -1; }
      s++;
      id = alloc(N_ALT);
      if (id < 0) return -1;
      p->n[id].first = head;
    } else if (c == '(') {
      s++;
      int head = -1, tail = -1;
      for (;;) {
        int opt = parseList();
        if (err) return -1;
        if (opt < 0) { err = "empty choice"; return -1; }
        if (p->n[opt].next >= 0) {  // multiple items in one option -- wrap them in a SEQ
          int w = alloc(N_SEQ);
          if (w < 0) return -1;
          p->n[w].first = opt;
          opt = w;
        }
        if (head < 0) head = opt; else p->n[tail].next = opt;
        tail = opt;
        if (*s == '|') { s++; continue; }
        break;
      }
      if (*s != ')') { err = "missing )"; return -1; }
      s++;
      id = alloc(N_CHOICE);
      if (id < 0) return -1;
      p->n[id].first = head;
    } else if (isDigit(c) || c == '-' || (c == '.' && isDigit(s[1]))) {
      char* e;
      float v = strtof(s, &e);
      if (e == s) { err = "bad number"; return -1; }
      s = e;
      id = alloc(N_VAL);
      if (id < 0) return -1;
      p->n[id].val = v;
    } else if (isAlpha(c) && c <= 'g') {
      float m;
      if (!parseNote(s, defOct, m)) { err = "bad note"; return -1; }
      id = alloc(N_VAL);
      if (id < 0) return -1;
      p->n[id].val = m;
    } else if (isAlpha(c) && values) {
      // A bare word, e.g. "sine" -- resolved against the NamedValue table for the ONE parameter
      // this pattern is being compiled for (passed in as `values`). This is only possible when the
      // target is already known at compile time, i.e. for an inline pattern given directly to
      // "lane <parameter> \"...\"" or "play \"...\"" -- never for a named pattern ("name = ..."),
      // since that could later be attached to a different parameter with a different vocabulary
      // (or none at all). There's no shared/global word namespace: the same word resolves through
      // whichever parameter's own table is in play, so "sine" can mean different numbers for two
      // different parameters with no ambiguity.
      char w[16];
      int n = 0;
      while ((isAlpha(*s) || *s == '.') && n < 15) w[n++] = *s++;
      w[n] = 0;
      int k = -1;
      for (int i = 0; values[i].name; i++) if (!strcmp(values[i].name, w)) { k = i; break; }
      if (k < 0) { err = "unknown word for this parameter (see 'params' or the host's docs)"; return -1; }
      id = alloc(N_VAL);
      if (id < 0) return -1;
      p->n[id].val = values[k].value;
    } else {
      err = values ? "unexpected char in pattern (only notes, numbers and this parameter's named values are allowed here)"
                   : "unexpected char in pattern (only notes and numbers are allowed here -- this parameter has no named values, or this pattern isn't attached to one yet; see LANGUAGE.md)";
      return -1;
    }
    if (id < 0) return -1;
    for (;;) {
      if (*s == '*') {
        s++;
        int k = readInt();
        if (k < 1 || k > 16) { err = "*n: 1..16"; return -1; }
        p->n[id].repeat = (uint8_t)k;
      } else if (*s == '?') {
        s++;
        int pr = isDigit(*s) ? readInt() : 50;
        p->n[id].prob = (uint8_t)(pr > 100 ? 100 : pr);
      } else break;
    }
    if (!(isSpace(*s) || isEnd(*s))) { err = "bad token"; return -1; }
    return id;
  }
};

bool compile(const char* src, Pattern& out, const char** err, const NamedValue* values = nullptr) {
  Parser ps;
  ps.s = src; ps.p = &out; ps.err = nullptr; ps.defOct = 3; ps.values = values;
  out.nnodes = 0; out.root = -1;
  int root = ps.alloc(N_SEQ);
  int head = ps.parseList();
  if (!ps.err && *ps.s) ps.err = "unbalanced bracket";
  if (!ps.err && head < 0) ps.err = "empty pattern";
  if (ps.err) { *err = ps.err; return false; }
  out.n[root].first = head;
  out.root = (int16_t)root;
  return true;
}

int childCount(const Pattern& p, int ni) {
  int k = 0;
  for (int c = p.n[ni].first; c >= 0; c = p.n[c].next) k++;
  return k;
}
int nthChild(const Pattern& p, int ni, int idx) {
  int c = p.n[ni].first;
  while (idx-- > 0 && c >= 0) c = p.n[c].next;
  return c;
}

struct Ev { float t, len, val; };

struct EmitCtx {
  const Pattern* p;
  uint32_t cycle;
  uint32_t* rng;
  Ev* ev;
  int n;
  uint32_t rnd() { uint32_t s = *rng; s ^= s << 13; s ^= s >> 17; s ^= s << 5; *rng = s; return s; }
};

void emit(EmitCtx& c, int ni, float t0, float dur) {
  const Node& nd = c.p->n[ni];
  if (nd.prob < 100 && (int)(c.rnd() % 100) >= nd.prob) return;
  int rep = nd.repeat;
  float d = dur / rep;
  for (int j = 0; j < rep; j++) {
    float t = t0 + j * d;
    switch (nd.type) {
      case N_VAL:
        if (c.n < MAX_EV) { c.ev[c.n].t = t; c.ev[c.n].len = d; c.ev[c.n].val = nd.val; c.n++; }
        break;
      case N_SEQ: {
        int k = childCount(*c.p, ni);
        float dd = d / k;
        int i = 0;
        for (int ch = nd.first; ch >= 0; ch = c.p->n[ch].next, i++) emit(c, ch, t + i * dd, dd);
        break;
      }
      case N_ALT: {
        int k = childCount(*c.p, ni);
        emit(c, nthChild(*c.p, ni, (int)(c.cycle % k)), t, d);
        break;
      }
      case N_CHOICE: {
        int k = childCount(*c.p, ni);
        emit(c, nthChild(*c.p, ni, (int)(c.rnd() % k)), t, d);
        break;
      }
      default: break;  // REST, TIE
    }
  }
}

float quantize(float m, int scale, int root) {
  int n = (int)(m + 0.5f);
  int rel = ((n - root) % 12 + 12) % 12;
  const ScaleDef& sc = SCALES[scale];
  bool allowed[12] = {false};
  for (int i = 0; i < 12 && sc.d[i] >= 0; i++) allowed[sc.d[i]] = true;
  for (int d = 0; d <= 6; d++) {
    if (allowed[((rel - d) % 12 + 12) % 12]) return (float)(n - d);
    if (allowed[(rel + d) % 12]) return (float)(n + d);
  }
  return (float)n;
}

void initXform(Xform& x) { x.rev = false; x.fast = 1; x.slow = 1; x.nops = 0; }

}  // namespace

// =================================================================== Interpreter

Interpreter::Interpreter(Host& h) : h_(h) { begin(); }

void Interpreter::begin() {
  for (int i = 0; i < MAX_SLOTS; i++) pats_[i].used = false;
  for (int t = 0; t < T_COUNT; t++) { tr_[t].slot = -1; tr_[t].pend = false; tr_[t].pslot = -1; initXform(tr_[t].xf); initXform(tr_[t].pxf); tr_[t].start = 0; }
  for (int i = 0; i < MAX_QUEUE; i++) q_[i].used = false;
  ll_ = 0; echo_ = false; started_ = false; gateOn_ = false;
  tempo_ = 120; stepUs_ = 60e6f / (tempo_ * 4); gateFrac_ = 0.5f;
  quant_ = 16; rootPc_ = 0;
  nextStepUs_ = 0; now_ = 0; g_ = 0; rng_ = 2463534242u; drops_ = 0;
}

uint32_t Interpreter::rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }

void Interpreter::out(const char* s) { h_.print(s); }
void Interpreter::outf(const char* fmt, ...) {
  char b[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  h_.print(b);
}

// ------------------------------------------------------------------ input
void Interpreter::feed(char c) {
  if (c == '\r' || c == '\n') {
    if (echo_) out("\r\n");
    if (ll_ > 0) { lb_[ll_] = 0; ll_ = 0; exec(lb_); }
    return;
  }
  if (c == 8 || c == 127) {
    if (ll_ > 0) { ll_--; if (echo_) out("\b \b"); }
    return;
  }
  if (c < 32) return;
  if (ll_ < (int)sizeof(lb_) - 1) {
    lb_[ll_++] = c;
    if (echo_) { char b[2] = {c, 0}; out(b); }
  }
}

void Interpreter::exec(const char* line) {
  char seg[160];
  int n = 0;
  bool inq = false;
  for (const char* p = line;; p++) {
    char c = *p;
    if (c == '"') inq = !inq;
    // '#' starts a comment only if preceded by whitespace or the start of the line -- otherwise
    // it would collide with a sharp in a note (c#3, g#4...) inside an UNQUOTED pattern definition:
    // "mel = (g#3)" would otherwise be truncated right at the sharp, as if a comment followed.
    bool hashIsComment = c == '#' && (p == line || p[-1] == ' ' || p[-1] == '\t');
    bool endSeg = (c == 0) || (!inq && (c == ';' || hashIsComment));
    if (endSeg) {
      seg[n] = 0;
      execSeg(seg);
      n = 0;
      if (c == 0 || c == '#') break;
      continue;
    }
    if (n < (int)sizeof(seg) - 1) seg[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
}

void Interpreter::execSeg(char* seg) {
  char* p = seg;
  while (isSpace(*p)) p++;
  if (!*p) return;
  const char* e = nullptr;
  // pattern definition:  name = ...
  char* ns = p;
  while (isAlpha(*p) || isDigit(*p) || *p == '_') p++;
  if (p > ns) {
    char* ne = p;
    while (isSpace(*p)) p++;
    if (*p == '=') {
      char name[10];
      size_t L = ne - ns;
      if (L > 8) { out("err: name max 8 chars\r\n"); return; }
      memcpy(name, ns, L); name[L] = 0;
      char* src = p + 1;
      while (isSpace(*src)) src++;
      size_t sl = strlen(src);
      if (sl >= 2 && src[0] == '"' && src[sl - 1] == '"') { src[sl - 1] = 0; src++; }
      e = define(name, src);
      if (e) outf("err: %s\r\n", e);
      return;
    }
  }
  e = cmd(seg);
  if (e) outf("err: %s\r\n", e);
}

int Interpreter::findSlot(const char* name) const {
  for (int i = 0; i < MAX_NAMED; i++) if (pats_[i].used && !strcmp(pats_[i].name, name)) return i;
  return -1;
}

int Interpreter::findTarget(const char* name) const {
  if (!strcmp(name, "note")) return T_NOTE;
  if (!strcmp(name, "gate")) return T_GATE;
  int n = h_.paramCount();
  if (n > MAX_PARAMS) n = MAX_PARAMS;
  for (int i = 0; i < n; i++) if (!strcmp(h_.paramName(i), name)) return T_PARAM0 + i;
  return -1;
}

const char* Interpreter::define(const char* name, const char* src) {
  if (!isAlpha(name[0])) return "name must start with a letter";
  for (size_t i = 0; i < sizeof(RESERVED_CORE) / sizeof(RESERVED_CORE[0]); i++)
    if (!strcmp(RESERVED_CORE[i], name)) return "reserved word";
  if (findTarget(name) >= 0) return "reserved word (a parameter has this name)";
  Pattern tmp;
  const char* err = nullptr;
  if (!compile(src, tmp, &err)) return err;
  int slot = findSlot(name);
  if (slot < 0) {
    for (int i = 0; i < MAX_NAMED; i++) if (!pats_[i].used) { slot = i; break; }
    if (slot < 0) return "no free pattern slots (del something)";
  }
  tmp.used = true;
  strncpy(tmp.name, name, sizeof(tmp.name) - 1);
  tmp.name[sizeof(tmp.name) - 1] = 0;
  pats_[slot] = tmp;
  outf("%s = %d steps\r\n", name, childCount(tmp, tmp.root));
  return nullptr;
}

// ------------------------------------------------------------------ play / lane
const char* Interpreter::doPlay(int target, const Tok* t, int nt, int i) {
  if (i >= nt) return "need pattern name or \"pattern\"";
  int slot;
  if (t[i].q) {
    Pattern tmp;
    const char* err = nullptr;
    // Named values are only offered here: this inline pattern is being attached directly to a known
    // target, so words in it (e.g. "sine") can be resolved against THIS parameter's own vocabulary.
    const NamedValue* values = (target >= T_PARAM0) ? h_.paramValues(target - T_PARAM0) : nullptr;
    if (!compile(t[i].s, tmp, &err, values)) return err;
    slot = MAX_NAMED + target;
    tmp.used = true; tmp.name[0] = 0;
    pats_[slot] = tmp;
  } else {
    slot = findSlot(t[i].s);
    if (slot < 0) return "no such pattern";
  }
  i++;
  Xform x;
  initXform(x);
  while (i < nt) {
    const char* w = t[i].s;
    if (!strcmp(w, "|")) { i++; continue; }
    if (!strcmp(w, "rev")) { x.rev = true; i++; continue; }
    if (!strcmp(w, "tr") || !strcmp(w, "oct") || !strcmp(w, "fast") || !strcmp(w, "slow")) {
      float v;
      if (i + 1 >= nt || !parseNum(t[i + 1].s, v)) return "expected number";
      if (w[0] == 't' || w[0] == 'o') {
        if (target != T_NOTE) return "tr/oct/q only for the note track";
        if (x.nops >= 4) return "too many tr/q";
        if (w[0] == 'o') v *= 12;
        if (v < -60 || v > 60) return "tr: -60..60";
        x.ops[x.nops].kind = 0; x.ops[x.nops].a = (int8_t)v; x.nops++;
      } else {
        if (v < 1 || v > 8) return "fast/slow: 1..8";
        if (w[0] == 'f') x.fast = (uint8_t)v; else x.slow = (uint8_t)v;
      }
      i += 2;
      continue;
    }
    if (!strcmp(w, "q")) {
      if (target != T_NOTE) return "tr/oct/q only for the note track";
      if (i + 1 >= nt) return "q: scale name";
      int sc = -1;
      for (int k = 0; k < NSCALES; k++) if (!strcmp(SCALES[k].n, t[i + 1].s)) sc = k;
      if (sc < 0) return "unknown scale (major minor dorian phrygian lydian mixo harmin penta minpenta blues whole chrom)";
      int root = rootPc_;
      i += 2;
      if (i < nt && strcmp(t[i].s, "|")) {
        float m; const char* ns = t[i].s;
        if (parseNote(ns, 3, m) && *ns == 0) { root = ((int)m) % 12; i++; }
      }
      if (x.nops >= 4) return "too many tr/q";
      x.ops[x.nops].kind = 1; x.ops[x.nops].a = (int8_t)sc; x.ops[x.nops].b = (int8_t)root; x.nops++;
      continue;
    }
    return "unknown modifier (rev tr oct fast slow q)";
  }
  if (x.fast > 1 && x.slow > 1) return "use either fast or slow";
  tr_[target].pend = true;
  tr_[target].pslot = (int8_t)slot;
  tr_[target].pxf = x;
  return nullptr;
}

void Interpreter::stopTrack(int t) {
  tr_[t].slot = -1;
  tr_[t].pend = false;
  if (t == T_NOTE) { purge(T_NOTE); purge(S_GATEOFF); apply(S_GATEOFF, 0); }
  else if (t == T_GATE) { /* nothing to purge: T_GATE is applied synchronously, never queued */ }
  else { purge((uint8_t)t); h_.releaseParam(t - T_PARAM0); }
}

// ------------------------------------------------------------------ commands
const char* Interpreter::cmd(char* seg) {
  Tok tk[12];
  char pool[176];
  int nt = 0, pl = 0;
  static const char PIPE[] = "|";
  char* p = seg;
  while (*p && nt < 12) {
    while (isSpace(*p)) p++;
    if (!*p) break;
    if (*p == '"') {
      p++;
      tk[nt].s = pool + pl; tk[nt].q = true;
      while (*p && *p != '"' && pl < (int)sizeof(pool) - 1) pool[pl++] = *p++;
      pool[pl++] = 0;
      if (*p == '"') p++;
      nt++;
    } else if (*p == '|') {
      tk[nt].s = PIPE; tk[nt].q = false; nt++; p++;
    } else {
      tk[nt].s = pool + pl; tk[nt].q = false;
      while (*p && !isSpace(*p) && *p != '|' && *p != '"' && pl < (int)sizeof(pool) - 1) pool[pl++] = *p++;
      pool[pl++] = 0;
      nt++;
    }
  }
  if (nt == 0) return nullptr;
  const char* c = tk[0].s;
  float v;
  auto arg = [&](int i, float& out_) { return i < nt && parseNum(tk[i].s, out_); };

  if (!strcmp(c, "tempo") || !strcmp(c, "bpm")) {
    if (!arg(1, v) || v < 20 || v > 400) return "tempo 20..400";
    tempo_ = v; stepUs_ = 60e6f / (tempo_ * 4);
    return nullptr;
  }
  if (!strcmp(c, "quant")) {
    if (!arg(1, v) || v < 0 || v > 64) return "quant 0..64 (steps)";
    quant_ = (int)v; return nullptr;
  }
  if (!strcmp(c, "root")) {
    if (nt < 2) return "root NOTE";
    const char* s = tk[1].s; float m;
    if (!parseNote(s, 3, m) || *s) return "bad note";
    rootPc_ = ((int)m) % 12; return nullptr;
  }
  if (!strcmp(c, "gate"))  { if (!arg(1, v) || v < 0.02f || v > 1) return "gate 0.02..1"; gateFrac_ = v > 0.98f ? 0.98f : v; return nullptr; }
  if (!strcmp(c, "seed"))  { if (!arg(1, v)) return "seed N"; rng_ = (uint32_t)v * 2654435761u + 1; h_.reseed((uint32_t)v); return nullptr; }
  if (!strcmp(c, "free")) {
    if (nt < 2 || !strcmp(tk[1].s, "all")) {
      stopTrack(T_NOTE); stopTrack(T_GATE);
      for (int i = 0; i < h_.paramCount() && i < MAX_PARAMS; i++) stopTrack(T_PARAM0 + i);
      return nullptr;
    }
    int t = findTarget(tk[1].s);
    if (t < 0) return "free [all|note|gate|<parameter>]  (see 'params' for the list)";
    stopTrack(t);
    return nullptr;
  }
  if (!strcmp(c, "hit")) {
    if (nt < 2) return "hit NOTE [ms]";
    const char* s = tk[1].s; float m;
    if (!parseNote(s, 3, m) || *s) return "bad note";
    float ms = 200;
    if (nt >= 3 && (!arg(2, ms) || ms < 1)) return "bad ms";
    purge(T_NOTE); purge(S_GATEOFF);   // a stale pending gate-off must not cut the new note short
    apply(T_NOTE, m);
    push(S_GATEOFF, now_ + (uint32_t)(ms * 1000), 0);
    return nullptr;
  }
  if (!strcmp(c, "play")) return doPlay(T_NOTE, tk, nt, 1);
  if (!strcmp(c, "lane")) {
    if (nt < 3) return "lane <parameter> NAME|\"pattern\" [| mods]  (see 'params' for the list)";
    int t = findTarget(tk[1].s);
    if (t <= 0) return "unknown parameter (see 'params' for the list)";
    return doPlay(t, tk, nt, 2);
  }
  if (!strcmp(c, "stop")) {
    if (nt < 2 || !strcmp(tk[1].s, "all")) { for (int t = 0; t < T_COUNT; t++) stopTrack(t); return nullptr; }
    int t = findTarget(tk[1].s);
    if (t < 0) return "stop [all|note|gate|<parameter>]  (see 'params' for the list)";
    stopTrack(t); return nullptr;
  }
  if (!strcmp(c, "hush") || !strcmp(c, "panic")) {
    for (int t = 0; t < T_COUNT; t++) stopTrack(t);
    for (int i = 0; i < MAX_QUEUE; i++) q_[i].used = false;
    h_.gate(false); gateOn_ = false;
    return nullptr;
  }
  if (!strcmp(c, "ls")) {
    for (int i = 0; i < MAX_NAMED; i++) if (pats_[i].used) outf("  %s (%d)\r\n", pats_[i].name, childCount(pats_[i], pats_[i].root));
    return nullptr;
  }
  if (!strcmp(c, "del")) {
    if (nt < 2) return "del NAME";
    int s = findSlot(tk[1].s);
    if (s < 0) return "no such pattern";
    for (int t = 0; t < T_COUNT; t++) if (tr_[t].slot == s || (tr_[t].pend && tr_[t].pslot == s)) return "pattern is in use -- stop it first";
    pats_[s].used = false; return nullptr;
  }
  if (!strcmp(c, "echo")) { echo_ = (nt >= 2 && !strcmp(tk[1].s, "on")); return nullptr; }
  if (!strcmp(c, "status")) { status(); return nullptr; }
  if (!strcmp(c, "params")) {
    int n = h_.paramCount();
    if (n > MAX_PARAMS) n = MAX_PARAMS;
    for (int i = 0; i < n; i++) outf("  %s\r\n", h_.paramName(i));
    return nullptr;
  }
  if (!strcmp(c, "help")) { help(); return nullptr; }

  // Not a core command: is it a single-value "<parameter> <value>" shorthand? Tried before ext()
  // so that any host parameter can be set with no per-parameter code in the core. Falls through to
  // ext() if the word isn't a known parameter, or if the value doesn't parse as a number or a
  // registered named value (letting the host handle multi-word phrases like "elem in").
  {
    int t = findTarget(c);
    if (t >= T_PARAM0 && nt >= 2) {
      int pi = t - T_PARAM0;
      const NamedValue* nv = h_.paramValues(pi);
      if (nv) for (int k = 0; nv[k].name; k++) if (!strcmp(nv[k].name, tk[1].s)) { h_.setParam(pi, nv[k].value); return nullptr; }
      if (parseNum(tk[1].s, v)) { h_.setParam(pi, v); return nullptr; }
    }
  }
  {
    const char* av[12];
    for (int i = 0; i < nt; i++) av[i] = tk[i].s;
    return h_.ext(nt, av);
  }
}

void Interpreter::status() {
  char b[16], g[16];
  fmtF(b, sizeof b, tempo_); fmtF(g, sizeof g, gateFrac_);
  outf("tempo %s  quant %d  gate %s  root %d  step %d\r\n", b, quant_, g, rootPc_, (int)g_);
  auto printTrack = [&](int t, const char* name) {
    const Track& k = tr_[t];
    if (k.slot < 0 && !k.pend) return;
    const Xform& x = k.xf;
    outf("  %-10s %s%s%s fast%d slow%d%s\r\n", name, k.slot >= 0 ? (pats_[k.slot].name[0] ? pats_[k.slot].name : "\"inline\"") : "-",
         x.rev ? " rev" : "", x.nops ? " +ops" : "", x.fast, x.slow, k.pend ? "  [queued]" : "");
  };
  printTrack(T_NOTE, "note");
  printTrack(T_GATE, "gate");
  int n = h_.paramCount();
  if (n > MAX_PARAMS) n = MAX_PARAMS;
  for (int i = 0; i < n; i++) printTrack(T_PARAM0 + i, h_.paramName(i));
  if (drops_) outf("  dropped events: %d\r\n", (int)drops_);
  h_.status();
}

void Interpreter::help() {
  out("strata -- a live-coding language for text-sequenced instruments\r\n"
      "  a = c3 e3 g3 ~ [c4 e4]*2 <g3 b3> (c3|e3) d3?   define a pattern (1 item = 1 step)\r\n"
      "  play a | rev | tr 12 | fast 2 | q minor d       melody (applied at the next bar)\r\n"
      "  play \"c3 e3 g3\"                                inline pattern\r\n"
      "  lane <parameter> NAME|\"...\" | mods              modulate any parameter the host exposes\r\n"
      "  params                                          list the parameters this instrument exposes\r\n"
      "  tempo N  quant N  root NOTE  gate F  seed N\r\n"
      "  <parameter> VALUE                                direct set, e.g. \"wave morph\", \"attack 5\"\r\n"
      "  command args accept arithmetic: + - * / and parens, e.g. tempo 60*2\r\n"
      "  hit NOTE [ms]   stop [target]   free [target]   hush   ls   del NAME   status   echo on|off\r\n"
      "  setting a parameter from code takes it away from its physical control, if it has one;\r\n"
      "  'free'/'stop' hand it back (moving the control by hand does too)\r\n"
      "  ~ . rest   _ tie   [..] subdivide   <..> alternate   (a|b) random   *n repeat   ?[p] chance\r\n");
}

// ------------------------------------------------------------------ sequencer
void Interpreter::push(uint8_t target, uint32_t at, float val) {
  for (int i = 0; i < MAX_QUEUE; i++) if (!q_[i].used) { q_[i].used = true; q_[i].target = target; q_[i].at = at; q_[i].val = val; return; }
  drops_++;
}

void Interpreter::purge(uint8_t target) {
  for (int i = 0; i < MAX_QUEUE; i++) if (q_[i].used && q_[i].target == target) q_[i].used = false;
}

void Interpreter::apply(uint8_t target, float v) {
  if (target == T_NOTE) {
    if (gateOn_) h_.gate(false);  // retrigger the envelope
    h_.pitch(v); h_.gate(true); gateOn_ = true;
    return;
  }
  if (target == S_GATEOFF) { h_.gate(false); gateOn_ = false; return; }
  if (target >= T_PARAM0) { int i = target - T_PARAM0; if (i < h_.paramCount() && i < MAX_PARAMS) h_.setParam(i, v); }
}

void Interpreter::drain() {
  for (;;) {
    int best = -1;
    int32_t bestAge = -1;
    for (int i = 0; i < MAX_QUEUE; i++) {
      if (!q_[i].used) continue;
      int32_t age = (int32_t)(now_ - q_[i].at);
      if (age >= 0 && age > bestAge) { bestAge = age; best = i; }
    }
    if (best < 0) return;
    q_[best].used = false;
    apply(q_[best].target, q_[best].val);
  }
}

void Interpreter::tick(uint32_t now) {
  now_ = now;
  if (!started_) { started_ = true; nextStepUs_ = now; g_ = 0; }
  int guard = 0;
  while ((int32_t)(now - nextStepUs_) >= 0) {
    if (++guard > 64) { nextStepUs_ = now; break; }
    runStep(g_, nextStepUs_);
    g_++;
    nextStepUs_ += (uint32_t)stepUs_;
  }
  drain();
}

void Interpreter::runStep(uint32_t g, uint32_t stepStart) {
  for (int t = 0; t < T_COUNT; t++) {
    Track& k = tr_[t];
    if (k.pend && (quant_ <= 1 || g % (uint32_t)quant_ == 0)) {
      k.slot = k.pslot; k.xf = k.pxf; k.start = g; k.pend = false;
    }
  }
  // T_GATE first (it only affects how the SAME step's note events are phrased, read inline below),
  // then every parameter lane, then T_NOTE last (so gate-length and parameter changes for this step
  // are already in effect by the time the note is scheduled).
  if (tr_[T_GATE].slot >= 0) schedule(T_GATE, g, stepStart);
  int n = h_.paramCount();
  if (n > MAX_PARAMS) n = MAX_PARAMS;
  for (int i = 0; i < n; i++) { int t = T_PARAM0 + i; if (tr_[t].slot >= 0) schedule(t, g, stepStart); }
  if (tr_[T_NOTE].slot >= 0) schedule(T_NOTE, g, stepStart);
}

void Interpreter::schedule(int t, uint32_t g, uint32_t stepStart) {
  Track& k = tr_[t];
  const Pattern& p = pats_[k.slot];
  if (!p.used) return;
  const int len = childCount(p, p.root);
  if (len == 0) return;
  const Xform& x = k.xf;
  const uint32_t ls = g - k.start;
  Ev ev[MAX_EV];
  EmitCtx c;
  c.p = &p; c.rng = &rng_; c.ev = ev; c.n = 0; c.cycle = 0;

  auto mapIdx = [&](uint32_t pstep) { int i = (int)(pstep % len); return x.rev ? len - 1 - i : i; };

  if (x.fast > 1) {
    for (int j = 0; j < x.fast; j++) {
      uint32_t ps = ls * x.fast + j;
      c.cycle = ps / len;
      emit(c, nthChild(p, p.root, mapIdx(ps)), (float)j / x.fast, 1.0f / x.fast);
    }
  } else {
    if (ls % x.slow != 0) return;
    uint32_t ps = ls / x.slow;
    c.cycle = ps / len;
    emit(c, nthChild(p, p.root, mapIdx(ps)), 0, (float)x.slow);
    if (t == T_NOTE && c.n > 0) {  // a tie ("_") extends the step's last note
      int ties = 0;
      for (int i = 1; i < len; i++) {
        if (p.n[nthChild(p, p.root, mapIdx(ps + i))].type == N_TIE) ties++; else break;
      }
      ev[c.n - 1].len += ties * (float)x.slow;
    }
  }
  if (c.n == 0) return;

  if (t == T_GATE) { float v = ev[0].val; gateFrac_ = v < 0.02f ? 0.02f : v > 0.98f ? 0.98f : v; return; }

  for (int i = 0; i < c.n; i++) {
    uint32_t on = stepStart + (uint32_t)(ev[i].t * stepUs_);
    float v = ev[i].val;
    if (t == T_NOTE) {
      for (int o = 0; o < x.nops; o++) {
        if (x.ops[o].kind == 0) v += x.ops[o].a;
        else v = quantize(v, x.ops[o].a, x.ops[o].b);
      }
      v = v < 0 ? 0 : v > 127 ? 127 : v;   // generic MIDI float range; the host clamps to its own range in pitch()
      uint32_t off = on + (uint32_t)(ev[i].len * gateFrac_ * stepUs_);
      if (i + 1 < c.n) {
        uint32_t nextOn = stepStart + (uint32_t)(ev[i + 1].t * stepUs_);
        if ((int32_t)(off - (nextOn - MIN_GATE_GAP_US)) > 0) off = nextOn - MIN_GATE_GAP_US;
      }
      if ((int32_t)(off - (on + MIN_GATE_GAP_US / 2)) < 0) off = on + MIN_GATE_GAP_US / 2;
      push(T_NOTE, on, v);
      push(S_GATEOFF, off, 0);
    } else {
      push((uint8_t)t, on, v);
    }
  }
}

}  // namespace strata
