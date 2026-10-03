// Strata -- a small live-coding language for text-sequenced, monophonic instruments.
// Core has no Arduino dependency and no knowledge of any specific instrument: it only knows how
// to sequence a melodic "note" track plus any number of host-named numeric "parameter" lanes.
// A host (one per instrument/project) implements Host below and tells the language, at boot,
// which named parameters exist for that particular device -- the language itself never hardcodes
// a parameter list. See README.md for the language reference and an example of writing a Host.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace strata {

static const int MAX_PARAMS = 24;  // upper bound on how many named parameters a Host may expose

// Sequencer-internal track indices. T_NOTE (pitch+gate) and T_GATE (note-length fraction within a
// step) are universal sequencer concepts and always exist. Everything else is a host-defined
// parameter, addressed as T_PARAM0 + i for i in [0, host.paramCount()).
enum : uint8_t { T_NOTE = 0, T_GATE = 1, T_PARAM0 = 2 };
static const int T_COUNT = T_PARAM0 + MAX_PARAMS;

// Optional named values for a parameter (e.g. parameter "wave" might accept the words "sine"/"saw"
// as well as plain numbers). Returned by Host::paramValues(); a list is terminated by {nullptr,0}.
struct NamedValue { const char* name; float value; };

// Everything the language needs from the instrument it drives. One Host per project/instrument;
// the language core (this file + strata.cpp) never changes between projects.
struct Host {
  virtual void gate(bool on) = 0;
  virtual void pitch(float midi) = 0;        // fractional MIDI note, C4 = 60

  // Named parameter lanes, declared by the host at boot. "i" is 0-based, stable for the process
  // lifetime. setParam() should clamp out-of-range values rather than reject them (the language
  // has no per-parameter range knowledge). releaseParam() hands control back to a physical
  // control if the host has one for that parameter (release(-1) means "release everything").
  virtual int paramCount() const = 0;
  virtual const char* paramName(int i) const = 0;
  virtual void setParam(int i, float v) = 0;
  virtual void releaseParam(int i) = 0;
  // Optional: named words accepted for parameter i (e.g. "sine", "saw"), in addition to plain
  // numbers. Default: no named values. Returns a pointer to a NamedValue[] terminated by {nullptr,0}.
  virtual const NamedValue* paramValues(int i) const { (void)i; return nullptr; }

  virtual void reseed(uint32_t seed) = 0;    // reset any host-side randomness (e.g. "seed N")
  virtual void print(const char* s) = 0;
  // Host-specific commands that don't fit the "<name> <value>" shape (e.g. multi-argument
  // shorthands, read-only queries). Tried after the language's own commands. Return nullptr on
  // success, or an error string.
  virtual const char* ext(int argc, const char* const* argv) { (void)argc; (void)argv; return "unknown command (help)"; }
  virtual void status() {}                   // appended to the built-in "status" command's output
  virtual ~Host() {}
};

// Small calculator for numeric COMMAND arguments: + - * / (usual precedence), unary +/-, parens.
// E.g. "tempo 60*2", "bind pitch lfo (0.1+0.05)". NOT used inside pattern text ("mel = ...") --
// there *, - and () already mean repeat/sign/random-choice, and mixing in arithmetic would be
// ambiguous (is "0.5*2" the number 1.0, or the value 0.5 repeated twice?).
bool evalExpr(const char* s, float& out);

// ---- Internal pattern representation (static memory, no malloc/new) ----
enum : uint8_t { N_REST, N_TIE, N_VAL, N_SEQ, N_ALT, N_CHOICE };

struct Node {
  uint8_t type, repeat, prob, pad;
  int16_t first, next;
  float val;
};

static const int MAX_NODES = 64;    // nodes per pattern
static const int MAX_NAMED = 16;    // named patterns
static const int MAX_SLOTS = MAX_NAMED + T_COUNT;  // + one anonymous (inline) slot per track
static const int MAX_QUEUE = 64;
static const int MAX_EV = 16;

struct Pattern {
  bool used;
  char name[10];
  uint8_t nnodes;
  int16_t root;
  Node n[MAX_NODES];
};

struct Xform {
  bool rev;
  uint8_t fast, slow;
  uint8_t nops;
  struct Op { uint8_t kind; int8_t a, b; } ops[4];  // kind 0: tr(a)  kind 1: quantize(scale a, root b)
};

struct Track {
  int8_t slot;
  Xform xf;
  bool pend;
  int8_t pslot;
  Xform pxf;
  uint32_t start;
};

struct Sched {
  bool used;
  uint8_t target;   // a track index, or S_GATEOFF (see strata.cpp)
  uint32_t at;
  float val;
};

struct Tok;

class Interpreter {
 public:
  explicit Interpreter(Host& h);
  void begin();                    // banner + reset to initial state
  void feed(char c);               // character-at-a-time input from UART/USB
  void exec(const char* line);     // run one line (multiple commands allowed via ';')
  void tick(uint32_t nowUs);       // call as often as possible from loop()

 private:
  Host& h_;
  Pattern pats_[MAX_SLOTS];
  Track tr_[T_COUNT];
  Sched q_[MAX_QUEUE];
  char lb_[160];
  int ll_;
  bool echo_;
  bool started_, gateOn_;
  float tempo_, stepUs_, gateFrac_;
  int quant_, rootPc_;
  uint32_t nextStepUs_, now_, g_, rng_, drops_;

  uint32_t rnd();
  void out(const char* s);
  void outf(const char* fmt, ...);
  void execSeg(char* seg);
  const char* cmd(char* seg);
  const char* define(const char* name, const char* src);
  const char* doPlay(int target, const Tok* toks, int nt, int i);
  int findSlot(const char* name) const;
  int findTarget(const char* name) const;     // "note"/"gate"/host-registered parameter name -> track index, -1 if none
  void runStep(uint32_t g, uint32_t stepStart);
  void schedule(int t, uint32_t g, uint32_t stepStart);
  void push(uint8_t target, uint32_t at, float val);
  void purge(uint8_t target);
  void apply(uint8_t target, float v);
  void drain();
  void stopTrack(int t);
  void status();
  void help();
};

}  // namespace strata
