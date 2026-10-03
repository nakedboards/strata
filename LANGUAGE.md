# Strata -- language reference

A line-oriented language typed into a terminal. Case doesn't matter. `;` separates multiple commands
on one line; `#` starts a comment that runs to the end of the line (but only when preceded by
whitespace or at the start of the line -- see the note on sharps below).

This document describes the **core language**, which is the same regardless of which instrument it's
driving. The specific parameter names available (`wave`, `cutoff`, `lfo.rate`, whatever your project
registers) depend entirely on the `Host` implementation -- type `params` at the terminal to list them
for whatever you're connected to.

## Model

* A **step** is 1/16 of a beat (4 steps per beat). `tempo 120` -> a step is 125 ms.
* A pattern is a sequence of items; **one item = one step**. Patterns loop.
* There is one melodic track (`play`), driven by the always-present `note` target, plus any number of
  numeric "lanes" (`lane <parameter> ...`) for whatever your host exposes.
* **Taking a parameter from its knob.** Setting a parameter from code takes it away from any physical
  control the host has for it (if any). `free <parameter>` hands it back explicitly; on the reference
  hardware implementation, turning the physical control also hands it back automatically once it has
  moved noticeably.

## Pattern mini-notation

| Token | Meaning |
|---|---|
| `c3` `d#4` `eb2` `a` | a note (C4 = 60; no octave = 3) |
| `~` or `.` | rest |
| `_` | tie: extends the previous note by one step (top level only) |
| `[c3 e3 g3]` | subdivide: all items share one step |
| `c3*2` | repeat within the step |
| `<c3 e3 g3>` | alternate: advances to the next option on every pass through the pattern |
| `(c3\|e3\|g3)` | random choice, re-rolled every time |
| `c3?` `c3?25` | 50% / 25% chance |
| `0.5` `.25` `3` | plain numbers (for `lane` patterns) |

Patterns normally hold only notes and numbers. The one exception: a bare word (e.g. `sine`) is
allowed **inside an inline pattern given directly to `lane <parameter> "..."` or `play "..."`**,
where the target parameter is already known, so the word resolves against that parameter's own
named values with no ambiguity -- `lane wave "morph land tri"` and `lane lfo.wave "sine tri saw
pulse rand"` both work, and the same word ("tri") can mean a different number for each, since each
resolves through its own parameter's vocabulary rather than a shared global one. Words are **not**
allowed in a **named** pattern (`name = ...`), since a named pattern isn't tied to any one parameter
at definition time -- it could later be attached to a different lane with a different vocabulary, or
none at all. Use plain numbers there, or just write the pattern inline instead of naming it.

## Commands

```
name = <pattern>               define/redefine a pattern (up to 16 names, <=8 chars each)
play name | mod | mod ...      melody (pitch + gate)
play "c3 e3 g3"                 inline pattern
lane <parameter> NAME|"..." | ...   modulate any parameter the host exposes
<parameter> VALUE               direct set, e.g. "wave morph", "attack 5"
hit c4 [ms]                     one-shot note
stop [target|all]   free [target|all]   hush   ls   del NAME   status   help   echo on|off
params                          list the parameters this instrument exposes
```

**Modifiers** (after `|`): `rev` * `fast N` * `slow N` (1..8) * `tr N` (semitones) * `oct N` *
`q <scale> [tonic]` (`major minor dorian phrygian lydian mixo harmin penta minpenta blues whole
chrom`). `tr`/`oct`/`q` only apply to the `note` track.

**Direct settings:** `tempo N` * `quant N` * `root NOTE` * `gate F` (note length as a fraction of the
step, 0.02..1) * `seed N` (reset any host-side randomness).

Command arguments accept **arithmetic**: `+ - * /`, unary +/-, parentheses -- e.g. `tempo 60*2`,
`bind pitch lfo (0.1+0.05)` (if your host's `ext()` defines a `bind` command; arithmetic works the
same way in any command argument). This is **not** available inside pattern text, where `*`, `-` and
`()` already mean repeat/sign/random-choice -- `0.5*2` inside a pattern means "the value 0.5,
repeated twice within its step", not "multiply by 2".

A `#` starts a comment only when preceded by whitespace or at the start of a line -- this is so a
sharp inside an unquoted pattern definition (`mel = (g#3)`) is never mistaken for a comment.

## Examples

```
tempo 90
mel = a2 _ c3 e3 <a3 g3> ~ (e3|d3) [c3 e3]
play mel | q minor a
lane sustain "1 .6 .8 .5" | slow 2
```

Live editing:
```
mel = a2 c3 e3 g3 a3 g3 e3 c3      # takes effect on the next step
play mel | rev | tr 12 | fast 2    # takes effect on the next bar
free all                           # hand everything back to physical controls
```

Generative:
```
play "(c3|e3|g3)*2 ~ (a3|b3)? [c3 d3]?30" | q penta c
lane gate "(.2|.5|.9)"
```
